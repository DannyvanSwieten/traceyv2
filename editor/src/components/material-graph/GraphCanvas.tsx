import { Component, For, createSignal, onCleanup, onMount, Show } from 'solid-js';
import {
  Node,
  Connection,
  inputPortCount,
  outputPortCount,
  PALETTE,
  allocNodeUid,
} from '../../lib/material_graph';
import {
  materialGraph,
  addNode,
  moveNode,
  addConnection,
  removeConnection,
  removeNode,
  selectedNode,
  selectedNodes,
  setSelectedNode,
  setSelectedNodes,
  toggleSelectedNode,
  isNodeSelected,
} from '../../stores/materials';
import {
  rectFromCorners,
  nodesInRect,
  type MarqueeRect,
  type NodeBox,
} from '../../lib/graph_canvas_marquee';
import { ContextMenu, type MenuEntry } from '../context-menu/ContextMenu';
import {
  sampleCubicBezier,
  intersectingConnections,
  type ConnPolyline,
  type Pt,
} from '../../lib/graph_cut';
import { computeInsertShift } from '../../lib/graph_insert_layout';
import { computeFrame, type FrameRect } from '../../lib/graph_frame';
import './GraphCanvas.css';

const NODE_WIDTH = 180;
const NODE_HEADER_HEIGHT = 28;
const PORT_ROW_HEIGHT = 22;
const PORT_RADIUS = 6;

// World-space port positions (relative to the node origin in graph coords).
function inputPortY(idx: number): number {
  return NODE_HEADER_HEIGHT + idx * PORT_ROW_HEIGHT + PORT_ROW_HEIGHT / 2;
}
function outputPortY(idx: number): number {
  // Outputs go on the right edge, indexed from the bottom of the inputs.
  return NODE_HEADER_HEIGHT + idx * PORT_ROW_HEIGHT + PORT_ROW_HEIGHT / 2;
}
function nodeHeight(node: Node): number {
  const ins = inputPortCount(node.kind);
  const outs = outputPortCount(node.kind);
  return NODE_HEADER_HEIGHT + Math.max(ins, outs) * PORT_ROW_HEIGHT + 8;
}

function nodeOrigin(node: Node): [number, number] {
  return node.position ? [node.position[0], node.position[1]] : [0, 0];
}

function inputAnchor(node: Node, portIdx: number): [number, number] {
  const [x, y] = nodeOrigin(node);
  return [x, y + inputPortY(portIdx)];
}
function outputAnchor(node: Node, portIdx: number): [number, number] {
  const [x, y] = nodeOrigin(node);
  return [x + NODE_WIDTH, y + outputPortY(portIdx)];
}

function bezier(from: [number, number], to: [number, number]): string {
  const [x1, y1] = from;
  const [x2, y2] = to;
  const dx = Math.max(40, Math.abs(x2 - x1) * 0.5);
  return `M ${x1} ${y1} C ${x1 + dx} ${y1}, ${x2 - dx} ${y2}, ${x2} ${y2}`;
}

// Short label for a node header. Kinds with `op` show the op; the others show
// kind-specific data (the constant/parameter value or name).
function nodeLabel(n: Node): string {
  switch (n.kind) {
    case 'Constant':
      return `Const (${n.value.map((v) => v.toFixed(2)).join(', ')})`;
    case 'Parameter':
      return `Param: ${n.name}`;
    default:
      return n.op;
  }
}

// Kind-tagged port ref — see SopGraphCanvas for the rationale.
interface PortRef { nodeUid: number; portIdx: number; kind: 'input' | 'output' }

// Module-scoped clipboard — see SopGraphCanvas for the rationale.
interface MatClipboard {
  nodes: Node[];
  connections: Connection[];
  originX: number;
  originY: number;
}
let matClipboard: MatClipboard | null = null;

export const GraphCanvas: Component = () => {
  const [pan, setPan] = createSignal<[number, number]>([0, 0]);
  const [zoom, setZoom] = createSignal(1);
  const [pendingFrom, setPendingFrom] = createSignal<PortRef | null>(null);
  // Magnetic port snap: while dragging a wire, attract the rubber-band's
  // free end to the nearest compatible-kind port within a world-space
  // radius. See SopGraphCanvas for the full rationale.
  const [snapTarget, setSnapTarget] = createSignal<PortRef | null>(null);
  const [mouseWorld, setMouseWorld] = createSignal<[number, number]>([0, 0]);
  // Houdini-style: hold Alt to pan; empty-canvas drag without Alt starts a
  // rubber-band selection. Alt was picked over Space because the editor's
  // timeline owns Space for play/pause.
  const [panKeyDown, setPanKeyDown] = createSignal(false);
  const [marquee, setMarquee] = createSignal<MarqueeRect | null>(null);
  // Cut mode: hold Y, drag to cut every connection the cursor crosses.
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

  // Single pointerdown handler on the SVG dispatches based on which element
  // was actually hit. Avoids any SVG <g>-level event binding quirks where
  // child rect/text events don't reach the group's onPointerDown reliably.
  function onSvgPointerDown(e: PointerEvent) {
    if (e.button !== 0) return;
    e.preventDefault();
    if (cutKeyDown()) { startCutDrag(e); return; }
    // Alt-held drag pans (e.altKey covers a "first click before keydown fired"
    // race when focus was elsewhere).
    if (panKeyDown() || e.altKey) { startCanvasPan(e); return; }
    const targetEl = e.target as Element;
    if (targetEl.closest?.('[data-port-kind]')) {
      // Ports use the click event for connection creation; skip drag setup.
      return;
    }
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

  // Cut-mode bezier sampling. Material wires curve left-to-right just like
  // VOP wires — control handles offset along X. See SopGraphCanvas for the
  // full design notes.
  function snapshotConnectionPolylines(): ConnPolyline<Connection>[] {
    const g = materialGraph();
    const byUid = new Map<number, Node>();
    for (const n of g.nodes) byUid.set(n.uid, n);
    const out: ConnPolyline<Connection>[] = [];
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
    const keyOf = (c: Connection) =>
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
      const boxes: NodeBox<number>[] = materialGraph().nodes.map((n) => ({
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
      // Keep the world point under the cursor stable across zoom.
      const ratio = newZoom / z;
      setPan([cx - (cx - px) * ratio, cy - (cy - py) * ratio]);
    }
    setZoom(newZoom);
  }

  // Node drag: translate every selected node by the cursor's world-space
  // delta. Cmd/Ctrl/Shift+click on a node toggles its membership in the
  // selection instead of starting a drag.
  function startNodeDrag(e: PointerEvent, uid: number) {
    const multi = e.metaKey || e.ctrlKey || e.shiftKey;
    if (multi) { toggleSelectedNode(uid); return; }
    if (!isNodeSelected(uid)) setSelectedNode(uid);

    const [startX, startY] = clientToWorld(e.clientX, e.clientY);
    const origPositions = new Map<number, [number, number]>();
    for (const u of selectedNodes()) {
      const n = materialGraph().nodes.find((nd) => nd.uid === u);
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

  // See SopGraphCanvas for the design notes. Material nodes are 180 world-
  // units wide, so 60 is roughly "if the cursor is within a third of a node,
  // snap" — slightly looser than the SOP canvas because material nodes have
  // more vertical port stacking and the user has to dodge more neighbours.
  const SNAP_RADIUS = 60;
  function findSnapTarget(from: PortRef, mx: number, my: number): PortRef | null {
    const targetKind: 'input' | 'output' = from.kind === 'output' ? 'input' : 'output';
    const r2 = SNAP_RADIUS * SNAP_RADIUS;
    let best: PortRef | null = null;
    let bestD2 = r2;
    for (const node of materialGraph().nodes) {
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
  function startPortDrag(e: PointerEvent, nodeUid: number, portIdx: number,
                         kind: 'input' | 'output') {
    e.stopPropagation();
    e.preventDefault();
    setPendingFrom({ nodeUid, portIdx, kind });
    setSnapTarget(null);

    const onMove = (mv: PointerEvent) => {
      const [wx, wy] = clientToWorld(mv.clientX, mv.clientY);
      setMouseWorld([wx, wy]);
      const from = pendingFrom();
      setSnapTarget(from ? findSnapTarget(from, wx, wy) : null);
    };
    const onUp = (mv: PointerEvent) => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);

      const from = pendingFrom();
      if (!from) { setSnapTarget(null); return; }

      // Magnetic snap takes priority — see SopGraphCanvas.
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

  // Copy / paste / duplicate — mirror of the SOP variant. Material nodes
  // use `position` rather than `pos`, and addNode takes a fully-built Node.
  function copySelection() {
    const uids = new Set(selectedNodes());
    if (uids.size === 0) return;
    const g = materialGraph();
    const nodes: Node[] = [];
    let minX = Infinity, minY = Infinity;
    for (const n of g.nodes) {
      if (!uids.has(n.uid)) continue;
      nodes.push(structuredClone(n));
      const [x, y] = n.position ?? [0, 0];
      if (x < minX) minX = x;
      if (y < minY) minY = y;
    }
    if (nodes.length === 0) return;
    const connections = g.connections
      .filter((c) => uids.has(c.from_node) && uids.has(c.to_node))
      .map((c) => structuredClone(c));
    matClipboard = {
      nodes, connections,
      originX: minX === Infinity ? 0 : minX,
      originY: minY === Infinity ? 0 : minY,
    };
  }
  function pasteAt(targetX: number, targetY: number) {
    if (!matClipboard || matClipboard.nodes.length === 0) return;
    const remap = new Map<number, number>();
    const newUids: number[] = [];
    const dx = targetX - matClipboard.originX;
    const dy = targetY - matClipboard.originY;
    for (const src of matClipboard.nodes) {
      const newUid = allocNodeUid();
      remap.set(src.uid, newUid);
      newUids.push(newUid);
      const srcPos = src.position ?? [0, 0];
      const clone: Node = {
        ...structuredClone(src),
        uid: newUid,
        position: [srcPos[0] + dx, srcPos[1] + dy],
      } as Node;
      addNode(clone);
    }
    for (const c of matClipboard.connections) {
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
    const saved = matClipboard;
    copySelection();
    if (matClipboard) {
      pasteAt(matClipboard.originX + 30, matClipboard.originY + 30);
    }
    matClipboard = saved ?? matClipboard;
  }

  // Frame the given node uids inside the viewport. See SopGraphCanvas.
  function frameNodes(uids: number[]) {
    if (!svgRef) return;
    const g = materialGraph();
    const rects: FrameRect[] = [];
    for (const uid of uids) {
      const n = g.nodes.find((x) => x.uid === uid);
      if (!n || !n.position) continue;
      const [x, y] = n.position;
      const h = nodeHeight(n);
      rects.push({ minX: x, minY: y, maxX: x + NODE_WIDTH, maxY: y + h });
    }
    const rect = svgRef.getBoundingClientRect();
    const r = computeFrame(rects, rect.width, rect.height);
    if (!r) return;
    setPan(r.pan);
    setZoom(r.zoom);
  }

  // Window-level Alt tracking — see SopGraphCanvas for the rationale.
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
      // Alt = pan modifier (Space is reserved for the timeline).
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
          setSelectedNodes(materialGraph().nodes.map((n) => n.uid));
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
      // Delete on window (not SVG focus) — same rationale as SopGraphCanvas.
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
        frameNodes(materialGraph().nodes.map((n) => n.uid));
        return;
      }
      if (e.key === 'f' || e.key === 'F') {
        e.preventDefault();
        const sel = selectedNodes();
        frameNodes(sel.length > 0 ? sel : materialGraph().nodes.map((n) => n.uid));
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

  // Right-click menus — material graph version. Uses the static PALETTE
  // tree from lib/material_graph.ts instead of a fetched catalog.
  type MenuState =
    | { kind: 'add'; clientX: number; clientY: number; worldX: number; worldY: number;
        connectFrom?: PortRef }
    | { kind: 'node'; clientX: number; clientY: number; nodeUid: number }
    | { kind: 'insert'; clientX: number; clientY: number; worldX: number; worldY: number; conn: Connection }
    | null;
  const [menuState, setMenuState] = createSignal<MenuState>(null);

  function parseEdgeAttr(s: string): Connection | null {
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
    // PALETTE is grouped at the source. In add+connect mode, probe each
    // entry's port shape via a throwaway factory call to filter out
    // nodes that can't satisfy the drag direction.
    return PALETTE.map((g) => ({
      kind: 'category' as const,
      label: g.group,
      entries: g.entries
        .map((p) => {
          if (connectFrom) {
            const probe = p.factory([0, 0]);
            const ins = inputPortCount(probe.kind);
            const outs = outputPortCount(probe.kind);
            if (connectFrom.kind === 'output' && ins === 0) return null;
            if (connectFrom.kind === 'input'  && outs === 0) return null;
          }
          return {
            kind: 'leaf' as const,
            label: p.label,
            onPick: () => {
              const node = p.factory([worldX, worldY]);
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
          };
        })
        .filter((e): e is MenuEntry => e !== null),
    })).filter((g) => g.entries.length > 0);
  }

  // Insert-on-wire: filter PALETTE to nodes with ≥1 input and ≥1 output,
  // splice the wire on pick. inputPortCount / outputPortCount probe the
  // node *after* construction; we run them on a throwaway node placed at
  // (0,0) since they only look at `kind`. The throwaway is discarded.
  // Swap-inputs helpers — see SopGraphCanvas for the rationale.
  function canSwapInputs(uid: number): boolean {
    const node = materialGraph().nodes.find((n) => n.uid === uid);
    if (!node) return false;
    if (inputPortCount(node.kind) !== 2) return false;
    const conns = materialGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    return !!c0 && !!c1;
  }
  function swapInputs(uid: number): void {
    const conns = materialGraph().connections;
    const c0 = conns.find((c) => c.to_node === uid && c.to_port === 0);
    const c1 = conns.find((c) => c.to_node === uid && c.to_port === 1);
    if (!c0 || !c1) return;
    removeConnection(c0);
    removeConnection(c1);
    addConnection({ ...c0, to_port: 1 });
    addConnection({ ...c1, to_port: 0 });
  }

  function buildInsertMenuEntries(worldX: number, worldY: number, conn: Connection): MenuEntry[] {
    return PALETTE.map((g) => ({
      kind: 'category' as const,
      label: g.group,
      entries: g.entries
        .map((p) => {
          // Probe port counts via a temp node so we can filter terminals
          // (no inputs OR no outputs) out of the insert menu.
          const probe = p.factory([0, 0]);
          const ins = inputPortCount(probe.kind);
          const outs = outputPortCount(probe.kind);
          if (ins === 0 || outs === 0) return null;
          return {
            kind: 'leaf' as const,
            label: p.label,
            onPick: () => {
              const node = p.factory([worldX, worldY]);
              addNode(node);
              removeConnection(conn);
              addConnection({ from_node: conn.from_node, from_port: conn.from_port, to_node: node.uid,      to_port: 0 });
              addConnection({ from_node: node.uid,        from_port: 0,             to_node: conn.to_node, to_port: conn.to_port });
              // Splay-apart along L→R flow axis.
              const g = materialGraph();
              const moves = computeInsertShift({
                fromUid: conn.from_node,
                toUid: conn.to_node,
                insertedUid: node.uid,
                insertedExtent: NODE_WIDTH,
                connections: g.connections,
              });
              for (const m of moves) {
                const n = g.nodes.find((x) => x.uid === m.uid);
                if (!n || !n.position) continue;
                moveNode(m.uid, n.position[0] + m.delta, n.position[1]);
              }
              setSelectedNode(node.uid);
            },
          };
        })
        .filter((e): e is MenuEntry => e !== null),
    })).filter((g) => g.entries.length > 0);
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
      classList={{ 'graph-canvas--panning': panKeyDown() }}
      ref={svgRef}
      onPointerDown={onSvgPointerDown}
      onPointerMove={onSvgPointerMove}
      onWheel={onWheel}
      onContextMenu={onSvgContextMenu}
    >
      <defs>
        <pattern id="grid" width="40" height="40" patternUnits="userSpaceOnUse">
          <path d="M 40 0 L 0 0 0 40" fill="none" stroke="#2a2a2a" stroke-width="1"/>
        </pattern>
        {/* See SopGraphCanvas for the rationale — hard-clips long titles. */}
        <clipPath id="mat-title-clip">
          <rect x={4} y={0} width={NODE_WIDTH - 8} height={NODE_HEADER_HEIGHT} />
        </clipPath>
      </defs>
      <rect width="100%" height="100%" fill="url(#grid)" />
      <g transform={`translate(${pan()[0]} ${pan()[1]}) scale(${zoom()})`}>
        {/* Edges. Two paths per wire: transparent thick hit-area + visible
            thin stroke. See SopGraphCanvas for the rationale. */}
        <For each={materialGraph().connections}>
          {(c: Connection) => {
            const fromNode = () => materialGraph().nodes.find((n) => n.uid === c.from_node);
            const toNode = () => materialGraph().nodes.find((n) => n.uid === c.to_node);
            return (
              <Show when={fromNode() && toNode()}>
                <>
                  <path
                    class="graph-edge-hit"
                    data-edge={`${c.from_node}:${c.from_port}>${c.to_node}:${c.to_port}`}
                    d={bezier(outputAnchor(fromNode()!, c.from_port),
                              inputAnchor(toNode()!, c.to_port))}
                  />
                  <path
                    class="graph-edge"
                    d={bezier(outputAnchor(fromNode()!, c.from_port),
                              inputAnchor(toNode()!, c.to_port))}
                  />
                </>
              </Show>
            );
          }}
        </For>

        {/* In-progress connection (cursor-following). Direction follows
            the drag origin — see SopGraphCanvas. */}
        <Show when={pendingFrom()}>
          {() => {
            const ref = pendingFrom()!;
            const node = materialGraph().nodes.find((n) => n.uid === ref.nodeUid);
            if (!node) return null;
            const sourceAnchor = ref.kind === 'output'
              ? outputAnchor(node, ref.portIdx)
              : inputAnchor(node, ref.portIdx);
            // Magnetic snap pins the rubber-band's free end to the
            // nearest compatible port when in range.
            const snap = snapTarget();
            let freeEnd = mouseWorld();
            if (snap) {
              const snapNode = materialGraph().nodes.find((n) => n.uid === snap.nodeUid);
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
        <For each={materialGraph().nodes}>
          {(node: Node) => {
            const [x, y] = nodeOrigin(node);
            const h = nodeHeight(node);
            const ins = inputPortCount(node.kind);
            const outs = outputPortCount(node.kind);
            const selected = () => isNodeSelected(node.uid);
            return (
              <g
                transform={`translate(${x} ${y})`}
                data-node-uid={node.uid}
                class={`graph-node graph-node-${node.kind} ${selected() ? 'graph-node-selected' : ''}`}
              >
                <rect class="graph-node-body" width={NODE_WIDTH} height={h} rx={6} />
                <rect class="graph-node-header" width={NODE_WIDTH} height={NODE_HEADER_HEIGHT} rx={6} />
                <text
                  class="graph-node-title"
                  x={10}
                  y={NODE_HEADER_HEIGHT * 0.65}
                  clip-path="url(#mat-title-clip)"
                >
                  {nodeLabel(node)}
                  <title>{nodeLabel(node)}</title>
                </text>
                {/* Input ports (left side) */}
                <For each={Array.from({ length: ins }, (_, i) => i)}>
                  {(i) => {
                    const isSnap = () => {
                      const s = snapTarget();
                      return s !== null && s.nodeUid === node.uid &&
                             s.portIdx === i && s.kind === 'input';
                    };
                    return (
                    <circle
                      class="graph-port graph-port-input"
                      classList={{ 'graph-port-snap': isSnap() }}
                      data-port-kind="input"
                      data-port-idx={i}
                      cx={0}
                      cy={inputPortY(i)}
                      r={PORT_RADIUS}
                      onPointerDown={(e) => startPortDrag(e, node.uid, i, 'input')}
                    />
                    );
                  }}
                </For>
                {/* Output ports (right side) */}
                <For each={Array.from({ length: outs }, (_, i) => i)}>
                  {(i) => {
                    const isSnap = () => {
                      const s = snapTarget();
                      return s !== null && s.nodeUid === node.uid &&
                             s.portIdx === i && s.kind === 'output';
                    };
                    return (
                    <circle
                      class="graph-port graph-port-output"
                      classList={{ 'graph-port-snap': isSnap() }}
                      data-port-kind="output"
                      data-port-idx={i}
                      cx={NODE_WIDTH}
                      cy={outputPortY(i)}
                      r={PORT_RADIUS}
                      onPointerDown={(e) => startPortDrag(e, node.uid, i, 'output')}
                    />
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
        return (
          <ContextMenu
            x={s.clientX}
            y={s.clientY}
            showSearch
            searchPlaceholder={s.connectFrom ? 'Add & connect…' : 'Add material node…'}
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
