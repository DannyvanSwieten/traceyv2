// Solid store for the scene-level SOP graph. Mirrors editor/src/stores/materials.ts
// in shape so the canvas/inspector/palette pattern lifts cleanly: local
// signals + 50ms debounced push to the host. The host re-cooks on every
// push and broadcasts `scene_changed` when the new actor list is live.

import { createSignal } from 'solid-js';
import * as api from '../lib/api';
import {
  SopGraph,
  SopNode,
  SopConnection,
  ParamValue,
  emptyGraph,
  syncNextUid,
} from '../lib/sop_graph';

const [graph, setGraphInternal] = createSignal<SopGraph>(emptyGraph());

const [selectedNodeId, setSelectedNodeId] = createSignal<number | null>(null);
export const selectedNode = selectedNodeId;
export function setSelectedNode(uid: number | null): void {
  setSelectedNodeId(uid);
}

let pushTimer: ReturnType<typeof setTimeout> | null = null;
let dirty = false;
const PUSH_DEBOUNCE_MS = 50;

function schedulePush() {
  dirty = true;
  if (pushTimer !== null) return;
  pushTimer = setTimeout(async () => {
    pushTimer = null;
    if (!dirty) return;
    dirty = false;
    try {
      await api.send<null>('set_sop_graph', { graph: JSON.stringify(graph()) });
    } catch (e) {
      const msg = e instanceof Error ? e.message : JSON.stringify(e);
      console.error('Failed to push SOP graph:', msg);
    }
  }, PUSH_DEBOUNCE_MS);
}

export const sopGraph = graph;

export async function loadSopGraphFromEngine(): Promise<void> {
  try {
    const json = await api.send<string>('get_sop_graph', {});
    if (!json) return;
    const parsed = JSON.parse(json) as SopGraph;
    // Seed the uid allocator past the highest existing node so subsequent
    // add operations don't collide.
    for (const n of parsed.nodes) syncNextUid(n.uid);
    setGraphInternal(parsed);
  } catch (e) {
    console.error('Failed to load SOP graph:', e);
  }
}

// Refresh the SOP graph when the host signals it mutated externally —
// currently emitted after `set_actor_transform` writes back into the source
// ObjectOutput SOP node. Skip the reload if a local edit is pending so we
// don't race with the in-flight debounced push.
api.listen('sop_graph_changed', () => {
  if (pushTimer !== null || dirty) return;
  loadSopGraphFromEngine().catch((e) =>
    console.warn('sop_graph_changed reload failed:', e)
  );
});

export function replaceGraph(next: SopGraph): void {
  for (const n of next.nodes) syncNextUid(n.uid);
  setGraphInternal(next);
  setSelectedNodeId(null);
  schedulePush();
}

// Mutators -----------------------------------------------------------------

export function addNode(node: SopNode): void {
  setGraphInternal((g) => ({ ...g, nodes: [...g.nodes, node] }));
  schedulePush();
}

export function removeNode(uid: number): void {
  setGraphInternal((g) => ({
    ...g,
    nodes: g.nodes.filter((n) => n.uid !== uid),
    connections: g.connections.filter(
      (c) => c.from_node !== uid && c.to_node !== uid
    ),
  }));
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
    nodes: g.nodes.map((n) =>
      n.uid === uid ? { ...n, params: { ...n.params, [paramName]: value } } : n
    ),
  }));
  schedulePush();
}

export function addConnection(c: SopConnection): void {
  setGraphInternal((g) => {
    // A sink port can only have one incoming edge — replace any existing.
    const filtered = g.connections.filter(
      (x) => !(x.to_node === c.to_node && x.to_port === c.to_port)
    );
    return { ...g, connections: [...filtered, c] };
  });
  schedulePush();
}

export function removeConnection(c: SopConnection): void {
  setGraphInternal((g) => ({
    ...g,
    connections: g.connections.filter(
      (x) =>
        !(
          x.from_node === c.from_node &&
          x.from_port === c.from_port &&
          x.to_node === c.to_node &&
          x.to_port === c.to_port
        )
    ),
  }));
  schedulePush();
}

export async function flushSopGraph(): Promise<void> {
  if (pushTimer !== null) {
    clearTimeout(pushTimer);
    pushTimer = null;
  }
  if (dirty) {
    dirty = false;
    try {
      await api.send<null>('set_sop_graph', { graph: JSON.stringify(graph()) });
    } catch (e) {
      console.error('Failed to flush SOP graph:', e);
    }
  }
}
