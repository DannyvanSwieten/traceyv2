// SVG-based pan/zoom/wires canvas for the VOP graph. Copy of
// SopGraphCanvas with imports rebound to the VOP store / catalog. VOPs do
// not nest, so the subnet enter/exit logic is omitted. Shares the
// material-graph stylesheet for theme parity.

import { Component, For, createSignal, onCleanup, onMount, Show } from 'solid-js';
import {
  VopNode,
  VopConnection,
  inputPortCount,
  outputPortCount,
  inputPortName,
  outputPortName,
  lookupCatalog,
  catalog,
  makeNode,
  allocNodeUid,
} from '../../lib/vop_graph';
import {
  vopGraph,
  addNode,
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
  isNodeSelected,
} from '../../stores/vops';
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

// VOP nodes flow left-to-right: inputs on the left edge, outputs on the
// right. Slightly more compact than the SOP canvas because VOP graphs
// tend to be denser (one bind-node per attribute) and need to fit a row
// of passthroughs comfortably side-by-side.
const NODE_WIDTH = 110;
const NODE_HEADER_HEIGHT = 20;
const PORT_ROW_HEIGHT = 16;
const PORT_RADIUS = 5;

function inputPortY(idx: number): number {
  return NODE_HEADER_HEIGHT + idx * PORT_ROW_HEIGHT + PORT_ROW_HEIGHT / 2;
}
function outputPortY(idx: number): number {
  return NODE_HEADER_HEIGHT + idx * PORT_ROW_HEIGHT + PORT_ROW_HEIGHT / 2;
}
function nodeHeight(node: VopNode): number {
  const ins = inputPortCount(node.kind);
  const outs = outputPortCount(node.kind);
  return NODE_HEADER_HEIGHT + Math.max(ins, outs, 1) * PORT_ROW_HEIGHT + 8;
}
function nodeOrigin(node: VopNode): [number, number] {
  return [node.pos?.[0] ?? 0, node.pos?.[1] ?? 0];
}
function inputAnchor(node: VopNode, portIdx: number): [number, number] {
  const [x, y] = nodeOrigin(node);
  return [x, y + inputPortY(portIdx)];
}
function outputAnchor(node: VopNode, portIdx: number): [number, number] {
  const [x, y] = nodeOrigin(node);
  return [x + NODE_WIDTH, y + outputPortY(portIdx)];
}
function bezier(from: [number, number], to: [number, number]): string {
  const [x1, y1] = from;
  const [x2, y2] = to;
  const dx = Math.max(40, Math.abs(x2 - x1) * 0.5);
  return `M ${x1} ${y1} C ${x1 + dx} ${y1}, ${x2 - dx} ${y2}, ${x2} ${y2}`;
}
function nodeLabel(n: VopNode): string {
  return lookupCatalog(n.kind)?.label ?? n.kind;
}

// See SopGraphCanvas for the kind-tagged port-ref rationale.
interface PortRef { nodeUid: number; portIdx: number; kind: 'input' | 'output' }

// Module-scoped clipboard — see SopGraphCanvas for the rationale.
interface VopClipboard {
  nodes: VopNode[];
  connections: VopConnection[];
  originX: number;
  originY: number;
}
let vopClipboard: VopClipboard | null = null;

export const VopGraphCanvas: Component = () => {
  const [pan, setPan] = createSignal<[number, number]>([0, 0]);
  const [zoom, setZoom] = createSignal(1);
  const [pendingFrom, setPendingFrom] = createSignal<PortRef | null>(null);
  // Magnetic port snap: while dragging a wire, attract the rubber-band's
  // free end to the nearest compatible-kind port within a world-space
  // radius. See SopGraphCanvas for the full rationale.
  const [snapTarget, setSnapTarget] = createSignal<PortRef | null>(null);
  const [mouseWorld, setMouseWorld] = createSignal<[number, number]>([0, 0]);
  // Houdini-style: hold Alt to pan; empty-canvas drag without Alt starts
  // a rubber-band selection.
  const [panKeyDown, setPanKeyDown] = createSignal(false);
  const [marquee, setMarquee] = createSignal<MarqueeRect | null>(null);
  // Cut mode: hold Y to swap the cursor to scissors and drag-cut every
  // connection the cursor crosses. See SopGraphCanvas for the design.
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
    // Cut takes priority over every other gesture; see SopGraphCanvas.
    if (cutKeyDown()) { startCutDrag(e); return; }
    // Alt-held drag pans (e.altKey covers the case where Alt was pressed
    // while focus was elsewhere and the keydown listener missed it).
    if (panKeyDown() || e.altKey) { startCanvasPan(e); return; }
    const targetEl = e.target as Element;
    if (targetEl.closest?.('[data-port-kind]')) return;
    const nodeEl = targetEl.closest?.('[data-node-uid]');
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

  // Snapshot every connection as a sampled polyline so the cut-path can
  // test segment intersections cheaply during the drag. See
  // SopGraphCanvas:snapshotConnectionPolylines for the full rationale.
  // VOP wires curve left-to-right so the control handles offset on X.
  function snapshotConnectionPolylines(): ConnPolyline<VopConnection>[] {
    const g = vopGraph();
    const byUid = new Map<number, VopNode>();
    for (const n of g.nodes) byUid.set(n.uid, n);
    const out: ConnPolyline<VopConnection>[] = [];
    for (const c of g.connections) {
      const a = byUid.get(c.from_node);
      const b = byUid.get(c.to_node);
      if (!a || !b) continue;
      const p0 = outputAnchor(a, c.from_port);
      const p3 = inputAnchor(b, c.to_port);
      const dx = Math.max(40, Math.abs(p3[0] - p0[0]) * 0.5);
      const p1: Pt = [p0[0] + dx, p0[1]];
      const p2: Pt = [p3[0] - dx, p3[1]];
      out.push({ key: c, points: sampleCubicBezier(p0, p1, p2, p3, 16) });
    }
    return out;
  }

  function startCutDrag(e: PointerEvent) {
    const polylines = snapshotConnectionPolylines();
    const cutHits = new Set<string>();
    const keyOf = (c: VopConnection) =>
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
      const dx = rect.maxX - rect.minX;
      const dy = rect.maxY - rect.minY;
      if (dx < 1 && dy < 1) return;
      const boxes: NodeBox<number>[] = vopGraph().nodes.map((n) => ({
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
    if (multi) { toggleSelectedNode(uid); return; }
    if (!isNodeSelected(uid)) setSelectedNode(uid);

    const [startX, startY] = clientToWorld(e.clientX, e.clientY);
    const origPositions = new Map<number, [number, number]>();
    for (const u of selectedNodes()) {
      const n = vopGraph().nodes.find((nd) => nd.uid === u);
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

  // See SopGraphCanvas for the full rationale on this radius and the
  // snap algorithm. VOP nodes are ~110 world-units wide, so 50 is roughly
  // "if the cursor is within half a node, snap".
  const SNAP_RADIUS = 50;
  function findSnapTarget(from: PortRef, mx: number, my: number): PortRef | null {
    const targetKind: 'input' | 'output' = from.kind === 'output' ? 'input' : 'output';
    const r2 = SNAP_RADIUS * SNAP_RADIUS;
    let best: PortRef | null = null;
    let bestD2 = r2;
    for (const node of vopGraph().nodes) {
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

  // Drag-from-port wiring — see SopGraphCanvas for the design notes.
  // Supports both drag-connect and click-click connect.
  function startPortDrag(e: PointerEvent, nodeUid: number, portIdx: number,
                         kind: 'input' | 'output') {
    e.stopPropagation();
    e.preventDefault();

    // If a connection is already pending from a prior click-without-drag,
    // treat this pointerdown as the completion click.
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
    const DRAG_THRESHOLD = 4;
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

      // Magnetic snap takes priority — see SopGraphCanvas. Honour the
      // visible snap state at release time so "let go close enough" works.
      const snap = snapTarget();
      if (snap) {
        completeConnectionFromDrag(from, snap.nodeUid, snap.portIdx, snap.kind);
        setPendingFrom(null);
        setSnapTarget(null);
        return;
      }

      const dropEl = document.elementFromPoint(mv.clientX, mv.clientY) as Element | null;
      const portEl = dropEl?.closest?.('[data-port-kind]');
      const nodeEl = dropEl?.closest?.('[data-node-uid]');

      // Click on source port without drag → enter click-click mode.
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
      if (nodeEl) { setPendingFrom(null); return; }

      // Empty canvas — drag opens add+connect menu, click cancels.
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
    if (from.kind === dropKind) return;
    if (from.nodeUid === dropNodeUid) return;
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

  // Copy / paste / duplicate — mirror of the SOP variant.
  function copySelection() {
    const uids = new Set(selectedNodes());
    if (uids.size === 0) return;
    const g = vopGraph();
    const nodes: VopNode[] = [];
    let minX = Infinity, minY = Infinity;
    for (const n of g.nodes) {
      if (!uids.has(n.uid)) continue;
      nodes.push(structuredClone(n));
      const [x, y] = n.pos ?? [0, 0];
      if (x < minX) minX = x;
      if (y < minY) minY = y;
    }
    if (nodes.length === 0) return;
    const connections = g.connections
      .filter((c) => uids.has(c.from_node) && uids.has(c.to_node))
      .map((c) => structuredClone(c));
    vopClipboard = {
      nodes, connections,
      originX: minX === Infinity ? 0 : minX,
      originY: minY === Infinity ? 0 : minY,
    };
  }
  function pasteAt(targetX: number, targetY: number) {
    if (!vopClipboard || vopClipboard.nodes.length === 0) return;
    const remap = new Map<number, number>();
    const newUids: number[] = [];
    const dx = targetX - vopClipboard.originX;
    const dy = targetY - vopClipboard.originY;
    for (const src of vopClipboard.nodes) {
      const newUid = allocNodeUid();
      remap.set(src.uid, newUid);
      newUids.push(newUid);
      const srcPos = src.pos ?? [0, 0];
      const clone: VopNode = {
        ...structuredClone(src),
        uid: newUid,
        pos: [srcPos[0] + dx, srcPos[1] + dy],
      };
      addNode(clone);
    }
    for (const c of vopClipboard.connections) {
      const fn = remap.get(c.from_node);
      const tn = remap.get(c.to_node);
      if (fn === undefined || tn === undefined) continue;
      addConnection({ from_node: fn, from_port: c.from_port, to_node: tn, to_port: c.to_port });
    }
    setSelectedNodes(newUids);
  }
  function duplicateSelection() {
    const uids = selectedNodes();
    if (uids.length === 0) return;
    const saved = vopClipboard;
    copySelection();
    if (vopClipboard) {
      pasteAt(vopClipboard.originX + 30, vopClipboard.originY + 30);
    }
    vopClipboard = saved ?? vopClipboard;
  }

  // Frame the given node uids inside the viewport. See SopGraphCanvas.
  function frameNodes(uids: number[]) {
    if (!svgRef) return;
    const g = vopGraph();
    const rects: FrameRect[] = [];
    for (const uid of uids) {
      const n = g.nodes.find((x) => x.uid === uid);
      if (!n || !n.pos) continue;
      const [x, y] = n.pos;
      const h = nodeHeight(n);
      rects.push({ minX: x, minY: y, maxX: x + NODE_WIDTH, maxY: y + h });
    }
    const rect = svgRef.getBoundingClientRect();
    const r = computeFrame(rects, rect.width, rect.height);
    if (!r) return;
    setPan(r.pan);
    setZoom(r.zoom);
  }

  // Window-level Alt tracking — see SopGraphCanvas for the rationale (SVG
  // focus is too narrow for a global hold-to-pan gesture; window listeners
  // catch the key regardless of which panel actually owns focus).
  onMount(() => {
    const isTextEditing = (target: EventTarget | null): boolean => {
      const el = target as HTMLElement | null;
      if (!el) return false;
      const tag = el.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return true;
      return el.isContentEditable === true;
    };
    const onDown = (e: KeyboardEvent) => {
      if (isTextEditing(e.target)) return;
      // Alt = pan modifier (timeline uses Space).
      if (e.key === 'Alt' || e.altKey) {
        if (!e.repeat) setPanKeyDown(true);
        return;
      }
      // Y = hold to cut connections (Houdini convention).
      if (e.key === 'y' || e.key === 'Y') {
        if (!e.repeat) setCutKeyDown(true);
        return;
      }

      // Cmd/Ctrl combos — see SopGraphCanvas for rationale.
      if (e.metaKey || e.ctrlKey) {
        const k = e.key.toLowerCase();
        if (k === 'a') {
          e.preventDefault();
          setSelectedNodes(vopGraph().nodes.map((n) => n.uid));
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
        return;
      }
      // Delete on window (not SVG focus) so the key still works after the
      // user clicks the inspector — same rationale as SopGraphCanvas.
      if (e.key === 'Delete' || e.key === 'Backspace') {
        const uids = [...selectedNodes()];
        if (uids.length === 0) return;
        e.preventDefault();
        for (const u of uids) removeNode(u);
        setSelectedNode(null);
        return;
      }
      // S = swap inputs on a 2-input node.
      if (e.key === 's' || e.key === 'S') {
        const uid = selectedNode();
        if (uid === null || !canSwapInputs(uid)) return;
        e.preventDefault();
        swapInputs(uid);
        return;
      }
      // A / F = frame all / frame selection. See SopGraphCanvas.
      if (e.key === 'a' || e.key === 'A') {
        e.preventDefault();
        frameNodes(vopGraph().nodes.map((n) => n.uid));
        return;
      }
      if (e.key === 'f' || e.key === 'F') {
        e.preventDefault();
        const sel = selectedNodes();
        frameNodes(sel.length > 0 ? sel : vopGraph().nodes.map((n) => n.uid));
        return;
      }
    };
    const onUp = (e: KeyboardEvent) => {
      if (e.key === 'Alt') setPanKeyDown(false);
      if (e.key === 'y' || e.key === 'Y') setCutKeyDown(false);
    };
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

  // Right-click menus — mirror of the SOP variant, minus the subnet /
  // attribute_vop dive actions which don't apply to VOP nodes.
  type MenuState =
    | { kind: 'add'; clientX: number; clientY: number; worldX: number; worldY: number;
        connectFrom?: PortRef }
    | { kind: 'node'; clientX: number; clientY: number; nodeUid: number }
    | { kind: 'insert'; clientX: number; clientY: number; worldX: number; worldY: number; conn: VopConnection }
    | null;
  const [menuState, setMenuState] = createSignal<MenuState>(null);

  function parseEdgeAttr(s: string): VopConnection | null {
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

  function buildAddMenuEntries(
    worldX: number, worldY: number,
    connectFrom?: PortRef,
  ): MenuEntry[] {
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

  // Insert-on-wire menu (filters to kinds with ≥1 input AND ≥1 output, then
  // splices the wire on pick — see SopGraphCanvas for the full rationale).
  // Swap-inputs helpers — see SopGraphCanvas for the rationale.
  function canSwapInputs(uid: number): boolean {
    const node = vopGraph().nodes.find((n) => n.uid === uid);
    if (!node) return false;
    if (inputPortCount(node.kind) !== 2) return false;
    const conns = vopGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    return !!c0 && !!c1;
  }
  function swapInputs(uid: number): void {
    const conns = vopGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    if (!c0 || !c1) return;
    removeConnection(c0);
    removeConnection(c1);
    addConnection({ ...c0, to_port: 1 });
    addConnection({ ...c1, to_port: 0 });
  }

  function buildInsertMenuEntries(worldX: number, worldY: number, conn: VopConnection): MenuEntry[] {
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
          removeConnection(conn);
          addConnection({ from_node: conn.from_node, from_port: conn.from_port, to_node: node.uid,      to_port: 0 });
          addConnection({ from_node: node.uid,        from_port: 0,             to_node: conn.to_node, to_port: conn.to_port });
          // Splay-apart along the L→R flow axis: upstream chain slides
          // left, downstream slides right, leaving room for the inserted
          // node at the cursor.
          const g = vopGraph();
          const moves = computeInsertShift({
            fromUid: conn.from_node,
            toUid: conn.to_node,
            insertedUid: node.uid,
            insertedExtent: NODE_WIDTH,
            connections: g.connections,
          });
          for (const m of moves) {
            const n = g.nodes.find((x) => x.uid === m.uid);
            if (!n || !n.pos) continue;
            moveNode(m.uid, n.pos[0] + m.delta, n.pos[1]);
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

  function buildReplaceMenu(uid: number): MenuEntry {
    const node = vopGraph().nodes.find((n) => n.uid === uid);
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

  function buildNodeMenuEntries(uid: number): MenuEntry[] {
    const entries: MenuEntry[] = [];
    if (canSwapInputs(uid)) {
      entries.push({
        kind: 'leaf',
        label: 'Swap Inputs',
        hint: 'S',
        onPick: () => swapInputs(uid),
      });
    }
    entries.push(buildReplaceMenu(uid));
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
        <pattern id="grid-vop" width="40" height="40" patternUnits="userSpaceOnUse">
          <path d="M 40 0 L 0 0 0 40" fill="none" stroke="#2a2a2a" stroke-width="1" />
        </pattern>
        {/* See SopGraphCanvas for the rationale — hard-clips long titles. */}
        <clipPath id="vop-title-clip">
          <rect x={4} y={0} width={NODE_WIDTH - 8} height={NODE_HEADER_HEIGHT} />
        </clipPath>
      </defs>
      <rect width="100%" height="100%" fill="url(#grid-vop)" />
      <g transform={`translate(${pan()[0]} ${pan()[1]}) scale(${zoom()})`}>
        {/* Edges. Two paths per wire: transparent thick hit-area + visible
            thin stroke. See SopGraphCanvas for the rationale. */}
        <For each={vopGraph().connections}>
          {(c: VopConnection) => {
            const fromNode = () => vopGraph().nodes.find((n) => n.uid === c.from_node);
            const toNode = () => vopGraph().nodes.find((n) => n.uid === c.to_node);
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
            const node = vopGraph().nodes.find((n) => n.uid === ref.nodeUid);
            if (!node) return null;
            // Direction follows the drag origin — see SopGraphCanvas.
            const sourceAnchor = ref.kind === 'output'
              ? outputAnchor(node, ref.portIdx)
              : inputAnchor(node, ref.portIdx);
            // Magnetic snap pins the rubber-band's free end to the
            // nearest compatible port when in range.
            const snap = snapTarget();
            let freeEnd = mouseWorld();
            if (snap) {
              const snapNode = vopGraph().nodes.find((n) => n.uid === snap.nodeUid);
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
        <For each={vopGraph().nodes}>
          {(node: VopNode) => {
            const [x, y] = nodeOrigin(node);
            const h = nodeHeight(node);
            const ins = inputPortCount(node.kind);
            const outs = outputPortCount(node.kind);
            const selected = () => isNodeSelected(node.uid);
            const category = () => lookupCatalog(node.kind)?.category ?? '';
            return (
              <g
                transform={`translate(${x} ${y})`}
                data-node-uid={node.uid}
                class={`graph-node graph-node-vop-${category()} ${selected() ? 'graph-node-selected' : ''}`}
              >
                <rect class="graph-node-body" width={NODE_WIDTH} height={h} rx={6} />
                <rect class="graph-node-header" width={NODE_WIDTH} height={NODE_HEADER_HEIGHT} rx={6} />
                <text
                  class="graph-node-title"
                  x={10}
                  y={NODE_HEADER_HEIGHT * 0.65}
                  clip-path="url(#vop-title-clip)"
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
                        cx={0}
                        cy={inputPortY(i)}
                        r={PORT_RADIUS}
                        onPointerDown={(e) => startPortDrag(e, node.uid, i, 'input')}
                      >
                        <title>{inputPortName(node.kind, i) || `in ${i}`}</title>
                      </circle>
                      {/* Labels sit inside the node body next to each port —
                          inputs left-aligned just right of the dot, outputs
                          right-aligned just left of the dot. pointer-events
                          is none so clicks fall through to the circle. */}
                      <text
                        class="graph-port-label graph-port-label-input"
                        x={PORT_RADIUS + 3}
                        y={inputPortY(i) + 3}
                        text-anchor="start"
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
                        cx={NODE_WIDTH}
                        cy={outputPortY(i)}
                        r={PORT_RADIUS}
                        onPointerDown={(e) => startPortDrag(e, node.uid, i, 'output')}
                      >
                        <title>{outputPortName(node.kind, i) || `out ${i}`}</title>
                      </circle>
                      <text
                        class="graph-port-label graph-port-label-output"
                        x={NODE_WIDTH - PORT_RADIUS - 3}
                        y={outputPortY(i) + 3}
                        text-anchor="end"
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
              searchPlaceholder="Insert VOP node on wire…"
              entries={buildInsertMenuEntries(s.worldX, s.worldY, s.conn)}
              onClose={() => setMenuState(null)}
            />
          );
        }
        return (
          <ContextMenu
            x={s.clientX}
            y={s.clientY}
            showSearch
            searchPlaceholder={s.connectFrom ? 'Add & connect…' : 'Add VOP node…'}
            entries={buildAddMenuEntries(s.worldX, s.worldY, s.connectFrom)}
            onClose={() => {
              setMenuState(null);
              setPendingFrom(null);
            }}
          />
        );
      }}
    </Show>
  </>);
};
