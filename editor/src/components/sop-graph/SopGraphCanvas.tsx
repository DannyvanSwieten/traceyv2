// SVG-based pan/zoom/wires canvas for the SOP graph. Mirrors the structure
// of editor/src/components/material-graph/GraphCanvas.tsx but talks to the
// SOP store + catalog instead of the material ones. The two canvases share
// a stylesheet (../material-graph/GraphCanvas.css) so theming stays
// consistent; promoting the rendering core to a generic component is a
// later refactor (the existing material canvas is intentionally left alone
// to keep this change contained).

import { Component, For, createSignal, onCleanup, onMount, Show } from 'solid-js';
import {
  SopNode,
  SopConnection,
  inputPortCount,
  outputPortCount,
  inputPortName,
  outputPortName,
  lookupCatalog,
  catalog,
  makeNode,
  allocNodeUid,
} from '../../lib/sop_graph';
import {
  addNode,
  connectToObjectOutput,
  currentGraph,
  enterSubnet,
  exitSubnet,
  moveNode,
  addConnection,
  removeConnection,
  removeNode,
  replaceNode,
  selectedNode,
  selectedNodes,
  setSelectedNode,
  setSelectedNodes,
  toggleSelectedNode,
  toggleNodeBypass,
  isNodeSelected,
} from '../../stores/sops';
import { openVopEditor } from '../../stores/vops';
import {
  rectFromCorners,
  nodesInRect,
  type MarqueeRect,
  type NodeBox,
} from '../../lib/graph_canvas_marquee';
import { ContextMenu, type MenuEntry } from '../context-menu/ContextMenu';
import { computeInsertShift } from '../../lib/graph_insert_layout';
import { computeFrame, type FrameRect } from '../../lib/graph_frame';
import {
  sampleCubicBezier,
  intersectingConnections,
  type ConnPolyline,
  type Pt,
} from '../../lib/graph_cut';
import '../material-graph/GraphCanvas.css';

// SOPs flow top-to-bottom (Houdini-classic): inputs sit on the node's top
// edge, outputs on the bottom edge, multiple ports of the same kind spread
// evenly along that edge. Bezier control points are along the Y axis so
// wires curve down between rows of nodes rather than left↔right.
const NODE_WIDTH = 140;
const NODE_HEADER_HEIGHT = 24;
const NODE_BODY_PADDING = 10;
const PORT_RADIUS = 5;

function portX(idx: number, total: number): number {
  // Spread N ports evenly across the node's width, with margins on the ends
  // so a single port lands at the centre.
  const n = Math.max(total, 1);
  return ((idx + 1) / (n + 1)) * NODE_WIDTH;
}
function nodeHeight(_node: SopNode): number {
  // Compact, fixed-height node since ports no longer stack as rows.
  return NODE_HEADER_HEIGHT + NODE_BODY_PADDING;
}
function nodeOrigin(node: SopNode): [number, number] {
  return [node.pos?.[0] ?? 0, node.pos?.[1] ?? 0];
}
function inputAnchor(node: SopNode, portIdx: number): [number, number] {
  const [x, y] = nodeOrigin(node);
  const total = inputPortCount(node.kind);
  return [x + portX(portIdx, total), y];
}
function outputAnchor(node: SopNode, portIdx: number): [number, number] {
  const [x, y] = nodeOrigin(node);
  const total = outputPortCount(node.kind);
  return [x + portX(portIdx, total), y + nodeHeight(node)];
}
function bezier(from: [number, number], to: [number, number]): string {
  // Vertical bezier: control handles offset on Y so the curve flows
  // downward between source-bottom and destination-top.
  const [x1, y1] = from;
  const [x2, y2] = to;
  const dy = Math.max(40, Math.abs(y2 - y1) * 0.5);
  return `M ${x1} ${y1} C ${x1} ${y1 + dy}, ${x2} ${y2 - dy}, ${x2} ${y2}`;
}
function nodeLabel(n: SopNode): string {
  // Prefer the node's `name` param when present — subnets and object_outputs
  // carry it as a user-facing identifier (the glTF importer fills it with
  // each node's name, the palette seeds defaults). Falls back to the catalog
  // label / kind for nodes without a name param (primitives, transforms,
  // gltf_import, etc.) so they keep their generic "Primitive Cube" / "glTF
  // Import" titles.
  const named = n.params['name'];
  if (named && named.type === 'string' && typeof named.value === 'string' && named.value) {
    return named.value;
  }
  return lookupCatalog(n.kind)?.label ?? n.kind;
}

// `kind` records whether the drag began from an input or output port.
// Determines which direction the bezier rubber-band paints (input drags
// extend backward toward an upstream source) and how a released
// connection is wired: dragging from an output → input is a forward
// connection; dragging from an input → output is the reverse.
interface PortRef { nodeUid: number; portIdx: number; kind: 'input' | 'output' }

// Module-scoped clipboard so a copy survives unmount/remount of the dock.
// Each canvas owns its own clipboard (no cross-graph paste — node kinds
// don't translate across SOP/VOP/material). The clipboard stores nodes
// with their *original* uids; paste remaps to fresh uids on insert.
interface SopClipboard {
  nodes: SopNode[];
  connections: SopConnection[];
  // Top-left of the clipboard's bounding box, in graph-space. Paste at
  // cursor positions the clipboard so this corner lands at the cursor.
  originX: number;
  originY: number;
}
let sopClipboard: SopClipboard | null = null;

export const SopGraphCanvas: Component = () => {
  const [pan, setPan] = createSignal<[number, number]>([0, 0]);
  const [zoom, setZoom] = createSignal(1);
  const [pendingFrom, setPendingFrom] = createSignal<PortRef | null>(null);
  // While dragging from a port, the cursor "magnetises" to the nearest
  // compatible port within a world-space radius. The rubber-band wire
  // visually anchors there instead of at the raw cursor, and pointerup
  // completes the connection to the snapped target — so the user doesn't
  // have to land pixel-perfect on the port circle. Set to null when no
  // port is within range, so the cord follows the cursor as before.
  const [snapTarget, setSnapTarget] = createSignal<PortRef | null>(null);
  const [mouseWorld, setMouseWorld] = createSignal<[number, number]>([0, 0]);
  // Houdini-style: hold Alt (Option on macOS) to pan. Drag on empty canvas
  // without Alt does a rubber-band (marquee) select instead. Alt was picked
  // over Space because the editor's timeline uses Space for play/pause.
  const [panKeyDown, setPanKeyDown] = createSignal(false);
  const [marquee, setMarquee] = createSignal<MarqueeRect | null>(null);
  // Cut mode: hold Y to swap the cursor to scissors and drag a path that
  // deletes every connection it crosses. cutPath is the live polyline in
  // world space, drawn under the cursor while the gesture is active.
  const [cutKeyDown, setCutKeyDown] = createSignal(false);
  const [cutPath, setCutPath] = createSignal<Pt[]>([]);

  let svgRef: SVGSVGElement | undefined;

  function clientToWorld(clientX: number, clientY: number): [number, number] {
    if (!svgRef) return [0, 0];
    const rect = svgRef.getBoundingClientRect();
    const sx = clientX - rect.left;
    const sy = clientY - rect.top;
    const [px, py] = pan();
    const z = zoom();
    return [(sx - px) / z, (sy - py) / z];
  }

  function onSvgPointerDown(e: PointerEvent) {
    if (e.button !== 0) return;
    e.preventDefault();
    // Cut takes priority over everything else (Alt-pan, node drag, port
    // drag, marquee). Holding Y while pressing means the user is in
    // "drag-to-cut connections" mode.
    if (cutKeyDown()) { startCutDrag(e); return; }
    // Alt-held drag pans the canvas, even when starting over a node or
    // port — matches the Houdini convention of Alt being a pan modifier.
    // Also read e.altKey directly so the pan starts on the very first
    // pointerdown even if the keydown listener hasn't fired yet (focus on
    // a different element when Alt was pressed).
    if (panKeyDown() || e.altKey) { startCanvasPan(e); return; }
    // Re-query the live element at the cursor in case a focus blur on a
    // previously-focused inspector input fired its commit-on-change
    // handler synchronously around this pointerdown, mutating the graph
    // signal and causing Solid to replace the underlying <g>. Without
    // this, `e.target` points at the (now detached) old DOM node and
    // `closest()` walks a parent chain that ends in null — the click
    // falls through to marquee select instead of starting the drag, and
    // the user has to click a second time to actually grab the node.
    const liveTarget = (document.elementFromPoint(e.clientX, e.clientY)
                        ?? e.target) as Element;
    if (liveTarget.closest?.('[data-port-kind]')) return;
    const nodeEl = liveTarget.closest?.('[data-node-uid]');
    if (nodeEl) {
      const uid = parseInt(nodeEl.getAttribute('data-node-uid') || '');
      if (!Number.isNaN(uid)) {
        startNodeDrag(e, uid);
        return;
      }
    }
    if (pendingFrom()) {
      setPendingFrom(null);
      return;
    }
    startMarqueeSelect(e);
  }

  function onSvgPointerMove(e: PointerEvent) {
    setMouseWorld(clientToWorld(e.clientX, e.clientY));
  }

  function startCanvasPan(e: PointerEvent) {
    const startPan = pan();
    const startX = e.clientX;
    const startY = e.clientY;
    const onMove = (mv: PointerEvent) => {
      setPan([startPan[0] + (mv.clientX - startX), startPan[1] + (mv.clientY - startY)]);
    };
    const onUp = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  }

  // Snapshot every connection in the current sub-graph as a sampled
  // polyline keyed by (from_node, from_port, to_node, to_port). We do
  // this once at pointerdown — the cut path's segments are then tested
  // against these cached polylines as the cursor moves, so the hot
  // loop is O(cutSegments × connections × bezierSamples) without
  // re-walking the graph signal on every move.
  function snapshotConnectionPolylines(): ConnPolyline<SopConnection>[] {
    const g = currentGraph();
    const byUid = new Map<number, SopNode>();
    for (const n of g.nodes) byUid.set(n.uid, n);
    const out: ConnPolyline<SopConnection>[] = [];
    for (const c of g.connections) {
      const a = byUid.get(c.from_node);
      const b = byUid.get(c.to_node);
      if (!a || !b) continue;
      const p0 = outputAnchor(a, c.from_port);
      const p3 = inputAnchor(b, c.to_port);
      // SOPs flow top-to-bottom: control handles offset along Y. Mirror
      // the bezier() formula above so the sampled curve matches what's
      // drawn on screen.
      const dy = Math.max(40, Math.abs(p3[1] - p0[1]) * 0.5);
      const p1: Pt = [p0[0], p0[1] + dy];
      const p2: Pt = [p3[0], p3[1] - dy];
      out.push({ key: c, points: sampleCubicBezier(p0, p1, p2, p3, 16) });
    }
    return out;
  }

  function startCutDrag(e: PointerEvent) {
    const polylines = snapshotConnectionPolylines();
    const cutHits = new Set<string>(); // already-deleted, keyed by stringified conn
    const keyOf = (c: SopConnection) =>
      `${c.from_node}:${c.from_port}>${c.to_node}:${c.to_port}`;
    const start = clientToWorld(e.clientX, e.clientY);
    let last = start;
    setCutPath([start]);

    const onMove = (mv: PointerEvent) => {
      const p = clientToWorld(mv.clientX, mv.clientY);
      setCutPath([...cutPath(), p]);
      const hits = intersectingConnections(last, p, polylines);
      for (const c of hits) {
        const k = keyOf(c);
        if (cutHits.has(k)) continue;
        cutHits.add(k);
        removeConnection(c);
      }
      last = p;
    };
    const onUp = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      setCutPath([]);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  }

  function startMarqueeSelect(e: PointerEvent) {
    const additive = e.metaKey || e.ctrlKey || e.shiftKey;
    // Without a modifier, clearing first matches "drag a fresh box, replace
    // selection". With a modifier we keep what's there and extend it on up.
    const preExisting = additive ? [...selectedNodes()] : [];
    if (!additive) setSelectedNode(null);

    const startWorld = clientToWorld(e.clientX, e.clientY);
    setMarquee(rectFromCorners(startWorld, startWorld));

    const onMove = (mv: PointerEvent) => {
      const cur = clientToWorld(mv.clientX, mv.clientY);
      setMarquee(rectFromCorners(startWorld, cur));
    };
    const onUp = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      const rect = marquee();
      setMarquee(null);
      if (!rect) return;
      // Treat a near-zero drag as a click — no selection change in that case
      // (the modifier-less click already cleared above).
      const dx = rect.maxX - rect.minX;
      const dy = rect.maxY - rect.minY;
      if (dx < 1 && dy < 1) return;

      // Build the node-box list from the current sub-graph; the canvas only
      // renders/selects nodes in the visible level.
      const boxes: NodeBox<number>[] = currentGraph().nodes.map((n) => ({
        uid: n.uid,
        x: nodeOrigin(n)[0],
        y: nodeOrigin(n)[1],
        width: NODE_WIDTH,
        height: nodeHeight(n),
      }));
      const hits = nodesInRect(rect, boxes);
      setSelectedNodes(additive ? [...preExisting, ...hits] : hits);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  }

  function onWheel(e: WheelEvent) {
    e.preventDefault();
    const factor = Math.exp(-e.deltaY * 0.001);
    const z = zoom();
    const newZoom = Math.max(0.25, Math.min(4, z * factor));
    if (svgRef) {
      const rect = svgRef.getBoundingClientRect();
      const cx = e.clientX - rect.left;
      const cy = e.clientY - rect.top;
      const [px, py] = pan();
      const ratio = newZoom / z;
      setPan([cx - (cx - px) * ratio, cy - (cy - py) * ratio]);
    }
    setZoom(newZoom);
  }

  function startNodeDrag(e: PointerEvent, uid: number) {
    const multi = e.metaKey || e.ctrlKey || e.shiftKey;
    if (multi) {
      // Modifier-click: toggle membership in the selection and don't start a
      // drag. Matches Maya/Houdini ergonomics — drag-with-modifier is a
      // marquee operation (not implemented here yet), not a per-node move.
      toggleSelectedNode(uid);
      return;
    }

    // No modifier: if the clicked node isn't already part of the selection,
    // make it the sole selection. If it IS already selected (as part of a
    // multi-selection), keep the whole set so the upcoming drag translates
    // every selected node by the same delta.
    if (!isNodeSelected(uid)) {
      setSelectedNode(uid);
    }

    // Snapshot starting positions for every selected node so the drag math
    // stays stable across `moveNode` updates (those rewrite `currentGraph()`
    // and would otherwise drift as the loop re-reads positions).
    const [startX, startY] = clientToWorld(e.clientX, e.clientY);
    const origPositions = new Map<number, [number, number]>();
    for (const u of selectedNodes()) {
      const n = currentGraph().nodes.find((nd) => nd.uid === u);
      if (n) origPositions.set(u, nodeOrigin(n));
    }
    if (origPositions.size === 0) return;

    const onMove = (mv: PointerEvent) => {
      const [wx, wy] = clientToWorld(mv.clientX, mv.clientY);
      const dx = wx - startX;
      const dy = wy - startY;
      for (const [u, [ox, oy]] of origPositions) {
        moveNode(u, ox + dx, oy + dy);
      }
    };
    const onUp = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  }

  // Drag-from-port wiring. Pointerdown on either an input or output port
  // starts a rubber-band drag (rendered as a bezier from the source port
  // to the cursor). Pointerup decides what happens:
  //   • on a port of the *other* kind     → complete the connection
  //   • on a port of the *same* kind /
  //     on a port on the same node        → cancel (no self-loops)
  //   • on the empty canvas               → open the add-node menu in
  //                                          "add + connect" mode; picking
  //                                          a node spawns it at the
  //                                          cursor and wires it into the
  //                                          drag's source port
  //   • on a node body / anything else    → cancel
  //
  // Connection direction follows the drag origin: dragging from an
  // output uses (output → input); dragging from an input uses the
  // reverse pairing (output of the dropped target → that input).
  // Scan every node's compatible-kind ports for the one closest to
  // (mx, my) in world space, within `radius` world-units. Returns null
  // when nothing's in range. Skips the source node so dragging from a
  // port doesn't snap to a sibling on the same node — that's never a
  // legal connection (no self-loops). The radius is generous on
  // purpose: at default zoom one node is ~140 world-units wide, so 50
  // is roughly "if the cursor is more than a third of a node away,
  // don't snap".
  const SNAP_RADIUS = 50;
  function findSnapTarget(from: PortRef, mx: number, my: number): PortRef | null {
    const targetKind: 'input' | 'output' = from.kind === 'output' ? 'input' : 'output';
    const r2 = SNAP_RADIUS * SNAP_RADIUS;
    let best: PortRef | null = null;
    let bestD2 = r2;
    for (const node of currentGraph().nodes) {
      if (node.uid === from.nodeUid) continue;
      const count = targetKind === 'input'
        ? inputPortCount(node.kind)
        : outputPortCount(node.kind);
      for (let i = 0; i < count; ++i) {
        const [ax, ay] = targetKind === 'input'
          ? inputAnchor(node, i)
          : outputAnchor(node, i);
        const dx = ax - mx, dy = ay - my;
        const d2 = dx * dx + dy * dy;
        if (d2 < bestD2) {
          bestD2 = d2;
          best = { nodeUid: node.uid, portIdx: i, kind: targetKind };
        }
      }
    }
    return best;
  }

  function startPortDrag(e: PointerEvent, nodeUid: number, portIdx: number,
                         kind: 'input' | 'output') {
    e.stopPropagation();
    e.preventDefault();

    // Dual-mode wiring: if a connection is already pending (from a prior
    // click-without-drag on another port), treat this pointerdown as the
    // *completion* click. Otherwise begin a tentative drag — pointerup
    // without significant motion on the same port leaves pendingFrom set
    // so a subsequent click on another port completes the connection
    // (click-click mode). With motion, behaves as before (drag-connect).
    const existing = pendingFrom();
    if (existing && (existing.nodeUid !== nodeUid || existing.kind !== kind)) {
      completeConnectionFromDrag(existing, nodeUid, portIdx, kind);
      setPendingFrom(null);
      return;
    }

    setPendingFrom({ nodeUid, portIdx, kind });
    setSnapTarget(null);
    const startX = e.clientX;
    const startY = e.clientY;
    const DRAG_THRESHOLD = 4; // px in client space — below this, it's a click
    let moved = false;

    const onMove = (mv: PointerEvent) => {
      const [wx, wy] = clientToWorld(mv.clientX, mv.clientY);
      setMouseWorld([wx, wy]);
      const from = pendingFrom();
      setSnapTarget(from ? findSnapTarget(from, wx, wy) : null);
      if (!moved) {
        const dx = mv.clientX - startX;
        const dy = mv.clientY - startY;
        if (dx * dx + dy * dy > DRAG_THRESHOLD * DRAG_THRESHOLD) moved = true;
      }
    };
    const onUp = (mv: PointerEvent) => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);

      const from = pendingFrom();
      if (!from) { setSnapTarget(null); return; }

      // Magnetic snap takes priority: when the rubber-band was visibly
      // attached to a nearby port at release time, complete the
      // connection to *that* port rather than rechecking elementFromPoint.
      // Keeps the "I let go close enough" UX honest — the user committed
      // to the snapped target by releasing while the snap was active.
      const snap = snapTarget();
      if (snap) {
        completeConnectionFromDrag(from, snap.nodeUid, snap.portIdx, snap.kind);
        setPendingFrom(null);
        setSnapTarget(null);
        return;
      }

      // What did we drop on? elementFromPoint sees through SVG, so the
      // port circles + the canvas background both report cleanly.
      const dropEl = document.elementFromPoint(mv.clientX, mv.clientY) as Element | null;
      const portEl = dropEl?.closest?.('[data-port-kind]');
      const nodeEl = dropEl?.closest?.('[data-node-uid]');

      // Click-without-drag on the source port → enter click-click mode:
      // keep pendingFrom set, do nothing on this pointerup. The next
      // pointerdown on a different port (handled above) will complete it,
      // or a pointerdown on empty canvas will cancel.
      if (!moved && portEl) {
        const upKind = portEl.getAttribute('data-port-kind') as 'input' | 'output';
        const upPortIdx = parseInt(portEl.getAttribute('data-port-idx') || '0');
        const upNodeUid = parseInt(nodeEl?.getAttribute('data-node-uid') || '0');
        if (upNodeUid === from.nodeUid && upPortIdx === from.portIdx && upKind === from.kind) {
          return;
        }
      }

      if (portEl) {
        const dropKind = portEl.getAttribute('data-port-kind') as 'input' | 'output';
        const dropPortIdx = parseInt(portEl.getAttribute('data-port-idx') || '0');
        const dropNodeUid = parseInt(nodeEl?.getAttribute('data-node-uid') || '0');
        if (!Number.isNaN(dropNodeUid) && !Number.isNaN(dropPortIdx)) {
          completeConnectionFromDrag(from, dropNodeUid, dropPortIdx, dropKind);
        }
        setPendingFrom(null);
        return;
      }

      if (nodeEl) {
        // Released on a node's body (not a port) — cancel silently.
        setPendingFrom(null);
        return;
      }

      // Empty canvas. With a real drag → open the add-node menu in
      // add+connect mode. With a click (no motion) → cancel pending so
      // a stray click on a port followed by a click on empty canvas
      // doesn't hijack the next port click.
      if (!moved) {
        setPendingFrom(null);
        return;
      }
      const [wx, wy] = clientToWorld(mv.clientX, mv.clientY);
      setMenuState({
        kind: 'add',
        clientX: mv.clientX, clientY: mv.clientY,
        worldX: wx, worldY: wy,
        connectFrom: from,
      });
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  }

  function completeConnectionFromDrag(
    from: PortRef,
    dropNodeUid: number,
    dropPortIdx: number,
    dropKind: 'input' | 'output',
  ) {
    if (from.kind === dropKind) return;            // same kind → no-op
    if (from.nodeUid === dropNodeUid) return;       // self-loop → no-op
    // Normalise to (output node → input node).
    if (from.kind === 'output') {
      addConnection({
        from_node: from.nodeUid, from_port: from.portIdx,
        to_node: dropNodeUid,    to_port: dropPortIdx,
      });
    } else {
      addConnection({
        from_node: dropNodeUid,  from_port: dropPortIdx,
        to_node: from.nodeUid,   to_port: from.portIdx,
      });
    }
  }

  // Alt-key tracking lives on `window` so the pan-modifier works even when
  // focus has wandered to a sibling panel (inspector, palette dropdown, etc.).
  // ── Copy / paste / duplicate ─────────────────────────────────────────────
  // Snapshot the current selection into the module-level clipboard. Only
  // connections whose *both* endpoints are in the selection are captured —
  // a half-selected wire would have nothing to land on after paste. The
  // clipboard stores a deep copy so subsequent edits in the source graph
  // don't mutate it.
  function copySelection() {
    const uids = new Set(selectedNodes());
    if (uids.size === 0) return;
    const g = currentGraph();
    const nodes: SopNode[] = [];
    let minX = Infinity, minY = Infinity;
    for (const n of g.nodes) {
      if (!uids.has(n.uid)) continue;
      nodes.push(structuredClone(n));
      const [x, y] = nodeOrigin(n);
      if (x < minX) minX = x;
      if (y < minY) minY = y;
    }
    if (nodes.length === 0) return;
    const connections = g.connections
      .filter((c) => uids.has(c.from_node) && uids.has(c.to_node))
      .map((c) => structuredClone(c));
    sopClipboard = {
      nodes,
      connections,
      originX: minX === Infinity ? 0 : minX,
      originY: minY === Infinity ? 0 : minY,
    };
  }
  // Place the clipboard at (targetX, targetY) in graph space — that point
  // becomes the new top-left corner. Allocates fresh uids for each
  // clipboard node and remaps connections so the pasted block is a
  // self-contained copy independent of the original. The pasted nodes
  // become the new selection so the user can drag them immediately.
  function pasteAt(targetX: number, targetY: number) {
    if (!sopClipboard || sopClipboard.nodes.length === 0) return;
    const remap = new Map<number, number>();
    const newUids: number[] = [];
    const dx = targetX - sopClipboard.originX;
    const dy = targetY - sopClipboard.originY;
    for (const src of sopClipboard.nodes) {
      const newUid = allocNodeUid();
      remap.set(src.uid, newUid);
      newUids.push(newUid);
      const clone: SopNode = {
        ...structuredClone(src),
        uid: newUid,
        pos: [src.pos[0] + dx, src.pos[1] + dy],
      };
      addNode(clone);
    }
    for (const c of sopClipboard.connections) {
      const fn = remap.get(c.from_node);
      const tn = remap.get(c.to_node);
      if (fn === undefined || tn === undefined) continue;
      addConnection({ from_node: fn, from_port: c.from_port, to_node: tn, to_port: c.to_port });
    }
    setSelectedNodes(newUids);
  }
  // Duplicate = copy current selection + paste with a small fixed offset
  // so the clone is visually offset from the original. Keeps the
  // clipboard intact so a follow-up Cmd+V still pastes the same set.
  function duplicateSelection() {
    const uids = selectedNodes();
    if (uids.length === 0) return;
    const savedClipboard = sopClipboard;
    copySelection();
    if (sopClipboard) {
      pasteAt(sopClipboard.originX + 30, sopClipboard.originY + 30);
    }
    // Restore the previous clipboard contents — duplicate is a transient
    // operation; the user shouldn't lose what they copied earlier.
    sopClipboard = savedClipboard ?? sopClipboard;
  }

  // Frame the given node uids inside the viewport. Skips silently when
  // the canvas hasn't mounted yet or none of the uids match nodes in the
  // current graph (e.g. user pressed F with a stale selection during a
  // graph reload).
  function frameNodes(uids: number[]) {
    if (!svgRef) return;
    const g = currentGraph();
    const rects: FrameRect[] = [];
    for (const uid of uids) {
      const n = g.nodes.find((x) => x.uid === uid);
      if (!n) continue;
      const [x, y] = nodeOrigin(n);
      const h = nodeHeight(n);
      rects.push({ minX: x, minY: y, maxX: x + NODE_WIDTH, maxY: y + h });
    }
    const rect = svgRef.getBoundingClientRect();
    const r = computeFrame(rects, rect.width, rect.height);
    if (!r) return;
    setPan(r.pan);
    setZoom(r.zoom);
  }

  // The SVG-level keydown only sees keys while the canvas itself has focus,
  // which is too narrow for a global "hold to pan" gesture.
  onMount(() => {
    const isTextEditing = (target: EventTarget | null): boolean => {
      const el = target as HTMLElement | null;
      if (!el) return false;
      const tag = el.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return true;
      // contenteditable / web-components used by some inspectors.
      return el.isContentEditable === true;
    };
    const onDown = (e: KeyboardEvent) => {
      // Skip everything if the user is typing in an input — typing "o" in a
      // parameter field shouldn't fire connectToObjectOutput, Delete in a
      // number input shouldn't nuke selected nodes, etc.
      if (isTextEditing(e.target)) return;

      // Alt (Option on macOS) is the pan modifier. Space was the original
      // choice but it conflicts with timeline play/pause; Alt-drag matches
      // Houdini's network-editor convention and stays out of the way of
      // text input (which preempts above via isTextEditing).
      if (e.key === 'Alt' || e.altKey) {
        if (!e.repeat) setPanKeyDown(true);
        // No preventDefault — Alt is a modifier; suppressing it can break
        // OS-level accelerators that we don't own.
        return;
      }
      // Y = hold to enter cut mode. Houdini's convention. Don't fire on
      // repeats — once it's true, it's true; we'll clear on keyup.
      if (e.key === 'y' || e.key === 'Y') {
        if (!e.repeat) setCutKeyDown(true);
        return;
      }

      // Cmd/Ctrl combos first — branched before the bare-letter shortcuts
      // so e.g. Cmd+A is "select all" and not "frame all".
      if (e.metaKey || e.ctrlKey) {
        const k = e.key.toLowerCase();
        if (k === 'a') {
          e.preventDefault();
          setSelectedNodes(currentGraph().nodes.map((n) => n.uid));
          return;
        }
        if (k === 'c') {
          e.preventDefault();
          copySelection();
          return;
        }
        if (k === 'v') {
          e.preventDefault();
          const [wx, wy] = mouseWorld();
          pasteAt(wx, wy);
          return;
        }
        if (k === 'd') {
          e.preventDefault();
          duplicateSelection();
          return;
        }
        // Other Cmd/Ctrl combos: fall through (OS / browser handle them).
        return;
      }

      // Delete / Backspace / Escape / O all used to be scoped to keydown on
      // the SVG element, which only fired when the canvas had keyboard
      // focus. As soon as the user clicked the inspector, palette, or any
      // other panel, focus moved and the keys went nowhere. Promoting these
      // to a window listener — gated by the selection state and a panel
      // mounted-check (this listener cleans up on unmount) — gives the
      // expected "press Delete and the selected nodes vanish" UX regardless
      // of where focus currently sits.
      if (e.key === 'Delete' || e.key === 'Backspace') {
        const uids = [...selectedNodes()];
        if (uids.length === 0) return;
        e.preventDefault();
        for (const u of uids) removeNode(u);
        setSelectedNode(null);
        return;
      }
      if (e.key === 'Escape') {
        e.preventDefault();
        exitSubnet();
        return;
      }
      if (e.key === 'o' || e.key === 'O') {
        const uid = selectedNode();
        if (uid === null) return;
        e.preventDefault();
        connectToObjectOutput(uid);
        return;
      }
      // S = swap inputs on a 2-input node. canSwapInputs guards
      // single-letter shortcuts from firing for unrelated nodes.
      if (e.key === 's' || e.key === 'S') {
        const uid = selectedNode();
        if (uid === null) return;
        if (!canSwapInputs(uid)) return;
        e.preventDefault();
        swapInputs(uid);
        return;
      }
      // B = toggle bypass on the selected node(s). Bypassed nodes pass
      // their first input straight through — handy for A/B-comparing the
      // effect of one transformation, or disabling a half-broken node
      // while you debug downstream.
      if (e.key === 'b' || e.key === 'B') {
        const sel = selectedNodes();
        if (sel.length === 0) return;
        e.preventDefault();
        for (const u of sel) toggleNodeBypass(u);
        return;
      }
      // A = frame all nodes, F = frame selection (or all when nothing
      // selected). Matches Maya / Blender / Houdini conventions.
      if (e.key === 'a' || e.key === 'A') {
        e.preventDefault();
        frameNodes(currentGraph().nodes.map((n) => n.uid));
        return;
      }
      if (e.key === 'f' || e.key === 'F') {
        e.preventDefault();
        const sel = selectedNodes();
        frameNodes(sel.length > 0 ? sel : currentGraph().nodes.map((n) => n.uid));
        return;
      }
    };
    const onUp = (e: KeyboardEvent) => {
      if (e.key === 'Alt') setPanKeyDown(false);
      if (e.key === 'y' || e.key === 'Y') setCutKeyDown(false);
    };
    // Window blur / tab-switch can drop the keyup event; treat blur as "Alt
    // released" / "Y released" so the user doesn't end up stuck in pan or
    // cut mode.
    const onBlur = () => { setPanKeyDown(false); setCutKeyDown(false); };
    window.addEventListener('keydown', onDown);
    window.addEventListener('keyup', onUp);
    window.addEventListener('blur', onBlur);
    onCleanup(() => {
      window.removeEventListener('keydown', onDown);
      window.removeEventListener('keyup', onUp);
      window.removeEventListener('blur', onBlur);
    });
  });

  // Houdini-style "dive into" — double-click the node body. Subnets push a
  // crumb and swap the visible graph; attribute_vop opens the docked VOP
  // editor for that host. Anything else is a no-op (regular SOPs have no
  // child graph to enter).
  function onNodeDoubleClick(e: MouseEvent, node: SopNode) {
    if (node.kind === 'subnet') {
      e.stopPropagation();
      enterSubnet(node.uid);
      return;
    }
    if (node.kind === 'attribute_vop') {
      e.stopPropagation();
      openVopEditor(node.uid);
      return;
    }
  }

  // ── Right-click context menus ─────────────────────────────────────────────
  // Two distinct surfaces:
  //   • on a node : action menu (Delete, Wire to output, Enter subnet …)
  //   • on canvas : searchable add-node catalog grouped by category
  // We carry the world-space cursor in `menuWorld` so node creation drops
  // the new node where the user right-clicked, not at a stale jitter.
  type MenuState =
    | { kind: 'add'; clientX: number; clientY: number; worldX: number; worldY: number;
        // When set, the menu was triggered by releasing a port-drag on the
        // empty canvas. The chosen node is spawned at the cursor *and*
        // auto-wired so the drag direction is preserved.
        connectFrom?: PortRef }
    | { kind: 'node'; clientX: number; clientY: number; nodeUid: number }
    | { kind: 'insert'; clientX: number; clientY: number; worldX: number; worldY: number; conn: SopConnection }
    | null;
  const [menuState, setMenuState] = createSignal<MenuState>(null);

  // Parse the `data-edge="from:fromPort>to:toPort"` tag we emit on every
  // wire <path> into a SopConnection. Returns null if the attribute is
  // malformed (shouldn't happen — we always write the same shape — but
  // defensive parsing keeps a typo from crashing the menu).
  function parseEdgeAttr(s: string): SopConnection | null {
    const m = s.match(/^(\d+):(\d+)>(\d+):(\d+)$/);
    if (!m) return null;
    return {
      from_node: parseInt(m[1]),
      from_port: parseInt(m[2]),
      to_node:   parseInt(m[3]),
      to_port:   parseInt(m[4]),
    };
  }

  function onSvgContextMenu(e: MouseEvent) {
    e.preventDefault();
    const targetEl = e.target as Element | null;
    // Order of checks: node first (a node body should never be confused
    // with a wire), then wire, then fall through to empty-canvas.
    const nodeEl = targetEl?.closest?.('[data-node-uid]');
    if (nodeEl) {
      const uid = parseInt(nodeEl.getAttribute('data-node-uid') || '');
      if (!Number.isNaN(uid)) {
        if (!isNodeSelected(uid)) setSelectedNode(uid);
        setMenuState({ kind: 'node', clientX: e.clientX, clientY: e.clientY, nodeUid: uid });
        return;
      }
    }
    const edgeEl = targetEl?.closest?.('[data-edge]');
    if (edgeEl) {
      const conn = parseEdgeAttr(edgeEl.getAttribute('data-edge') || '');
      if (conn) {
        const [wx, wy] = clientToWorld(e.clientX, e.clientY);
        setMenuState({ kind: 'insert', clientX: e.clientX, clientY: e.clientY, worldX: wx, worldY: wy, conn });
        return;
      }
    }
    const [wx, wy] = clientToWorld(e.clientX, e.clientY);
    setMenuState({ kind: 'add', clientX: e.clientX, clientY: e.clientY, worldX: wx, worldY: wy });
  }

  // Build the add-node tree from the live catalog, grouped by category.
  // The category order mirrors the C++ registration order in
  // src/sops/register_builtins.cpp (Generators → Cloners → Modifiers → …),
  // which is the order the catalog ships in — so we just preserve insertion
  // order via Map.
  function buildAddMenuEntries(
    worldX: number, worldY: number,
    connectFrom?: PortRef,
  ): MenuEntry[] {
    // In add+connect mode we filter the catalog to nodes whose port
    // shape can accept the drag's source side: drags from an output
    // need a target with ≥1 input, drags from an input need a target
    // with ≥1 output. Everything else is hidden so the user can't pick
    // a node that would be silently discarded.
    const byCategory = new Map<string, MenuEntry[]>();
    for (const e of catalog()) {
      if (connectFrom) {
        if (connectFrom.kind === 'output' && e.inputs.length === 0) continue;
        if (connectFrom.kind === 'input'  && e.outputs.length === 0) continue;
      }
      const leafs = byCategory.get(e.category) ?? [];
      leafs.push({
        kind: 'leaf',
        label: e.label,
        onPick: () => {
          const node = makeNode(e.kind, [worldX, worldY]);
          if (!node) return;
          addNode(node);
          // Wire the new node into the drag's source port, picking port
          // 0 on the matching side.
          if (connectFrom) {
            if (connectFrom.kind === 'output') {
              addConnection({
                from_node: connectFrom.nodeUid, from_port: connectFrom.portIdx,
                to_node: node.uid,             to_port: 0,
              });
            } else {
              addConnection({
                from_node: node.uid,           from_port: 0,
                to_node: connectFrom.nodeUid,  to_port: connectFrom.portIdx,
              });
            }
          }
          setSelectedNode(node.uid);
        },
      });
      byCategory.set(e.category, leafs);
    }
    const out: MenuEntry[] = [];
    for (const [cat, entries] of byCategory) {
      out.push({ kind: 'category', label: cat, entries });
    }
    return out;
  }

  // Insert-on-wire menu. Same shape as the add menu, but filtered to
  // node kinds that actually have at least one input AND one output —
  // generators (0 inputs) or terminals (object_output, light: 0 outputs)
  // can't sit in the middle of a wire. The pick action splices the wire:
  // removes the original connection and adds (from → newNode in0) +
  // (newNode out0 → to).
  // Swap-inputs helpers. Available for any node with exactly 2 input
  // ports AND both connected — the typical case is binary math/blend
  // nodes (Mix, Subtract, Divide, …) where the operand order matters
  // and the user wants to flip A and B without rewiring by hand. Single
  // edge sinks let us do this by removing both incoming edges and
  // re-adding them with swapped to_port values.
  function canSwapInputs(uid: number): boolean {
    const node = currentGraph().nodes.find((n) => n.uid === uid);
    if (!node) return false;
    if (inputPortCount(node.kind) !== 2) return false;
    const conns = currentGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    return !!c0 && !!c1;
  }
  function swapInputs(uid: number): void {
    const conns = currentGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    if (!c0 || !c1) return;
    removeConnection(c0);
    removeConnection(c1);
    addConnection({ ...c0, to_port: 1 });
    addConnection({ ...c1, to_port: 0 });
  }

  function buildInsertMenuEntries(worldX: number, worldY: number, conn: SopConnection): MenuEntry[] {
    const byCategory = new Map<string, MenuEntry[]>();
    for (const e of catalog()) {
      if (e.inputs.length === 0 || e.outputs.length === 0) continue;
      const leafs = byCategory.get(e.category) ?? [];
      leafs.push({
        kind: 'leaf',
        label: e.label,
        onPick: () => {
          const node = makeNode(e.kind, [worldX, worldY]);
          if (!node) return;
          addNode(node);
          // Order: remove old, then add new two. addConnection replaces
          // any existing edge on the destination port (single-edge sink
          // semantics) — but since we already removed the original, the
          // input edges we add are unique.
          removeConnection(conn);
          addConnection({ from_node: conn.from_node, from_port: conn.from_port, to_node: node.uid,         to_port: 0 });
          addConnection({ from_node: node.uid,        from_port: 0,             to_node: conn.to_node,    to_port: conn.to_port });
          // Splay-apart: shift the upstream chain back along the flow
          // axis (Y in the top-to-bottom SOP layout) and the downstream
          // chain forward, so the new node has room to live without
          // overlapping its neighbours.
          const g = currentGraph();
          const moves = computeInsertShift({
            fromUid: conn.from_node,
            toUid: conn.to_node,
            insertedUid: node.uid,
            insertedExtent: nodeHeight(node),
            connections: g.connections,
          });
          for (const m of moves) {
            const n = g.nodes.find((x) => x.uid === m.uid);
            if (!n || !n.pos) continue;
            moveNode(m.uid, n.pos[0], n.pos[1] + m.delta);
          }
          setSelectedNode(node.uid);
        },
      });
      byCategory.set(e.category, leafs);
    }
    const out: MenuEntry[] = [];
    for (const [cat, entries] of byCategory) {
      out.push({ kind: 'category', label: cat, entries });
    }
    return out;
  }

  // "Replace With" submenu — every catalog entry grouped by category, but
  // excluding the node's own kind (replacing with self is a no-op) and
  // (optionally) entries whose port shape would force dropping every
  // current edge. We keep the picker permissive: kinds with too few ports
  // appear too, since `replaceNode` will silently drop edges that don't
  // fit, and the user may want to do exactly that.
  function buildReplaceMenu(uid: number): MenuEntry {
    const node = currentGraph().nodes.find((n) => n.uid === uid);
    const byCategory = new Map<string, MenuEntry[]>();
    for (const e of catalog()) {
      if (node && e.kind === node.kind) continue;
      const leafs = byCategory.get(e.category) ?? [];
      leafs.push({
        kind: 'leaf',
        label: e.label,
        onPick: () => replaceNode(uid, e.kind),
      });
      byCategory.set(e.category, leafs);
    }
    const entries: MenuEntry[] = [];
    for (const [cat, leafs] of byCategory) {
      entries.push({ kind: 'category', label: cat, entries: leafs });
    }
    return { kind: 'category', label: 'Replace With', entries };
  }

  // Per-node action menu. Items vary by node kind:
  //   • subnet         → "Enter subnet"
  //   • attribute_vop  → "Open VOP editor"
  //   • everything     → "Wire to object_output" (no-op if none exists),
  //                       "Delete" (handles multi-selection automatically).
  function buildNodeMenuEntries(uid: number): MenuEntry[] {
    const node = currentGraph().nodes.find((n) => n.uid === uid);
    const entries: MenuEntry[] = [];
    if (node?.kind === 'subnet') {
      entries.push({
        kind: 'leaf',
        label: 'Enter Subnet',
        hint: 'dbl-click',
        onPick: () => enterSubnet(uid),
      });
    }
    if (node?.kind === 'attribute_vop') {
      entries.push({
        kind: 'leaf',
        label: 'Open VOP Editor',
        hint: 'dbl-click',
        onPick: () => openVopEditor(uid),
      });
    }
    entries.push({
      kind: 'leaf',
      label: 'Wire to Object Output',
      hint: 'O',
      onPick: () => connectToObjectOutput(uid),
    });
    if (canSwapInputs(uid)) {
      entries.push({
        kind: 'leaf',
        label: 'Swap Inputs',
        hint: 'S',
        onPick: () => swapInputs(uid),
      });
    }
    entries.push({
      kind: 'leaf',
      label: node?.bypass ? 'Un-bypass Node' : 'Bypass Node',
      hint: 'B',
      onPick: () => toggleNodeBypass(uid),
    });
    entries.push(buildReplaceMenu(uid));
    // Delete acts on the full selection (so right-clicking one of several
    // selected nodes still deletes them all), mirroring the keyboard path.
    entries.push({
      kind: 'leaf',
      label: selectedNodes().length > 1
        ? `Delete ${selectedNodes().length} Nodes`
        : 'Delete Node',
      hint: '⌫',
      onPick: () => {
        const uids = [...selectedNodes()];
        if (uids.length === 0) uids.push(uid);
        for (const u of uids) removeNode(u);
        setSelectedNode(null);
      },
    });
    return entries;
  }

  return (<>
    <svg
      class="graph-canvas"
      classList={{
        'graph-canvas--panning': panKeyDown(),
        'graph-canvas--cutting': cutKeyDown(),
      }}
      ref={svgRef}
      onPointerDown={onSvgPointerDown}
      onPointerMove={onSvgPointerMove}
      onWheel={onWheel}
      onContextMenu={onSvgContextMenu}
    >
      <defs>
        <pattern id="grid-sop" width="40" height="40" patternUnits="userSpaceOnUse">
          <path d="M 40 0 L 0 0 0 40" fill="none" stroke="#2a2a2a" stroke-width="1" />
        </pattern>
        {/* Hard-clip long node titles to the header rect. SVG <text> has no
            text-overflow:ellipsis, so we cap rendering at the node's right
            edge (with a small inset) instead. The full label is still
            available via the <title> tooltip emitted per-node below. */}
        <clipPath id="sop-title-clip">
          <rect x={4} y={0} width={NODE_WIDTH - 8} height={NODE_HEADER_HEIGHT} />
        </clipPath>
      </defs>
      <rect width="100%" height="100%" fill="url(#grid-sop)" />
      <g transform={`translate(${pan()[0]} ${pan()[1]}) scale(${zoom()})`}>
        {/* Edges. Two paths per wire: a transparent thick `graph-edge-hit`
            path catches right-click for the insert-on-wire menu (visible
            stroke is only 2px wide — too thin to target reliably), and
            the slim `graph-edge` on top is the visible wire. The hit path
            carries the `data-edge` tag the context-menu handler reads. */}
        <For each={currentGraph().connections}>
          {(c: SopConnection) => {
            const fromNode = () => currentGraph().nodes.find((n) => n.uid === c.from_node);
            const toNode = () => currentGraph().nodes.find((n) => n.uid === c.to_node);
            return (
              <Show when={fromNode() && toNode()}>
                <>
                  <path
                    class="graph-edge-hit"
                    data-edge={`${c.from_node}:${c.from_port}>${c.to_node}:${c.to_port}`}
                    d={bezier(
                      outputAnchor(fromNode()!, c.from_port),
                      inputAnchor(toNode()!, c.to_port)
                    )}
                  />
                  <path
                    class="graph-edge"
                    d={bezier(
                      outputAnchor(fromNode()!, c.from_port),
                      inputAnchor(toNode()!, c.to_port)
                    )}
                  />
                </>
              </Show>
            );
          }}
        </For>

        <Show when={pendingFrom()}>
          {() => {
            const ref = pendingFrom()!;
            const node = currentGraph().nodes.find((n) => n.uid === ref.nodeUid);
            if (!node) return null;
            // Bezier direction depends on which side of the node the
            // drag started from. From an output port, the wire flows out
            // and toward the cursor; from an input port, the cursor is
            // the (yet-unknown) source so the wire flows from cursor
            // into the input.
            const sourceAnchor = ref.kind === 'output'
              ? outputAnchor(node, ref.portIdx)
              : inputAnchor(node, ref.portIdx);
            // Magnetic snap: when a compatible port is within radius,
            // the rubber-band's free end jumps to that port's anchor so
            // the user gets a visible "this is what release will hit"
            // preview. Falls back to following the cursor when no snap.
            const snap = snapTarget();
            let freeEnd = mouseWorld();
            if (snap) {
              const snapNode = currentGraph().nodes.find((n) => n.uid === snap.nodeUid);
              if (snapNode) {
                freeEnd = snap.kind === 'input'
                  ? inputAnchor(snapNode, snap.portIdx)
                  : outputAnchor(snapNode, snap.portIdx);
              }
            }
            const [from, to] = ref.kind === 'output'
              ? [sourceAnchor, freeEnd]
              : [freeEnd, sourceAnchor];
            return (
              <path
                class="graph-edge graph-edge-pending"
                classList={{ 'graph-edge-pending-snapped': !!snap }}
                d={bezier(from, to)}
              />
            );
          }}
        </Show>

        {/* Nodes */}
        <For each={currentGraph().nodes}>
          {(node: SopNode) => {
            const [x, y] = nodeOrigin(node);
            const h = nodeHeight(node);
            const ins = inputPortCount(node.kind);
            const outs = outputPortCount(node.kind);
            const selected = () => isNodeSelected(node.uid);
            const bypassed = () => node.bypass === true;
            const category = () => lookupCatalog(node.kind)?.category ?? '';
            return (
              <g
                transform={`translate(${x} ${y})`}
                data-node-uid={node.uid}
                class={`graph-node graph-node-sop-${category()} ${selected() ? 'graph-node-selected' : ''} ${bypassed() ? 'graph-node-bypassed' : ''}`}
                onDblClick={(e) => onNodeDoubleClick(e, node)}
              >
                <rect class="graph-node-body" width={NODE_WIDTH} height={h} rx={6} />
                <rect class="graph-node-header" width={NODE_WIDTH} height={NODE_HEADER_HEIGHT} rx={6} />
                <text
                  class="graph-node-title"
                  x={10}
                  y={NODE_HEADER_HEIGHT * 0.65}
                  clip-path="url(#sop-title-clip)"
                >
                  {nodeLabel(node)}
                  <title>{nodeLabel(node)}</title>
                </text>
                <For each={Array.from({ length: ins }, (_, i) => i)}>
                  {(i) => {
                    const isSnap = () => {
                      const s = snapTarget();
                      return s !== null && s.nodeUid === node.uid &&
                             s.portIdx === i && s.kind === 'input';
                    };
                    return (
                    <>
                      <circle
                        class="graph-port graph-port-input"
                        classList={{ 'graph-port-snap': isSnap() }}
                        data-port-kind="input"
                        data-port-idx={i}
                        cx={portX(i, ins)}
                        cy={0}
                        r={PORT_RADIUS}
                        onPointerDown={(e) => startPortDrag(e, node.uid, i, 'input')}
                      >
                        <title>{inputPortName(node.kind, i) || `in ${i}`}</title>
                      </circle>
                      <text
                        class="graph-port-label graph-port-label-input"
                        x={portX(i, ins)}
                        y={-PORT_RADIUS - 3}
                        text-anchor="middle"
                        pointer-events="none"
                      >
                        {inputPortName(node.kind, i)}
                      </text>
                    </>
                    );
                  }}
                </For>
                <For each={Array.from({ length: outs }, (_, i) => i)}>
                  {(i) => {
                    const isSnap = () => {
                      const s = snapTarget();
                      return s !== null && s.nodeUid === node.uid &&
                             s.portIdx === i && s.kind === 'output';
                    };
                    return (
                    <>
                      <circle
                        class="graph-port graph-port-output"
                        classList={{ 'graph-port-snap': isSnap() }}
                        data-port-kind="output"
                        data-port-idx={i}
                        cx={portX(i, outs)}
                        cy={h}
                        r={PORT_RADIUS}
                        onPointerDown={(e) => startPortDrag(e, node.uid, i, 'output')}
                      >
                        <title>{outputPortName(node.kind, i) || `out ${i}`}</title>
                      </circle>
                      <text
                        class="graph-port-label graph-port-label-output"
                        x={portX(i, outs)}
                        y={h + PORT_RADIUS + 9}
                        text-anchor="middle"
                        pointer-events="none"
                      >
                        {outputPortName(node.kind, i)}
                      </text>
                    </>
                    );
                  }}
                </For>
              </g>
            );
          }}
        </For>

        {/* Marquee rectangle. Rendered last so it overlays nodes; the
            pointer-events:none CSS keeps it from intercepting clicks. */}
        <Show when={marquee()}>
          {() => {
            const r = marquee()!;
            return (
              <rect
                class="graph-marquee"
                x={r.minX}
                y={r.minY}
                width={r.maxX - r.minX}
                height={r.maxY - r.minY}
              />
            );
          }}
        </Show>

        {/* Cut-mode trail. The dashed red polyline follows the cursor and
            disappears on pointerup. Connections actually get removed as the
            cursor crosses them, so the trail is purely visual feedback. */}
        <Show when={cutPath().length >= 2}>
          <polyline
            class="graph-cut-path"
            points={cutPath().map((p) => `${p[0]},${p[1]}`).join(' ')}
          />
        </Show>
      </g>
    </svg>
    <Show when={menuState()}>
      {(state) => {
        const s = state();
        if (s.kind === 'node') {
          return (
            <ContextMenu
              x={s.clientX}
              y={s.clientY}
              entries={buildNodeMenuEntries(s.nodeUid)}
              onClose={() => setMenuState(null)}
            />
          );
        }
        if (s.kind === 'insert') {
          return (
            <ContextMenu
              x={s.clientX}
              y={s.clientY}
              showSearch
              searchPlaceholder="Insert node on wire…"
              entries={buildInsertMenuEntries(s.worldX, s.worldY, s.conn)}
              onClose={() => setMenuState(null)}
            />
          );
        }
        // 'add' (possibly add+connect when the menu was triggered from
        // a port-drag release on the empty canvas).
        return (
          <ContextMenu
            x={s.clientX}
            y={s.clientY}
            showSearch
            searchPlaceholder={s.connectFrom ? 'Add & connect…' : 'Add node…'}
            entries={buildAddMenuEntries(s.worldX, s.worldY, s.connectFrom)}
            onClose={() => {
              setMenuState(null);
              // Dismissing the add+connect menu cancels the in-flight
              // port-drag, so the rubber-band wire doesn't linger.
              setPendingFrom(null);
            }}
          />
        );
      }}
    </Show>
  </>);
};
