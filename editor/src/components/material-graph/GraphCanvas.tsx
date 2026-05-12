import { Component, For, createSignal, onCleanup, onMount, Show } from 'solid-js';
import {
  Node,
  Connection,
  inputPortCount,
  outputPortCount,
} from '../../lib/material_graph';
import {
  materialGraph,
  moveNode,
  addConnection,
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

interface PortRef { nodeUid: number; portIdx: number; }

export const GraphCanvas: Component = () => {
  const [pan, setPan] = createSignal<[number, number]>([0, 0]);
  const [zoom, setZoom] = createSignal(1);
  const [pendingFrom, setPendingFrom] = createSignal<PortRef | null>(null);
  const [mouseWorld, setMouseWorld] = createSignal<[number, number]>([0, 0]);
  // Houdini-style: hold Alt to pan; empty-canvas drag without Alt starts a
  // rubber-band selection. Alt was picked over Space because the editor's
  // timeline owns Space for play/pause.
  const [panKeyDown, setPanKeyDown] = createSignal(false);
  const [marquee, setMarquee] = createSignal<MarqueeRect | null>(null);

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

  // Click an output port -> start a pending connection. Click an input port
  // -> complete it (or vice versa for input-first). Same direction click
  // cancels.
  function onOutputPortClick(e: MouseEvent, nodeUid: number, portIdx: number) {
    e.stopPropagation();
    const pending = pendingFrom();
    if (pending) {
      // We were pending an input -- output click cancels.
      setPendingFrom(null);
      return;
    }
    setPendingFrom({ nodeUid, portIdx });
  }
  function onInputPortClick(e: MouseEvent, nodeUid: number, portIdx: number) {
    e.stopPropagation();
    const pending = pendingFrom();
    if (!pending) return;  // input-first connection not supported
    if (pending.nodeUid === nodeUid) {
      setPendingFrom(null);
      return;
    }
    addConnection({
      from_node: pending.nodeUid,
      from_port: pending.portIdx,
      to_node: nodeUid,
      to_port: portIdx,
    });
    setPendingFrom(null);
  }

  // Window-level Space tracking — see SopGraphCanvas for the rationale.
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
      if (e.key === ' ' || e.code === 'Space') {
        e.preventDefault();
        if (!e.repeat) setPanKeyDown(true);
        return;
      }
      // Delete on window (not SVG focus) — same rationale as SopGraphCanvas.
      if (e.key === 'Delete' || e.key === 'Backspace') {
        const uids = [...selectedNodes()];
        if (uids.length === 0) return;
        e.preventDefault();
        for (const u of uids) removeNode(u);
        setSelectedNode(null);
      }
    };
    const onUp = (e: KeyboardEvent) => {
      if (e.key === ' ' || e.code === 'Space') setPanKeyDown(false);
    };
    const onBlur = () => setPanKeyDown(false);
    window.addEventListener('keydown', onDown);
    window.addEventListener('keyup', onUp);
    window.addEventListener('blur', onBlur);
    onCleanup(() => {
      window.removeEventListener('keydown', onDown);
      window.removeEventListener('keyup', onUp);
      window.removeEventListener('blur', onBlur);
    });
  });

  return (
    <svg
      class="graph-canvas"
      classList={{ 'graph-canvas--panning': panKeyDown() }}
      ref={svgRef}
      onPointerDown={onSvgPointerDown}
      onPointerMove={onSvgPointerMove}
      onWheel={onWheel}
    >
      <defs>
        <pattern id="grid" width="40" height="40" patternUnits="userSpaceOnUse">
          <path d="M 40 0 L 0 0 0 40" fill="none" stroke="#2a2a2a" stroke-width="1"/>
        </pattern>
      </defs>
      <rect width="100%" height="100%" fill="url(#grid)" />
      <g transform={`translate(${pan()[0]} ${pan()[1]}) scale(${zoom()})`}>
        {/* Edges */}
        <For each={materialGraph().connections}>
          {(c: Connection) => {
            const fromNode = () => materialGraph().nodes.find((n) => n.uid === c.from_node);
            const toNode = () => materialGraph().nodes.find((n) => n.uid === c.to_node);
            return (
              <Show when={fromNode() && toNode()}>
                <path
                  class="graph-edge"
                  d={bezier(outputAnchor(fromNode()!, c.from_port),
                            inputAnchor(toNode()!, c.to_port))}
                />
              </Show>
            );
          }}
        </For>

        {/* In-progress connection (cursor-following) */}
        <Show when={pendingFrom()}>
          {() => {
            const ref = pendingFrom()!;
            const node = materialGraph().nodes.find((n) => n.uid === ref.nodeUid);
            if (!node) return null;
            return (
              <path
                class="graph-edge graph-edge-pending"
                d={bezier(outputAnchor(node, ref.portIdx), mouseWorld())}
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
                <text class="graph-node-title" x={10} y={NODE_HEADER_HEIGHT * 0.65}>
                  {nodeLabel(node)}
                </text>
                {/* Input ports (left side) */}
                <For each={Array.from({ length: ins }, (_, i) => i)}>
                  {(i) => (
                    <circle
                      class="graph-port graph-port-input"
                      data-port-kind="input"
                      cx={0}
                      cy={inputPortY(i)}
                      r={PORT_RADIUS}
                      onClick={(e) => onInputPortClick(e, node.uid, i)}
                    />
                  )}
                </For>
                {/* Output ports (right side) */}
                <For each={Array.from({ length: outs }, (_, i) => i)}>
                  {(i) => (
                    <circle
                      class="graph-port graph-port-output"
                      data-port-kind="output"
                      cx={NODE_WIDTH}
                      cy={outputPortY(i)}
                      r={PORT_RADIUS}
                      onClick={(e) => onOutputPortClick(e, node.uid, i)}
                    />
                  )}
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
  );
};
