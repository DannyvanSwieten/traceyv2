// Solid store for the top-level DOP graph. Single global root — no host_uid
// like the VOP store needs, no path navigation like the SOP store needs
// (no subnets yet on the DOP side). Mirrors stores/sops.ts's debounced
// push pattern.
//
// External `dop_graph_changed` broadcasts (e.g. after a server-side reset)
// trigger a reload, but only when no local push is pending so the
// in-flight edit isn't clobbered.

import { createSignal } from 'solid-js';
import * as api from '../lib/api';
import {
  DopGraph,
  DopNode,
  DopConnection,
  ParamValue,
  emptyGraph,
  syncNextUid,
} from '../lib/dop_graph';

const [graph, setGraphInternal] = createSignal<DopGraph>(emptyGraph());
const [selectedNodeIds, setSelectedNodeIdsInternal] = createSignal<number[]>([]);
const [primarySelectedId, setPrimarySelectedId] = createSignal<number | null>(null);
// Cache-status indicator for the dopesheet bar. Driven by the backend's
// `dop_status` broadcast event.
const [cachedToFrameInternal, setCachedToFrame] = createSignal(0);

export const dopGraph = graph;
export const selectedNode = primarySelectedId;
export const selectedNodes = selectedNodeIds;
export const cachedToFrame = cachedToFrameInternal;

export function setSelectedNode(uid: number | null): void {
  if (uid === null) {
    setSelectedNodeIdsInternal([]);
    setPrimarySelectedId(null);
    return;
  }
  setSelectedNodeIdsInternal([uid]);
  setPrimarySelectedId(uid);
}

export function setSelectedNodes(uids: number[]): void {
  const seen = new Set<number>();
  const ordered: number[] = [];
  for (const u of uids) {
    if (!seen.has(u)) { seen.add(u); ordered.push(u); }
  }
  setSelectedNodeIdsInternal(ordered);
  setPrimarySelectedId(ordered.length > 0 ? ordered[ordered.length - 1] : null);
}

export function toggleSelectedNode(uid: number): void {
  const cur = selectedNodeIds();
  const idx = cur.indexOf(uid);
  if (idx >= 0) {
    const next = cur.slice();
    next.splice(idx, 1);
    setSelectedNodeIdsInternal(next);
    setPrimarySelectedId(next.length > 0 ? next[next.length - 1] : null);
  } else {
    const next = [...cur, uid];
    setSelectedNodeIdsInternal(next);
    setPrimarySelectedId(uid);
  }
}

export function isNodeSelected(uid: number): boolean {
  return selectedNodeIds().includes(uid);
}

let pushTimer: ReturnType<typeof setTimeout> | null = null;
let dirty = false;
const PUSH_DEBOUNCE_MS = 300;

function schedulePush(): void {
  dirty = true;
  if (pushTimer !== null) clearTimeout(pushTimer);
  pushTimer = setTimeout(async () => {
    pushTimer = null;
    if (!dirty) return;
    dirty = false;
    try {
      await api.send<null>('set_dop_graph', {
        graph: JSON.stringify(graph()),
      });
    } catch (e) {
      const msg = e instanceof Error ? e.message : JSON.stringify(e);
      console.error('Failed to push DOP graph:', msg);
    }
  }, PUSH_DEBOUNCE_MS);
}

export async function loadDopGraphFromEngine(): Promise<void> {
  try {
    const json = await api.send<string>('get_dop_graph', {});
    if (!json) {
      setGraphInternal(emptyGraph());
      return;
    }
    const parsed = JSON.parse(json) as DopGraph;
    for (const n of parsed.nodes) syncNextUid(n.uid);
    setGraphInternal(parsed);
  } catch (e) {
    console.error('Failed to load DOP graph:', e);
    setGraphInternal(emptyGraph());
  }
}

export async function resetDopCache(): Promise<void> {
  try {
    await api.send<null>('dop_reset_cache', {});
  } catch (e) {
    console.error('dop_reset_cache failed:', e);
  }
}

// Reload on external server-side change. Skip when a local push is in
// flight so we don't drop an unflushed edit.
api.listen('dop_graph_changed', () => {
  if (pushTimer !== null || dirty) return;
  loadDopGraphFromEngine().catch((e) =>
    console.warn('dop reload after dop_graph_changed failed:', e),
  );
});

// Cache-status indicator updates. Backend broadcasts this whenever the
// cook extends the cache or a reset wipes it.
api.listen('dop_status', (msg: unknown) => {
  const data = msg as { cached_to_frame?: number } | undefined;
  if (typeof data?.cached_to_frame === 'number') {
    setCachedToFrame(data.cached_to_frame);
  }
});

api.listen('dop_cache_reset', () => {
  setCachedToFrame(0);
});

// ── Mutators ──────────────────────────────────────────────────────────────

export function addNode(node: DopNode): void {
  setGraphInternal((g) => ({ ...g, nodes: [...g.nodes, node] }));
  schedulePush();
}

export function removeNode(uid: number): void {
  setGraphInternal((g) => {
    // Passthrough bridge: connect upstream of (uid, port 0) to whatever was
    // downstream of (uid, port 0). Same pattern as the SOP / VOP store so
    // mid-chain deletes don't break the wire chain.
    const upstream = g.connections.find(
      (c) => c.to_node === uid && c.to_port === 0,
    );
    const remaining = g.connections.filter(
      (c) => c.from_node !== uid && c.to_node !== uid,
    );
    const bridged: DopConnection[] = [];
    if (upstream) {
      for (const c of g.connections) {
        if (c.from_node === uid && c.from_port === 0) {
          bridged.push({
            from_node: upstream.from_node,
            from_port: upstream.from_port,
            to_node: c.to_node,
            to_port: c.to_port,
          });
        }
      }
    }
    return {
      ...g,
      nodes: g.nodes.filter((n) => n.uid !== uid),
      connections: [...remaining, ...bridged],
    };
  });
  schedulePush();
}

export function moveNode(uid: number, x: number, y: number): void {
  setGraphInternal((g) => ({
    ...g,
    nodes: g.nodes.map((n) => (n.uid === uid ? { ...n, pos: [x, y] } : n)),
  }));
  schedulePush();
}

export function setParam(uid: number, paramName: string, value: ParamValue): void {
  setGraphInternal((g) => ({
    ...g,
    nodes: g.nodes.map((n) => {
      if (n.uid !== uid) return n;
      const prev = n.params[paramName];
      const channels =
        prev && prev.type !== 'string' && prev.channels !== undefined
          ? prev.channels
          : undefined;
      const next: ParamValue =
        channels !== undefined && value.type !== 'string'
          ? { ...value, channels }
          : value;
      return { ...n, params: { ...n.params, [paramName]: next } };
    }),
  }));
  schedulePush();
}

export function addConnection(c: DopConnection): void {
  setGraphInternal((g) => {
    // Replace any existing connection landing on the same (to_node, to_port)
    // — one input port can only carry one wire.
    const filtered = g.connections.filter(
      (x) => !(x.to_node === c.to_node && x.to_port === c.to_port),
    );
    return { ...g, connections: [...filtered, c] };
  });
  schedulePush();
}

export function removeConnection(c: DopConnection): void {
  setGraphInternal((g) => ({
    ...g,
    connections: g.connections.filter(
      (x) =>
        !(
          x.from_node === c.from_node &&
          x.from_port === c.from_port &&
          x.to_node === c.to_node &&
          x.to_port === c.to_port
        ),
    ),
  }));
  schedulePush();
}
