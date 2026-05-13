// Universal SVG graph canvas — the one rendering implementation shared
// across SOP / VOP / DOP / Material graph editors. All canvas-level
// interactions (pan, zoom, marquee select, cut mode, frame-to-fit,
// multi-node drag, drag-to-connect with magnetic port snap, click-click
// connect, delete shortcut, alt-pan, copy/paste keys) live here.
//
// Adapter-driven: the canvas reads graph + selection through `props.adapter`
// and mutates only via the adapter's setters. Optional adapter callbacks
// (`onNodeContextMenu`, `onWireContextMenu`, `onEmptyContextMenu`,
// `onWireDropEmpty`, `onNodeDoubleClick`) let wrappers wire up node
// palettes and store-specific menu actions without the canvas knowing
// anything about node kinds.
//
// Optional `handle?` prop receives a GraphCanvasHandle exposing pan/zoom
// + frame() so wrappers can drive the camera from outside (toolbar
// buttons, command palette).

import {
  Component,
  For,
  Show,
  createSignal,
  onCleanup,
  onMount,
} from 'solid-js';
import {
  DEFAULT_HORIZONTAL,
  GraphGeometryConfig,
  inputAnchor,
  outputAnchor,
  bezierPath,
  nodeHeight,
  portY,
} from '../../lib/graph_geometry';
import {
  MarqueeRect,
  NodeBox,
  nodesInRect,
  rectFromCorners,
} from '../../lib/graph_canvas_marquee';
import {
  ConnPolyline,
  Pt,
  intersectingConnections,
  sampleCubicBezier,
} from '../../lib/graph_cut';
import { computeFrame, FrameRect } from '../../lib/graph_frame';
import '../material-graph/GraphCanvas.css';

export interface CanvasNode {
  uid: number;
  kind: string;
  pos: [number, number];
}

export interface CanvasConnection {
  from_node: number;
  from_port: number;
  to_node:   number;
  to_port:   number;
}

export interface PortRef {
  nodeUid: number;
  portIdx: number;
  kind: 'input' | 'output';
}

// Context-menu callback target: what was right-clicked.
export type ContextMenuTarget =
  | { kind: 'node'; uid: number }
  | { kind: 'wire'; conn: CanvasConnection }
  | { kind: 'empty' };

export interface ContextMenuPayload {
  target: ContextMenuTarget;
  clientX: number;
  clientY: number;
  worldX:  number;
  worldY:  number;
  // Present when a wire-drag landed in empty space — the wrapper can
  // open an "add and connect" menu seeded from this port.
  connectFrom?: PortRef;
}

// Imperative handle exposed via `props.handle?.(h => …)`. Wrappers use
// this to drive the camera from outside the SVG (toolbar buttons,
// keyboard shortcuts owned by the wrapper).
export interface GraphCanvasHandle {
  pan(): [number, number];
  zoom(): number;
  setPan(p: [number, number]): void;
  setZoom(z: number): void;
  worldFromClient(clientX: number, clientY: number): [number, number];
  frame(uids?: number[]): void;
}

export interface GraphCanvasAdapter<N extends CanvasNode = CanvasNode> {
  // Reactive accessors (call inside render; Solid will track).
  graph(): { nodes: N[]; connections: CanvasConnection[] };
  selectedNodes(): number[];
  isNodeSelected(uid: number): boolean;

  // Mutations
  moveNode(uid: number, x: number, y: number): void;
  removeNode(uid: number): void;
  addConnection(c: CanvasConnection): void;
  removeConnection(c: CanvasConnection): void;
  setSelectedNode(uid: number | null): void;
  setSelectedNodes(uids: number[]): void;
  toggleSelectedNode(uid: number): void;

  // Catalog
  inputPortCount(kind: string): number;
  outputPortCount(kind: string): number;
  inputPortName(kind: string, idx: number): string;
  outputPortName(kind: string, idx: number): string;
  nodeLabel(node: N): string;

  // Optional category-driven node-tinting hook — the wrapper can return
  // a CSS class suffix (e.g. node category name) to style nodes by kind.
  // GraphCanvas appends "graph-node-cat-<value>" when this returns
  // non-empty.
  nodeCategoryClass?(node: N): string;

  // Optional "bypassed" indicator. When this returns true GraphCanvas
  // adds `graph-node-bypassed` to the node's class list. SOPs use this
  // to dim bypassed nodes so the user can see at a glance that the
  // transformation is currently disabled.
  isBypassed?(node: N): boolean;

  // Optional callbacks
  onNodeDoubleClick?(node: N): void;
  onContextMenu?(payload: ContextMenuPayload): void;
}

export interface GraphCanvasProps<N extends CanvasNode = CanvasNode> {
  adapter: GraphCanvasAdapter<N>;
  geometry?: GraphGeometryConfig;
  // Imperative handle escape hatch — receives an object once on mount.
  handle?: (h: GraphCanvasHandle) => void;
}

// Magnetic port snap radius. World-space pixels: same scale as node
// dimensions, so ~half the node width is reasonable.
const SNAP_RADIUS = 50;

// Pixels of cursor movement before a port-drag stops being a click.
const DRAG_THRESHOLD = 4;

export function GraphCanvas<N extends CanvasNode = CanvasNode>(
  props: GraphCanvasProps<N>,
): ReturnType<Component> {
  const cfg = (): GraphGeometryConfig => props.geometry ?? DEFAULT_HORIZONTAL;
  const ad = (): GraphCanvasAdapter<N> => props.adapter;

  const [pan, setPan] = createSignal<[number, number]>([0, 0]);
  const [zoom, setZoom] = createSignal(1);
  const [pendingFrom, setPendingFrom] = createSignal<PortRef | null>(null);
  const [snapTarget, setSnapTarget] = createSignal<PortRef | null>(null);
  const [mouseWorld, setMouseWorld] = createSignal<[number, number]>([0, 0]);
  const [marquee, setMarquee] = createSignal<MarqueeRect | null>(null);
  const [cutPath, setCutPath] = createSignal<Pt[]>([]);
  // Hold-state for modifier keys tracked at the window level so the
  // gesture stays active even when the SVG doesn't have focus.
  const [panKeyDown, setPanKeyDown] = createSignal(false);
  const [cutKeyDown, setCutKeyDown] = createSignal(false);

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

  // ── Pan ────────────────────────────────────────────────────────────
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

  // ── Cut ────────────────────────────────────────────────────────────
  function snapshotConnectionPolylines(): ConnPolyline<CanvasConnection>[] {
    const adp = ad();
    const g = adp.graph();
    const byUid = new Map<number, N>();
    for (const n of g.nodes) byUid.set(n.uid, n);
    const out: ConnPolyline<CanvasConnection>[] = [];
    for (const c of g.connections) {
      const a = byUid.get(c.from_node);
      const b = byUid.get(c.to_node);
      if (!a || !b) continue;
      const p0 = outputAnchor(a.pos, c.from_port, adp.outputPortCount(a.kind), cfg());
      const p3 = inputAnchor(b.pos, c.to_port, adp.inputPortCount(b.kind), cfg());
      // Orientation-aware control handles so the polyline matches the
      // visible wire — VOP/DOP curve on X, SOP curves on Y.
      let p1: Pt, p2: Pt;
      if (cfg().orientation === 'vertical') {
        const dy = Math.max(40, Math.abs(p3[1] - p0[1]) * 0.5);
        p1 = [p0[0], p0[1] + dy];
        p2 = [p3[0], p3[1] - dy];
      } else {
        const dx = Math.max(40, Math.abs(p3[0] - p0[0]) * 0.5);
        p1 = [p0[0] + dx, p0[1]];
        p2 = [p3[0] - dx, p3[1]];
      }
      out.push({ key: c, points: sampleCubicBezier(p0, p1, p2, p3, 16) });
    }
    return out;
  }

  function startCutDrag(e: PointerEvent) {
    const polylines = snapshotConnectionPolylines();
    const cutHits = new Set<string>();
    const keyOf = (c: CanvasConnection) =>
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
        ad().removeConnection(c);
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

  // ── Marquee select ─────────────────────────────────────────────────
  function startMarqueeSelect(e: PointerEvent) {
    const additive = e.metaKey || e.ctrlKey || e.shiftKey;
    const preExisting = additive ? [...ad().selectedNodes()] : [];
    if (!additive) ad().setSelectedNode(null);

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
      const a = ad();
      const boxes: NodeBox<number>[] = a.graph().nodes.map((n) => ({
        uid: n.uid,
        x: n.pos[0],
        y: n.pos[1],
        width: cfg().nodeWidth,
        height: nodeHeight(
          { inputCount: a.inputPortCount(n.kind), outputCount: a.outputPortCount(n.kind) },
          cfg(),
        ),
      }));
      const hits = nodesInRect(rect, boxes);
      a.setSelectedNodes(additive ? [...preExisting, ...hits] : hits);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  }

  // ── Multi-node drag ────────────────────────────────────────────────
  function startNodeDrag(e: PointerEvent, uid: number) {
    const a = ad();
    const multi = e.metaKey || e.ctrlKey || e.shiftKey;
    if (multi) { a.toggleSelectedNode(uid); return; }
    if (!a.isNodeSelected(uid)) a.setSelectedNode(uid);

    const [startX, startY] = clientToWorld(e.clientX, e.clientY);
    const origPositions = new Map<number, [number, number]>();
    for (const u of a.selectedNodes()) {
      const n = a.graph().nodes.find((nd) => nd.uid === u);
      if (n) origPositions.set(u, [n.pos[0], n.pos[1]]);
    }
    if (origPositions.size === 0) return;

    const onMove = (mv: PointerEvent) => {
      const [wx, wy] = clientToWorld(mv.clientX, mv.clientY);
      const dx = wx - startX;
      const dy = wy - startY;
      for (const [u, [ox, oy]] of origPositions) {
        a.moveNode(u, ox + dx, oy + dy);
      }
    };
    const onUp = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  }

  // ── Port drag → connection / click-click connect ───────────────────
  function findSnapTarget(from: PortRef, mx: number, my: number): PortRef | null {
    const a = ad();
    const targetKind: 'input' | 'output' = from.kind === 'output' ? 'input' : 'output';
    const r2 = SNAP_RADIUS * SNAP_RADIUS;
    let best: PortRef | null = null;
    let bestD2 = r2;
    for (const node of a.graph().nodes) {
      if (node.uid === from.nodeUid) continue;
      const count = targetKind === 'input'
        ? a.inputPortCount(node.kind)
        : a.outputPortCount(node.kind);
      for (let i = 0; i < count; i++) {
        const [ax, ay] = targetKind === 'input'
          ? inputAnchor(node.pos, i, count, cfg())
          : outputAnchor(node.pos, i, count, cfg());
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

  function completeConnection(from: PortRef, dropNodeUid: number,
                              dropPortIdx: number, dropKind: 'input' | 'output') {
    if (from.kind === dropKind) return;
    if (from.nodeUid === dropNodeUid) return;
    if (from.kind === 'output') {
      ad().addConnection({
        from_node: from.nodeUid, from_port: from.portIdx,
        to_node: dropNodeUid,    to_port: dropPortIdx,
      });
    } else {
      ad().addConnection({
        from_node: dropNodeUid,  from_port: dropPortIdx,
        to_node: from.nodeUid,   to_port: from.portIdx,
      });
    }
  }

  function startPortDrag(e: PointerEvent, nodeUid: number, portIdx: number,
                         kind: 'input' | 'output') {
    e.stopPropagation();
    e.preventDefault();

    // If a connection is already pending from a prior click-without-drag,
    // treat this pointerdown as the completion click.
    const existing = pendingFrom();
    if (existing && (existing.nodeUid !== nodeUid || existing.kind !== kind)) {
      completeConnection(existing, nodeUid, portIdx, kind);
      setPendingFrom(null);
      return;
    }

    setPendingFrom({ nodeUid, portIdx, kind });
    setSnapTarget(null);
    const startX = e.clientX;
    const startY = e.clientY;
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

      const snap = snapTarget();
      if (snap) {
        completeConnection(from, snap.nodeUid, snap.portIdx, snap.kind);
        setPendingFrom(null);
        setSnapTarget(null);
        return;
      }

      const dropEl = document.elementFromPoint(mv.clientX, mv.clientY) as Element | null;
      const portEl = dropEl?.closest?.('[data-port-kind]');
      const nodeEl = dropEl?.closest?.('[data-node-uid]');

      // Click on the source port without drag → enter click-click mode.
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
          completeConnection(from, dropNodeUid, dropPortIdx, dropKind);
        }
        setPendingFrom(null);
        return;
      }
      if (nodeEl) { setPendingFrom(null); return; }

      if (!moved) { setPendingFrom(null); return; }
      // Drag from port released in empty space — invite the wrapper to
      // open an "add and connect" menu seeded from this port.
      const [wx, wy] = clientToWorld(mv.clientX, mv.clientY);
      ad().onContextMenu?.({
        target: { kind: 'empty' },
        clientX: mv.clientX, clientY: mv.clientY,
        worldX: wx, worldY: wy,
        connectFrom: from,
      });
      setPendingFrom(null);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  }

  // ── Pointer / wheel root handlers ──────────────────────────────────
  function onSvgPointerDown(e: PointerEvent) {
    if (e.button !== 0) return;
    e.preventDefault();
    // Cut > pan > node drag > port drag > marquee. Cut and pan take
    // priority because they're modifier-keyed gestures.
    if (cutKeyDown()) { startCutDrag(e); return; }
    if (panKeyDown() || e.altKey) { startCanvasPan(e); return; }
    const targetEl = e.target as Element;
    if (targetEl.closest?.('[data-port-kind]')) return;
    const nodeEl = targetEl.closest?.('[data-node-uid]');
    if (nodeEl) {
      const uid = parseInt(nodeEl.getAttribute('data-node-uid') || '');
      if (!Number.isNaN(uid)) { startNodeDrag(e, uid); return; }
    }
    if (pendingFrom()) { setPendingFrom(null); return; }
    startMarqueeSelect(e);
  }

  function onSvgPointerMove(e: PointerEvent) {
    setMouseWorld(clientToWorld(e.clientX, e.clientY));
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

  function onSvgContextMenu(e: MouseEvent) {
    e.preventDefault();
    const cb = ad().onContextMenu;
    if (!cb) return;
    const targetEl = e.target as Element | null;
    const nodeEl = targetEl?.closest?.('[data-node-uid]');
    const [wx, wy] = clientToWorld(e.clientX, e.clientY);
    if (nodeEl) {
      const uid = parseInt(nodeEl.getAttribute('data-node-uid') || '');
      if (!Number.isNaN(uid)) {
        if (!ad().isNodeSelected(uid)) ad().setSelectedNode(uid);
        cb({ target: { kind: 'node', uid }, clientX: e.clientX, clientY: e.clientY,
             worldX: wx, worldY: wy });
        return;
      }
    }
    const edgeEl = targetEl?.closest?.('[data-edge]');
    if (edgeEl) {
      const conn = parseEdgeAttr(edgeEl.getAttribute('data-edge') || '');
      if (conn) {
        cb({ target: { kind: 'wire', conn }, clientX: e.clientX, clientY: e.clientY,
             worldX: wx, worldY: wy });
        return;
      }
    }
    cb({ target: { kind: 'empty' }, clientX: e.clientX, clientY: e.clientY,
         worldX: wx, worldY: wy });
  }

  // ── Frame to fit ───────────────────────────────────────────────────
  function frameNodes(uids?: number[]) {
    if (!svgRef) return;
    const a = ad();
    const g = a.graph();
    const sel = uids ?? a.selectedNodes();
    const target = sel.length > 0 ? sel : g.nodes.map((n) => n.uid);
    const rects: FrameRect[] = [];
    for (const uid of target) {
      const n = g.nodes.find((x) => x.uid === uid);
      if (!n) continue;
      const ins = a.inputPortCount(n.kind);
      const outs = a.outputPortCount(n.kind);
      const h = nodeHeight({ inputCount: ins, outputCount: outs }, cfg());
      rects.push({ minX: n.pos[0], minY: n.pos[1],
                   maxX: n.pos[0] + cfg().nodeWidth, maxY: n.pos[1] + h });
    }
    const rect = svgRef.getBoundingClientRect();
    const r = computeFrame(rects, rect.width, rect.height);
    if (!r) return;
    setPan(r.pan);
    setZoom(r.zoom);
  }

  // ── Window-level keyboard shortcuts ────────────────────────────────
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
      if (e.key === 'Alt' || e.altKey) {
        if (!e.repeat) setPanKeyDown(true);
        return;
      }
      if (e.key === 'y' || e.key === 'Y') {
        if (!e.repeat) setCutKeyDown(true);
        return;
      }
      if (e.metaKey || e.ctrlKey) {
        const k = e.key.toLowerCase();
        if (k === 'a') {
          e.preventDefault();
          ad().setSelectedNodes(ad().graph().nodes.map((n) => n.uid));
          return;
        }
        return;
      }
      if (e.key === 'Delete' || e.key === 'Backspace') {
        const uids = [...ad().selectedNodes()];
        if (uids.length === 0) return;
        e.preventDefault();
        for (const u of uids) ad().removeNode(u);
        ad().setSelectedNode(null);
        return;
      }
      if (e.key === 'a' || e.key === 'A') {
        e.preventDefault();
        frameNodes(ad().graph().nodes.map((n) => n.uid));
        return;
      }
      if (e.key === 'f' || e.key === 'F') {
        e.preventDefault();
        frameNodes();
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

    // Expose the imperative handle once.
    props.handle?.({
      pan, zoom, setPan, setZoom,
      worldFromClient: clientToWorld,
      frame: frameNodes,
    });
  });

  return (
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
        <pattern id="graph-canvas-grid" width="40" height="40" patternUnits="userSpaceOnUse">
          <path d="M 40 0 L 0 0 0 40" fill="none" stroke="#2a2a2a" stroke-width="1" />
        </pattern>
        <clipPath id="graph-canvas-title-clip">
          <rect x={4} y={0} width={cfg().nodeWidth - 8} height={cfg().headerHeight} />
        </clipPath>
      </defs>
      <rect width="100%" height="100%" fill="url(#graph-canvas-grid)" />
      <g transform={`translate(${pan()[0]} ${pan()[1]}) scale(${zoom()})`}>
        {/* Wires. Two paths each: a transparent thick hit-area + the
            visible thin stroke. The hit-area makes clicks easier and
            also serves as the contextmenu / delete target. */}
        <For each={ad().graph().connections}>
          {(c) => {
            const a = ad();
            const fromNode = () => a.graph().nodes.find((n) => n.uid === c.from_node);
            const toNode   = () => a.graph().nodes.find((n) => n.uid === c.to_node);
            const d = () => {
              const fn = fromNode(); const tn = toNode();
              if (!fn || !tn) return '';
              const p0 = outputAnchor(fn.pos, c.from_port, a.outputPortCount(fn.kind), cfg());
              const p1 = inputAnchor (tn.pos, c.to_port,   a.inputPortCount (tn.kind), cfg());
              return bezierPath(p0, p1, cfg());
            };
            return (
              <Show when={fromNode() && toNode()}>
                <>
                  <path
                    class="graph-edge-hit"
                    data-edge={`${c.from_node}:${c.from_port}>${c.to_node}:${c.to_port}`}
                    d={d()}
                  />
                  <path class="graph-edge" d={d()} />
                </>
              </Show>
            );
          }}
        </For>

        {/* Pending wire (during drag) with magnetic-snap end. */}
        <Show when={pendingFrom()} keyed>
          {(ref) => {
            // The `keyed` Show only re-runs this function when `ref` (the
            // source port) changes — not on every mouseWorld() update.
            // So we compute the source anchor ONCE here (it doesn't move
            // during a wire drag), and put the changing end of the wire
            // inside an accessor that the `d` attribute reads — Solid
            // wraps that read in a memo and re-evaluates on every
            // mouseWorld() / snapTarget() change.
            const a = ad();
            const node = a.graph().nodes.find((n) => n.uid === ref.nodeUid);
            if (!node) return null;
            const totalFrom = ref.kind === 'output'
              ? a.outputPortCount(node.kind)
              : a.inputPortCount(node.kind);
            const sourceAnchor = ref.kind === 'output'
              ? outputAnchor(node.pos, ref.portIdx, totalFrom, cfg())
              : inputAnchor (node.pos, ref.portIdx, totalFrom, cfg());
            const wirePath = () => {
              const snap = snapTarget();
              let freeEnd = mouseWorld();
              if (snap) {
                const snapNode = a.graph().nodes.find((n) => n.uid === snap.nodeUid);
                if (snapNode) {
                  const totalSnap = snap.kind === 'input'
                    ? a.inputPortCount(snapNode.kind)
                    : a.outputPortCount(snapNode.kind);
                  freeEnd = snap.kind === 'input'
                    ? inputAnchor (snapNode.pos, snap.portIdx, totalSnap, cfg())
                    : outputAnchor(snapNode.pos, snap.portIdx, totalSnap, cfg());
                }
              }
              const [from, to] = ref.kind === 'output'
                ? [sourceAnchor, freeEnd]
                : [freeEnd, sourceAnchor];
              return bezierPath(from, to, cfg());
            };
            return (
              <path
                class="graph-edge graph-edge-pending"
                classList={{ 'graph-edge-pending-snapped': !!snapTarget() }}
                d={wirePath()}
              />
            );
          }}
        </Show>

        {/* Nodes */}
        <For each={ad().graph().nodes}>
          {(node: N) => {
            const a = ad();
            const ins = a.inputPortCount(node.kind);
            const outs = a.outputPortCount(node.kind);
            const h = nodeHeight({ inputCount: ins, outputCount: outs }, cfg());
            const selected = () => a.isNodeSelected(node.uid);
            const cat = () => a.nodeCategoryClass?.(node) ?? '';
            const bypassed = () => a.isBypassed?.(node) ?? false;
            return (
              <g
                transform={`translate(${node.pos[0]} ${node.pos[1]})`}
                data-node-uid={node.uid}
                classList={{
                  'graph-node': true,
                  [`graph-node-cat-${cat()}`]: !!cat(),
                  'graph-node-selected': selected(),
                  'graph-node-bypassed': bypassed(),
                }}
                onDblClick={(e) => { e.stopPropagation(); a.onNodeDoubleClick?.(node); }}
              >
                <rect class="graph-node-body" width={cfg().nodeWidth} height={h} rx={6} />
                <rect class="graph-node-header" width={cfg().nodeWidth} height={cfg().headerHeight} rx={6} />
                <text
                  class="graph-node-title"
                  x={10}
                  y={cfg().headerHeight * 0.65}
                  clip-path="url(#graph-canvas-title-clip)"
                >
                  {a.nodeLabel(node)}
                  <title>{a.nodeLabel(node)}</title>
                </text>
                {/* Input ports. Position + label placement depend on
                    orientation: horizontal nodes carry inputs on the LEFT
                    edge with labels indented; vertical nodes carry them
                    on the TOP edge with labels centred above. */}
                <For each={Array.from({ length: ins }, (_, i) => i)}>
                  {(i) => {
                    const isSnap = () => {
                      const s = snapTarget();
                      return s !== null && s.nodeUid === node.uid &&
                             s.portIdx === i && s.kind === 'input';
                    };
                    const [cx, cy] = (() => {
                      if (cfg().orientation === 'vertical') {
                        const n = Math.max(ins, 1);
                        return [((i + 1) / (n + 1)) * cfg().nodeWidth, 0] as const;
                      }
                      return [0, portY(i, cfg())] as const;
                    })();
                    const labelProps = (): { x: number; y: number; anchor: 'start' | 'middle' | 'end' } =>
                      cfg().orientation === 'vertical'
                        ? { x: cx, y: -cfg().portRadius - 3, anchor: 'middle' }
                        : { x: cfg().portRadius + 3, y: cy + 3, anchor: 'start' };
                    return (
                    <>
                      <circle
                        class="graph-port graph-port-input"
                        classList={{ 'graph-port-snap': isSnap() }}
                        data-port-kind="input"
                        data-port-idx={i}
                        cx={cx}
                        cy={cy}
                        r={cfg().portRadius}
                        onPointerDown={(e) => startPortDrag(e, node.uid, i, 'input')}
                      >
                        <title>{a.inputPortName(node.kind, i) || `in ${i}`}</title>
                      </circle>
                      <text
                        class="graph-port-label graph-port-label-input"
                        x={labelProps().x}
                        y={labelProps().y}
                        text-anchor={labelProps().anchor}
                        pointer-events="none"
                      >
                        {a.inputPortName(node.kind, i)}
                      </text>
                    </>
                    );
                  }}
                </For>
                {/* Output ports. Horizontal → RIGHT edge; vertical →
                    BOTTOM edge with labels centred below. */}
                <For each={Array.from({ length: outs }, (_, i) => i)}>
                  {(i) => {
                    const isSnap = () => {
                      const s = snapTarget();
                      return s !== null && s.nodeUid === node.uid &&
                             s.portIdx === i && s.kind === 'output';
                    };
                    const [cx, cy] = (() => {
                      if (cfg().orientation === 'vertical') {
                        const n = Math.max(outs, 1);
                        return [((i + 1) / (n + 1)) * cfg().nodeWidth, h] as const;
                      }
                      return [cfg().nodeWidth, portY(i, cfg())] as const;
                    })();
                    const labelProps = (): { x: number; y: number; anchor: 'start' | 'middle' | 'end' } =>
                      cfg().orientation === 'vertical'
                        ? { x: cx, y: h + cfg().portRadius + 9, anchor: 'middle' }
                        : { x: cfg().nodeWidth - cfg().portRadius - 3, y: cy + 3, anchor: 'end' };
                    return (
                    <>
                      <circle
                        class="graph-port graph-port-output"
                        classList={{ 'graph-port-snap': isSnap() }}
                        data-port-kind="output"
                        data-port-idx={i}
                        cx={cx}
                        cy={cy}
                        r={cfg().portRadius}
                        onPointerDown={(e) => startPortDrag(e, node.uid, i, 'output')}
                      >
                        <title>{a.outputPortName(node.kind, i) || `out ${i}`}</title>
                      </circle>
                      <text
                        class="graph-port-label graph-port-label-output"
                        x={labelProps().x}
                        y={labelProps().y}
                        text-anchor={labelProps().anchor}
                        pointer-events="none"
                      >
                        {a.outputPortName(node.kind, i)}
                      </text>
                    </>
                    );
                  }}
                </For>
              </g>
            );
          }}
        </For>

        <Show when={marquee()} keyed>
          {(r) => (
            <rect
              class="graph-marquee"
              x={r.minX}
              y={r.minY}
              width={r.maxX - r.minX}
              height={r.maxY - r.minY}
            />
          )}
        </Show>

        <Show when={cutPath().length >= 2}>
          <polyline
            class="graph-cut-path"
            points={cutPath().map((p) => `${p[0]},${p[1]}`).join(' ')}
          />
        </Show>
      </g>
    </svg>
  );
}

function parseEdgeAttr(s: string): CanvasConnection | null {
  const m = s.match(/^(\d+):(\d+)>(\d+):(\d+)$/);
  if (!m) return null;
  return {
    from_node: parseInt(m[1]),
    from_port: parseInt(m[2]),
    to_node:   parseInt(m[3]),
    to_port:   parseInt(m[4]),
  };
}
