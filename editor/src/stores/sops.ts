// Solid store for the scene-level SOP graph. Mirrors editor/src/stores/materials.ts
// in shape so the canvas/inspector/palette pattern lifts cleanly: local
// signals + 50ms debounced push of the *root* graph to the host. The host
// re-cooks on every push and broadcasts `scene_changed` when the new actor
// list is live.
//
// Houdini-style /obj subnets nest sub-graphs under a parent SOP node's
// `subgraph` field. The store keeps the entire tree as a single recursive
// SopGraph and tracks "where the user is editing" via `currentPath` — a list
// of subnet uids from root. Mutators take an implicit path and rebuild the
// chain immutably back to the root, so a single debounced push always sends
// the whole nested graph.

import { createSignal } from 'solid-js';
import * as api from '../lib/api';
import {
  SopGraph,
  SopNode,
  SopConnection,
  ParamValue,
  emptyGraph,
  syncNextUidRecursive,
} from '../lib/sop_graph';

const [graph, setGraphInternal] = createSignal<SopGraph>(emptyGraph());
const [currentPathInternal, setCurrentPathInternal] = createSignal<number[]>([]);

// Selection model: a list of uids in click order. The single-uid accessor
// `selectedNode` returns the most recently added — the "primary" — so the
// inspector and keyboard shortcuts that only make sense with one selection
// (e.g. the `o` wire-to-output shortcut) keep working unchanged.
const [selectedNodeIds, setSelectedNodeIdsInternal] = createSignal<number[]>([]);
export const selectedNodes = selectedNodeIds;

const [primarySelectedId, setPrimarySelectedId] = createSignal<number | null>(null);
export const selectedNode = primarySelectedId;

// Compatibility wrapper: replaces the entire selection with either one uid or
// nothing. Existing callers (subnet enter/exit, graph reload, etc.) still
// reset selection through this single entry point.
export function setSelectedNode(uid: number | null): void {
  if (uid === null) {
    setSelectedNodeIdsInternal([]);
    setPrimarySelectedId(null);
    return;
  }
  setSelectedNodeIdsInternal([uid]);
  setPrimarySelectedId(uid);
}

// Replace the entire selection with the given uids. The last entry becomes
// the primary. Used by marquee-select to push a batch result.
export function setSelectedNodes(uids: number[]): void {
  const seen = new Set<number>();
  const ordered: number[] = [];
  for (const u of uids) {
    if (!seen.has(u)) { seen.add(u); ordered.push(u); }
  }
  setSelectedNodeIdsInternal(ordered);
  setPrimarySelectedId(ordered.length > 0 ? ordered[ordered.length - 1] : null);
}

// Add or remove the given uid from the current selection. Toggle is the
// natural primitive for modifier-click (Cmd/Ctrl/Shift+click).
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
// True debounce, not a throttle: every edit restarts the timer so a typing
// burst (or a dragged number-input spinner) coalesces into ONE push once
// activity stops. 300ms feels live without thrashing the cook.
const PUSH_DEBOUNCE_MS = 300;

// ── Undo / redo ─────────────────────────────────────────────────────────
//
// Snapshot-based history at the SOP-graph level. Because VOP graphs live
// nested inside attribute_vop SOP nodes, a SOP-graph snapshot also covers
// VOP edits — one history stack covers both.
//
// Granularity: one undo entry per debounced edit burst. schedulePush
// captures the *pre-edit* committed state on the first mutator call after
// a quiet period (dirty flips false→true). Subsequent edits in the same
// burst skip the snapshot — they collapse into the same undo step. This
// means dragging a node 50 pixels produces one undo entry, not 50.
//
// Material graphs aren't part of SopGraph and are NOT covered here.
const UNDO_LIMIT = 100;
const undoStack: SopGraph[] = [];
const redoStack: SopGraph[] = [];
let lastCommittedGraph: SopGraph = emptyGraph();
const [undoDepth, setUndoDepth] = createSignal(0);
const [redoDepth, setRedoDepth] = createSignal(0);
// Reactive accessors the toolbar/menu can subscribe to for enable/disable.
export const canUndo = (): boolean => undoDepth() > 0;
export const canRedo = (): boolean => redoDepth() > 0;

function refreshHistoryCounts(): void {
  setUndoDepth(undoStack.length);
  setRedoDepth(redoStack.length);
}

function snapshotForUndo(): void {
  undoStack.push(structuredClone(lastCommittedGraph));
  while (undoStack.length > UNDO_LIMIT) undoStack.shift();
  // Any new user action invalidates redo history (standard editor semantics).
  redoStack.length = 0;
  refreshHistoryCounts();
}

// Stringify the graph with every `pos` field stripped AND with object keys
// emitted in a canonical (alphabetical) order. Used to detect position-only
// edits so dragging a node doesn't trigger a re-cook on the native side —
// the geometry output is purely a function of params and connections, so
// canvas layout is a UI-only concern.
//
// Canonical key order matters because lastCommittedGraph receives its key
// ordering from whichever path last committed: an engine reload
// (loadSopGraphFromEngine → JSON.parse(engine_json)) uses the C++
// serializer's order, while a local push (schedulePush) uses whatever the
// frontend mutators built. Without sorting, those two orderings can differ
// for the same logical graph and produce false-positive cookNeeded results
// — every drag after a sop_graph_changed would re-cook unnecessarily.
function structuralKey(g: SopGraph): string {
  return JSON.stringify(g, (key, value) => {
    if (key === 'pos') return null;
    if (value && typeof value === 'object' && !Array.isArray(value)) {
      const sorted: Record<string, unknown> = {};
      for (const k of Object.keys(value).sort()) sorted[k] = (value as Record<string, unknown>)[k];
      return sorted;
    }
    return value;
  });
}

function schedulePush() {
  if (!dirty) {
    // First edit of a burst — snapshot the last committed state before
    // mutating, so undo brings us back to exactly what the engine last saw.
    snapshotForUndo();
  }
  dirty = true;
  if (pushTimer !== null) clearTimeout(pushTimer);
  pushTimer = setTimeout(async () => {
    pushTimer = null;
    if (!dirty) return;
    dirty = false;
    const snapshot = graph();
    const cookNeeded = structuralKey(snapshot) !== structuralKey(lastCommittedGraph);
    try {
      await api.send<null>('set_sop_graph', {
        graph: JSON.stringify(snapshot),
        cook: cookNeeded,
      });
      lastCommittedGraph = structuredClone(snapshot);
    } catch (e) {
      const msg = e instanceof Error ? e.message : JSON.stringify(e);
      console.error('Failed to push SOP graph:', msg);
    }
  }, PUSH_DEBOUNCE_MS);
}

export const sopGraph = graph;
export const currentPath = currentPathInternal;

// ── Cook status ──────────────────────────────────────────────────────────
// Mirrors the native cook lifecycle so the UI can surface a "Cooking…"
// indicator while the worker thread is parsing/decoding. Native broadcasts
// `cook_status` with a boolean each time the worker picks up a request
// and again after apply_emitted lands. Latest-wins.
const [isCookingInternal, setIsCookingInternal] = createSignal(false);
export const isCooking = isCookingInternal;
api.listen('cook_status', (msg) => {
  const busy = msg.busy === true;
  setIsCookingInternal(busy);
});

// ── Path resolution ───────────────────────────────────────────────────────

// Walk the path from the root and return the leaf SopGraph (the inner
// `subgraph` of the last subnet in the path). Falls back to `g` if any path
// entry doesn't resolve, which shouldn't happen during normal edits but keeps
// rendering robust if a stale path lingers after an external graph reload.
function resolveGraph(g: SopGraph, path: number[]): SopGraph {
  let cur = g;
  for (const uid of path) {
    const node = cur.nodes.find((n) => n.uid === uid);
    if (!node || !node.subgraph) return g;  // bail to root rather than crash
    cur = node.subgraph;
  }
  return cur;
}

export const currentGraph = (): SopGraph => resolveGraph(graph(), currentPathInternal());

// Apply `fn` to the SopGraph at `path`, rebuilding the chain back up to the
// root immutably so the parent's `nodes` array gets a new object and Solid's
// shallow signal compare picks up the change.
function updateAtPath(
  g: SopGraph,
  path: number[],
  fn: (sg: SopGraph) => SopGraph,
): SopGraph {
  if (path.length === 0) return fn(g);
  const [head, ...rest] = path;
  return {
    ...g,
    nodes: g.nodes.map((n) =>
      n.uid === head && n.subgraph
        ? { ...n, subgraph: updateAtPath(n.subgraph, rest, fn) }
        : n,
    ),
  };
}

// Truncate `path` to the longest prefix that still resolves through `g`.
// Used after an external reload (sop_graph_changed) so we don't sit inside a
// subnet that no longer exists.
function pruneToValid(g: SopGraph, path: number[]): number[] {
  let cur = g;
  const out: number[] = [];
  for (const uid of path) {
    const node = cur.nodes.find((n) => n.uid === uid);
    if (!node || !node.subgraph) break;
    out.push(uid);
    cur = node.subgraph;
  }
  return out;
}

// ── Initial load + broadcast subscription ─────────────────────────────────

export async function loadSopGraphFromEngine(): Promise<void> {
  try {
    const json = await api.send<string>('get_sop_graph', {});
    if (!json) return;
    const parsed = JSON.parse(json) as SopGraph;
    // Seed the uid allocator past the highest uid anywhere in the tree so
    // subsequent locally-allocated uids don't collide with nested ones.
    syncNextUidRecursive(parsed);
    setGraphInternal(parsed);
    setCurrentPathInternal((p) => pruneToValid(parsed, p));
    // The engine's state is now our committed baseline; any future edits
    // snapshot against this. Don't push onto undoStack — engine reloads
    // aren't user actions to undo, just resyncs.
    lastCommittedGraph = structuredClone(parsed);
  } catch (e) {
    console.error('Failed to load SOP graph:', e);
  }
}

// Refresh the SOP graph when the host signals it mutated externally — emitted
// after `set_actor_transform` writes back into a SOP node's translate/scale,
// or after a keyframe edit through the timeline IPC. Skip the reload if a
// local edit is pending so we don't race with the in-flight debounced push.
api.listen('sop_graph_changed', () => {
  if (pushTimer !== null || dirty) return;
  loadSopGraphFromEngine().catch((e) =>
    console.warn('sop_graph_changed reload failed:', e)
  );
});

export function replaceGraph(next: SopGraph): void {
  syncNextUidRecursive(next);
  setGraphInternal(next);
  setSelectedNode(null);
  setCurrentPathInternal([]);
  schedulePush();
}

// ── Navigation (subnet enter / exit / jump) ───────────────────────────────

// Enter the subnet at the given uid in the *current* graph. No-op if the
// uid doesn't resolve to a node with a subgraph.
export function enterSubnet(uid: number): void {
  const cur = currentGraph();
  const node = cur.nodes.find((n) => n.uid === uid);
  if (!node || !node.subgraph) return;
  setCurrentPathInternal((p) => [...p, uid]);
  setSelectedNode(null);
}

export function exitSubnet(): void {
  setCurrentPathInternal((p) => (p.length > 0 ? p.slice(0, -1) : p));
  setSelectedNode(null);
}

// Truncate the path to the first `length` entries. length === 0 returns to
// the root; passing the current length is a no-op.
export function navigateTo(length: number): void {
  setCurrentPathInternal((p) =>
    length >= p.length ? p : p.slice(0, Math.max(0, length)),
  );
  setSelectedNode(null);
}

// Resolve the path to a list of {uid, name} crumbs for the breadcrumb UI.
// Reads the subnet's `name` parameter when present.
export function pathCrumbs(): { uid: number; name: string }[] {
  const out: { uid: number; name: string }[] = [];
  let cur = graph();
  for (const uid of currentPathInternal()) {
    const node = cur.nodes.find((n) => n.uid === uid);
    if (!node) break;
    const nameParam = node.params['name'];
    const name =
      nameParam && nameParam.type === 'string' && typeof nameParam.value === 'string'
        ? nameParam.value
        : `subnet#${uid}`;
    out.push({ uid, name });
    if (!node.subgraph) break;
    cur = node.subgraph;
  }
  return out;
}

// ── Mutators ──────────────────────────────────────────────────────────────
// All mutators target the *current* sub-graph (resolved via currentPath).
// They call updateAtPath so the parent chain rebuilds immutably and
// schedulePush() always sends the entire root graph.

export function addNode(node: SopNode): void {
  setGraphInternal((g) =>
    updateAtPath(g, currentPathInternal(), (sg) => ({
      ...sg,
      nodes: [...sg.nodes, node],
    })),
  );
  schedulePush();
}

export function removeNode(uid: number): void {
  setGraphInternal((g) =>
    updateAtPath(g, currentPathInternal(), (sg) => ({
      ...sg,
      nodes: sg.nodes.filter((n) => n.uid !== uid),
      connections: sg.connections.filter(
        (c) => c.from_node !== uid && c.to_node !== uid,
      ),
    })),
  );
  schedulePush();
}

export function moveNode(uid: number, x: number, y: number): void {
  setGraphInternal((g) =>
    updateAtPath(g, currentPathInternal(), (sg) => ({
      ...sg,
      nodes: sg.nodes.map((n) => (n.uid === uid ? { ...n, pos: [x, y] } : n)),
    })),
  );
  schedulePush();
}

export function setParam(uid: number, paramName: string, value: ParamValue): void {
  setGraphInternal((g) =>
    updateAtPath(g, currentPathInternal(), (sg) => ({
      ...sg,
      nodes: sg.nodes.map((n) => {
        if (n.uid !== uid) return n;
        // Preserve any channel data the server attached: editing the constant
        // baseline shouldn't wipe out keyframes set via the timeline IPC.
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
    })),
  );
  schedulePush();
}

export function addConnection(c: SopConnection): void {
  setGraphInternal((g) =>
    updateAtPath(g, currentPathInternal(), (sg) => {
      // A sink port can only have one incoming edge — replace any existing.
      const filtered = sg.connections.filter(
        (x) => !(x.to_node === c.to_node && x.to_port === c.to_port),
      );
      return { ...sg, connections: [...filtered, c] };
    }),
  );
  schedulePush();
}

export function removeConnection(c: SopConnection): void {
  setGraphInternal((g) =>
    updateAtPath(g, currentPathInternal(), (sg) => ({
      ...sg,
      connections: sg.connections.filter(
        (x) =>
          !(
            x.from_node === c.from_node &&
            x.from_port === c.from_port &&
            x.to_node === c.to_node &&
            x.to_port === c.to_port
          ),
      ),
    })),
  );
  schedulePush();
}

// ── Tree-wide mutators (used by the scene hierarchy) ────────────────────
//
// Scene-hierarchy edits (delete an actor, drag-reparent, drag-reorder) all
// translate to structural edits of the SOP graph at the location the source
// node currently lives — which can be any depth inside the subnet tree, not
// just the active sub-graph the user is editing in the canvas. These helpers
// walk the full recursive graph rather than going through currentPath, then
// schedule one push back to the engine.

function cutNodeAnywhere(g: SopGraph, uid: number): { graph: SopGraph; removed: SopNode | null } {
  const idx = g.nodes.findIndex((n) => n.uid === uid);
  if (idx >= 0) {
    const removed = g.nodes[idx];
    return {
      graph: {
        ...g,
        nodes: g.nodes.filter((_, i) => i !== idx),
        connections: g.connections.filter(
          (c) => c.from_node !== uid && c.to_node !== uid,
        ),
      },
      removed,
    };
  }
  let removed: SopNode | null = null;
  const nodes = g.nodes.map((n) => {
    if (removed !== null || !n.subgraph) return n;
    const r = cutNodeAnywhere(n.subgraph, uid);
    if (r.removed) {
      removed = r.removed;
      return { ...n, subgraph: r.graph };
    }
    return n;
  });
  return { graph: { ...g, nodes }, removed };
}

function pasteInsideSubnet(g: SopGraph, subnetUid: number, node: SopNode): SopGraph {
  const idx = g.nodes.findIndex((n) => n.uid === subnetUid);
  if (idx >= 0) {
    const target = g.nodes[idx];
    if (!target.subgraph) return g;
    const sub = target.subgraph;
    return {
      ...g,
      nodes: g.nodes.map((n, i) =>
        i === idx
          ? { ...n, subgraph: { ...sub, nodes: [...sub.nodes, node] } }
          : n,
      ),
    };
  }
  return {
    ...g,
    nodes: g.nodes.map((n) =>
      n.subgraph
        ? { ...n, subgraph: pasteInsideSubnet(n.subgraph, subnetUid, node) }
        : n,
    ),
  };
}

function pasteSibling(
  g: SopGraph,
  targetUid: number,
  node: SopNode,
  after: boolean,
): SopGraph {
  const idx = g.nodes.findIndex((n) => n.uid === targetUid);
  if (idx >= 0) {
    const insertAt = after ? idx + 1 : idx;
    const next = [...g.nodes];
    next.splice(insertAt, 0, node);
    return { ...g, nodes: next };
  }
  return {
    ...g,
    nodes: g.nodes.map((n) =>
      n.subgraph
        ? { ...n, subgraph: pasteSibling(n.subgraph, targetUid, node, after) }
        : n,
    ),
  };
}

// Tree-wide delete: remove the node identified by `uid` regardless of which
// sub-graph it lives in. The scene hierarchy uses this to delete the SOP
// node that emits a given actor.
export function removeNodeAnywhere(uid: number): boolean {
  let removed = false;
  setGraphInternal((g) => {
    const r = cutNodeAnywhere(g, uid);
    if (r.removed) removed = true;
    return r.graph;
  });
  if (removed) {
    setCurrentPathInternal((p) => pruneToValid(graph(), p));
    schedulePush();
  }
  return removed;
}

// Tree-wide move: take `sourceUid` out of its current container and place
// it relative to `targetUid` according to `mode`. Returns false when the
// move would be a no-op or unsafe (source == target, dropping a node inside
// its own descendants, target not findable, or 'inside' on a non-subnet).
export function moveNodeAnywhere(
  sourceUid: number,
  targetUid: number,
  mode: 'before' | 'inside' | 'after',
): boolean {
  if (sourceUid === targetUid) return false;
  let success = false;
  setGraphInternal((g) => {
    if (mode === 'inside' && isInSubtree(g, sourceUid, targetUid)) return g;
    const cut = cutNodeAnywhere(g, sourceUid);
    if (!cut.removed) return g;
    const next =
      mode === 'inside'
        ? pasteInsideSubnet(cut.graph, targetUid, cut.removed)
        : pasteSibling(cut.graph, targetUid, cut.removed, mode === 'after');
    // pasteInsideSubnet returns the input unchanged when target isn't a
    // subnet; same for pasteSibling when target isn't findable. Detect via
    // reference equality and bail out (don't commit the cut).
    if (next === cut.graph) return g;
    success = true;
    return next;
  });
  if (success) {
    setCurrentPathInternal((p) => pruneToValid(graph(), p));
    schedulePush();
  }
  return success;
}

// True if `descendantUid` lives anywhere inside `ancestorUid`'s subtree
// (or equals it). Used to reject reparent-into-own-subtree.
function isInSubtree(g: SopGraph, ancestorUid: number, descendantUid: number): boolean {
  function searchSubtree(sg: SopGraph): boolean {
    for (const n of sg.nodes) {
      if (n.uid === descendantUid) return true;
      if (n.subgraph && searchSubtree(n.subgraph)) return true;
    }
    return false;
  }
  function find(sg: SopGraph): SopNode | null {
    for (const n of sg.nodes) {
      if (n.uid === ancestorUid) return n;
      if (n.subgraph) {
        const hit = find(n.subgraph);
        if (hit) return hit;
      }
    }
    return null;
  }
  const ancestor = find(g);
  if (!ancestor) return false;
  if (ancestor.uid === descendantUid) return true;
  return ancestor.subgraph ? searchSubtree(ancestor.subgraph) : false;
}

// True if the node identified by `uid` is a subnet (has a subgraph).
// Hierarchy uses this to decide whether "drop INSIDE" makes sense.
export function nodeIsSubnet(uid: number): boolean {
  function find(sg: SopGraph): SopNode | null {
    for (const n of sg.nodes) {
      if (n.uid === uid) return n;
      if (n.subgraph) {
        const hit = find(n.subgraph);
        if (hit) return hit;
      }
    }
    return null;
  }
  const node = find(graph());
  return !!(node && node.subgraph);
}

// Connect the given node's first output to the first input of the nearest
// `object_output` node in the *current* sub-graph. Mirrors Houdini's
// "promote to render output" gesture — picks the first object_output found
// (there's no render flag yet) and replaces any existing wire on its input,
// since each sink port can only have one incoming edge anyway.
// Returns true if a connection was made.
export function connectToObjectOutput(uid: number): boolean {
  const sg = currentGraph();
  if (!sg.nodes.find((n) => n.uid === uid)) return false;
  const out = sg.nodes.find((n) => n.kind === 'object_output');
  if (!out || out.uid === uid) return false;
  addConnection({ from_node: uid, from_port: 0, to_node: out.uid, to_port: 0 });
  return true;
}

export async function flushSopGraph(): Promise<void> {
  if (pushTimer !== null) {
    clearTimeout(pushTimer);
    pushTimer = null;
  }
  if (dirty) {
    dirty = false;
    const snapshot = graph();
    try {
      await api.send<null>('set_sop_graph', { graph: JSON.stringify(snapshot) });
      lastCommittedGraph = structuredClone(snapshot);
    } catch (e) {
      console.error('Failed to flush SOP graph:', e);
    }
  }
}

// ── Undo / redo entry points ────────────────────────────────────────────
//
// Both functions are idempotent at the boundaries: undo() with an empty
// stack and redo() with an empty stack return false and leave state
// untouched. Each successful step:
//   1. Flushes any in-flight edit burst so we work from a committed state.
//   2. Pops one entry from the source stack.
//   3. Pushes the current state to the opposite stack so it can be redone.
//   4. Applies the popped state locally AND pushes it to the engine right
//      away (skipping the 300 ms debounce — the user wants immediate
//      visible feedback after Cmd+Z).
// Selection is cleared on undo/redo since node uids can disappear.

async function applyHistoryState(g: SopGraph): Promise<void> {
  const cloned = structuredClone(g);
  syncNextUidRecursive(cloned);
  setGraphInternal(cloned);
  lastCommittedGraph = structuredClone(cloned);
  setCurrentPathInternal((p) => pruneToValid(cloned, p));
  setSelectedNode(null);
  try {
    await api.send<null>('set_sop_graph', { graph: JSON.stringify(cloned) });
  } catch (e) {
    console.error('history push failed:', e);
  }
}

export async function undo(): Promise<boolean> {
  if (dirty || pushTimer !== null) await flushSopGraph();
  if (undoStack.length === 0) return false;
  const previous = undoStack.pop()!;
  redoStack.push(structuredClone(lastCommittedGraph));
  refreshHistoryCounts();
  await applyHistoryState(previous);
  return true;
}

export async function redo(): Promise<boolean> {
  if (dirty || pushTimer !== null) await flushSopGraph();
  if (redoStack.length === 0) return false;
  const next = redoStack.pop()!;
  undoStack.push(structuredClone(lastCommittedGraph));
  refreshHistoryCounts();
  await applyHistoryState(next);
  return true;
}
