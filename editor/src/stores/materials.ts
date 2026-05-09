// Solid store for the active material graph. Backed by a debounced sync to
// the native engine: any mutation pushes a fresh JSON string within 50ms,
// triggering recompile + accumulator invalidation server-side.

import { createSignal } from 'solid-js';
import * as api from '../lib/api';
import {
  ShaderGraph,
  Node,
  Connection,
  reseedUidsFrom,
} from '../lib/material_graph';

const EMPTY_GRAPH: ShaderGraph = {
  version: 1,
  uid: 0,
  nodes: [],
  connections: [],
};

const [graph, setGraphInternal] = createSignal<ShaderGraph>(EMPTY_GRAPH);

// Currently-selected node uid (or null). Lives here rather than inside the
// canvas so the inspector and canvas share a single source of truth.
const [selectedNodeId, setSelectedNodeId] = createSignal<number | null>(null);
export const selectedNode = selectedNodeId;
export function setSelectedNode(uid: number | null): void {
  setSelectedNodeId(uid);
}

// Per-user material library, kept in a shared signal so the modal's library
// panel and the actor-inspector dropdown both refresh when an entry is added
// or removed -- regardless of which component triggered the change.
const [libraryEntries, setLibraryEntries] = createSignal<string[]>([]);
export const materialLibraryEntries = libraryEntries;

export async function refreshMaterialLibrary(): Promise<void> {
  try {
    setLibraryEntries(await api.listMaterialLibrary());
  } catch (e) {
    console.warn('listMaterialLibrary failed:', e);
  }
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
      await api.setMaterialGraph(JSON.stringify(graph()));
    } catch (e) {
      const msg = e instanceof Error ? e.message : JSON.stringify(e);
      console.error('Failed to push material graph:', msg);
    }
  }, PUSH_DEBOUNCE_MS);
}

export const materialGraph = graph;

export async function loadMaterialGraphFromEngine(): Promise<void> {
  try {
    const json = await api.getMaterialGraph();
    if (!json) return;
    const parsed = JSON.parse(json) as ShaderGraph;
    reseedUidsFrom(parsed);
    setGraphInternal(parsed);
  } catch (e) {
    console.error('Failed to load material graph:', e);
  }
}

// Replace the active graph wholesale (e.g. loading from the library). Schedules
// a push so the engine re-compiles + re-uploads.
export function replaceGraph(next: ShaderGraph): void {
  reseedUidsFrom(next);
  setGraphInternal(next);
  setSelectedNodeId(null);
  schedulePush();
}

// Mutators -----------------------------------------------------------------

export function addNode(node: Node): void {
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
    nodes: g.nodes.map((n) =>
      n.uid === uid ? { ...n, position: [x, y] } : n
    ),
  }));
  // Position changes don't affect the compiled program, but we still want
  // them persisted so they survive a reload. Let the debounce coalesce
  // multiple position updates per frame.
  schedulePush();
}

export function updateNode<T extends Node>(uid: number, patch: Partial<T>): void {
  setGraphInternal((g) => ({
    ...g,
    nodes: g.nodes.map((n) => (n.uid === uid ? ({ ...n, ...patch } as Node) : n)),
  }));
  schedulePush();
}

export function addConnection(c: Connection): void {
  setGraphInternal((g) => {
    // Replace any existing connection feeding into the same input port -- a
    // sink port can only have one incoming edge.
    const filtered = g.connections.filter(
      (x) => !(x.to_node === c.to_node && x.to_port === c.to_port)
    );
    return { ...g, connections: [...filtered, c] };
  });
  schedulePush();
}

export function removeConnection(c: Connection): void {
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

// Force-flush any pending debounced push immediately (e.g. on close).
export async function flushMaterialGraph(): Promise<void> {
  if (pushTimer !== null) {
    clearTimeout(pushTimer);
    pushTimer = null;
  }
  if (dirty) {
    dirty = false;
    try {
      await api.setMaterialGraph(JSON.stringify(graph()));
    } catch (e) {
      console.error('Failed to flush material graph:', e);
    }
  }
}
