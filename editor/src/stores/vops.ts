// Solid store for the active attribute_vop SOP's child VopGraph. Mirrors
// stores/sops.ts but scoped to ONE host SOP node at a time (the modal pattern
// only ever has one VOP editor open). Switching hosts swaps the loaded
// graph and resets the selection.
//
// The store does NOT handle the "currentPath" subnet navigation that the SOP
// store does — VOPs don't nest in v1. If VOP-of-VOP composition lands later,
// reuse the SOP store's path-resolution machinery.

import { createSignal } from 'solid-js';
import * as api from '../lib/api';
import {
  VopGraph,
  VopNode,
  VopConnection,
  ParamValue,
  InputDefault,
  emptyGraph,
  syncNextUid,
  makeNode,
  lookupCatalog,
} from '../lib/vop_graph';

const [graph, setGraphInternal] = createSignal<VopGraph>(emptyGraph());
const [currentHostUidInternal, setCurrentHostUid] = createSignal<number | null>(null);
// Multi-selection model mirroring stores/sops.ts. `selectedNodes` is the
// ordered list (click order); `selectedNode` returns the most recently added
// for legacy callers (inspector, single-node keyboard shortcuts).
const [selectedNodeIds, setSelectedNodeIdsInternal] = createSignal<number[]>([]);
const [primarySelectedId, setPrimarySelectedId] = createSignal<number | null>(null);
const [editorOpenInternal, setEditorOpenInternal] = createSignal(false);

export const vopGraph = graph;
export const currentHostUid = currentHostUidInternal;
export const selectedNode = primarySelectedId;
export const selectedNodes = selectedNodeIds;
export const isVopEditorOpen = editorOpenInternal;

// Drill into the VOP graph of a specific attribute_vop SOP host. The dock
// (SopGraphEditor) watches `isVopEditorOpen()` and swaps its body to the
// VOP panel while leaving the SOP breadcrumb above intact, so the user can
// click back out via the breadcrumb. We kick off the graph load here rather
// than from a parent component so callers only need this one entry point.
export function openVopEditor(hostUid: number): void {
  setCurrentHostUid(hostUid);
  setEditorOpenInternal(true);
  loadVopGraphFromEngine(hostUid).catch((e) =>
    console.error('failed to load VOP graph for host', hostUid, e),
  );
}
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
  // Dedup while preserving the caller's order (so the last entry stays the
  // primary). Used by marquee-select to push a result set into the store.
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
// `cookOnNextPush` toggles on whenever ANY semantic edit (param, connection,
// add/remove) has happened since the last push. Position-only moves leave it
// false, so dragging a node around the canvas pushes the new layout to the
// native side (it persists into save_scene) but doesn't force the worker
// thread to re-cook a graph that produces the same output. Coalescing means
// one cook=true edit anywhere in the debounce window keeps cook=true for
// the whole batch — correct: any semantic change should re-cook.
let cookOnNextPush = false;
// True debounce, not a throttle: every edit restarts the timer so a typing
// burst coalesces into ONE push once activity stops. Mirrors sops.ts.
const PUSH_DEBOUNCE_MS = 300;

function schedulePush(cook: boolean = true): void {
  // No host attached → nothing to push (e.g. modal closed mid-edit).
  if (currentHostUidInternal() === null) return;
  dirty = true;
  if (cook) cookOnNextPush = true;
  if (pushTimer !== null) clearTimeout(pushTimer);
  pushTimer = setTimeout(async () => {
    pushTimer = null;
    if (!dirty) return;
    dirty = false;
    const willCook = cookOnNextPush;
    cookOnNextPush = false;
    const host = currentHostUidInternal();
    if (host === null) return;
    try {
      await api.send<null>('set_vop_graph', {
        host_uid: host,
        graph: JSON.stringify(graph()),
        cook: willCook,
      });
    } catch (e) {
      const msg = e instanceof Error ? e.message : JSON.stringify(e);
      console.error('Failed to push VOP graph:', msg);
    }
  }, PUSH_DEBOUNCE_MS);
}

// Open the editor on a specific host SOP. Pulls the host's child VopGraph
// over IPC and seats it in the store. Subsequent mutations push back via the
// same host_uid until closeVopEditor() resets it.
//
// Only clear the selection when the host actually changes (drilling into a
// different attribute_vop). Same-host reloads — which happen after every
// push because the engine broadcasts `sop_graph_changed` — must preserve
// the selected node so the inspector doesn't reset to its empty state mid
// keystroke.
export async function loadVopGraphFromEngine(hostUid: number): Promise<void> {
  const hostChanged = currentHostUidInternal() !== hostUid;
  setCurrentHostUid(hostUid);
  if (hostChanged) setSelectedNode(null);
  try {
    const json = await api.send<string>('get_vop_graph', { host_uid: hostUid });
    if (!json) {
      setGraphInternal(emptyGraph());
      return;
    }
    const parsed = JSON.parse(json) as VopGraph;
    for (const n of parsed.nodes) syncNextUid(n.uid);
    setGraphInternal(parsed);
  } catch (e) {
    console.error('Failed to load VOP graph:', e);
    setGraphInternal(emptyGraph());
  }
}

// Closing the modal: flush any pending push, drop the host id, reset state.
export async function closeVopEditor(): Promise<void> {
  if (pushTimer !== null) {
    clearTimeout(pushTimer);
    pushTimer = null;
  }
  if (dirty) {
    dirty = false;
    const host = currentHostUidInternal();
    if (host !== null) {
      try {
        await api.send<null>('set_vop_graph', {
          host_uid: host,
          graph: JSON.stringify(graph()),
        });
      } catch (e) {
        console.error('Failed to flush VOP graph:', e);
      }
    }
  }
  setEditorOpenInternal(false);
  setCurrentHostUid(null);
  setSelectedNode(null);
}

// ── Promote / Demote (Houdini-style "promote up to host") ──────────────────
// Pulls a VOP-side knob up to a first-class param on the host attribute_vop
// SOP node so it can be animated through the existing SOP keyframe path.
// The server picks the host param name (auto-generated, unique) and returns
// it. We flush any pending VOP push first so the promotion sees the latest
// param state.

export async function promoteParamToHost(
  vopNodeUid: number,
  paramName: string,
): Promise<string | null> {
  const host = currentHostUidInternal();
  if (host === null) return null;
  await flushPendingPush();
  try {
    const result = await api.send<{ host_param_name: string }>('vop_promote_param', {
      host_uid: host,
      vop_node_uid: vopNodeUid,
      param_name: paramName,
    });
    return result.host_param_name;
  } catch (e) {
    console.error('vop_promote_param failed:', e);
    return null;
  }
}

export async function demoteHostParam(hostParamName: string): Promise<boolean> {
  const host = currentHostUidInternal();
  if (host === null) return false;
  await flushPendingPush();
  try {
    return await api.send<boolean>('vop_demote_param', {
      host_uid: host,
      host_param_name: hostParamName,
    });
  } catch (e) {
    console.error('vop_demote_param failed:', e);
    return false;
  }
}

// Internal: drain a pending debounced push synchronously so promote/demote
// land against the latest state. Mirrors closeVopEditor's flush.
async function flushPendingPush(): Promise<void> {
  if (pushTimer !== null) {
    clearTimeout(pushTimer);
    pushTimer = null;
  }
  if (!dirty) return;
  dirty = false;
  const host = currentHostUidInternal();
  if (host === null) return;
  try {
    await api.send<null>('set_vop_graph', {
      host_uid: host,
      graph: JSON.stringify(graph()),
    });
  } catch (e) {
    console.error('Failed to flush VOP graph before promote/demote:', e);
  }
}

// Reload when the host signals an external change (cook completion mostly).
// Skip if a local push is pending so the in-flight edit isn't clobbered.
api.listen('sop_graph_changed', () => {
  if (pushTimer !== null || dirty) return;
  const host = currentHostUidInternal();
  if (host === null) return;
  loadVopGraphFromEngine(host).catch((e) =>
    console.warn('vop reload after sop_graph_changed failed:', e),
  );
});

// ── Mutators ──────────────────────────────────────────────────────────────

export function addNode(node: VopNode): void {
  setGraphInternal((g) => ({ ...g, nodes: [...g.nodes, node] }));
  schedulePush();
}

export function removeNode(uid: number): void {
  setGraphInternal((g) => {
    // Houdini-style passthrough: bridge to_port=0 → from_port=0 so a node
    // removed mid-chain leaves its neighbours connected.
    const upstream = g.connections.find(
      (c) => c.to_node === uid && c.to_port === 0,
    );
    const remaining = g.connections.filter(
      (c) => c.from_node !== uid && c.to_node !== uid,
    );
    const bridged: VopConnection[] = [];
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

// In-place node-kind swap. Mirrors stores/sops.ts:replaceNode.
export function replaceNode(uid: number, newKind: string): void {
  const entry = lookupCatalog(newKind);
  if (!entry) return;
  setGraphInternal((g) => {
    const oldNode = g.nodes.find((n) => n.uid === uid);
    if (!oldNode) return g;
    const fresh = makeNode(newKind, oldNode.pos);
    if (!fresh) return g;
    for (const [pname, pval] of Object.entries(oldNode.params)) {
      const target = fresh.params[pname];
      if (target && target.type === pval.type) fresh.params[pname] = pval;
    }
    const nIn = entry.inputs.length;
    const nOut = entry.outputs.length;
    const conns: VopConnection[] = [];
    for (const c of g.connections) {
      if (c.from_node !== uid && c.to_node !== uid) {
        conns.push(c);
        continue;
      }
      let nc = { ...c };
      if (nc.from_node === uid) {
        if (nc.from_port >= nOut) continue;
        nc.from_node = fresh.uid;
      }
      if (nc.to_node === uid) {
        if (nc.to_port >= nIn) continue;
        nc.to_node = fresh.uid;
      }
      conns.push(nc);
    }
    return {
      ...g,
      nodes: g.nodes.map((n) => (n.uid === uid ? fresh : n)),
      connections: conns,
    };
  });
  schedulePush();
}

export function moveNode(uid: number, x: number, y: number): void {
  setGraphInternal((g) => ({
    ...g,
    nodes: g.nodes.map((n) => (n.uid === uid ? { ...n, pos: [x, y] } : n)),
  }));
  // Position is canvas metadata only — the cook doesn't depend on it.
  // Push (so save_scene round-trips the layout) but tell the worker
  // not to re-cook.
  schedulePush(/*cook=*/false);
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

export function addConnection(c: VopConnection): void {
  setGraphInternal((g) => {
    const filtered = g.connections.filter(
      (x) => !(x.to_node === c.to_node && x.to_port === c.to_port),
    );
    return { ...g, connections: [...filtered, c] };
  });
  schedulePush();
}

// Per-input constant editor. Updates the node's `input_defaults` map
// keyed by port index (string), then pushes through the regular
// schedulePush() debounce so the constant rides along with whatever
// other edit the user is making. Pass undefined to clear the slot.
export function setInputDefault(uid: number, port: number,
                                value: InputDefault | undefined): void {
  setGraphInternal((g) => ({
    ...g,
    nodes: g.nodes.map((n) => {
      if (n.uid !== uid) return n;
      const key = String(port);
      const next: Record<string, InputDefault> = { ...(n.input_defaults ?? {}) };
      if (value === undefined) delete next[key];
      else                     next[key] = value;
      const hasAny = Object.keys(next).length > 0;
      const out: VopNode = { ...n };
      if (hasAny) out.input_defaults = next;
      else        delete (out as Partial<VopNode>).input_defaults;
      return out;
    }),
  }));
  schedulePush();
}

export function removeConnection(c: VopConnection): void {
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
