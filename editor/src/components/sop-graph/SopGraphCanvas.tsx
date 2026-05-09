// SVG-based pan/zoom/wires canvas for the SOP graph. Mirrors the structure
// of editor/src/components/material-graph/GraphCanvas.tsx but talks to the
// SOP store + catalog instead of the material ones. The two canvases share
// a stylesheet (../material-graph/GraphCanvas.css) so theming stays
// consistent; promoting the rendering core to a generic component is a
// later refactor (the existing material canvas is intentionally left alone
// to keep this change contained).

import { Component, For, createSignal, Show } from 'solid-js';
import {
  SopNode,
  SopConnection,
  inputPortCount,
  outputPortCount,
  lookupCatalog,
} from '../../lib/sop_graph';
import {
  sopGraph,
  moveNode,
  addConnection,
  removeNode,
  selectedNode,
  setSelectedNode,
} from '../../stores/sops';
import '../material-graph/GraphCanvas.css';

const NODE_WIDTH = 200;
const NODE_HEADER_HEIGHT = 28;
const PORT_ROW_HEIGHT = 22;
const PORT_RADIUS = 6;

function inputPortY(idx: number): number {
  return NODE_HEADER_HEIGHT + idx * PORT_ROW_HEIGHT + PORT_ROW_HEIGHT / 2;
}
function outputPortY(idx: number): number {
  return NODE_HEADER_HEIGHT + idx * PORT_ROW_HEIGHT + PORT_ROW_HEIGHT / 2;
}
function nodeHeight(node: SopNode): number {
  const ins = inputPortCount(node.kind);
  const outs = outputPortCount(node.kind);
  return NODE_HEADER_HEIGHT + Math.max(ins, outs, 1) * PORT_ROW_HEIGHT + 8;
}
function nodeOrigin(node: SopNode): [number, number] {
  return [node.pos?.[0] ?? 0, node.pos?.[1] ?? 0];
}
function inputAnchor(node: SopNode, portIdx: number): [number, number] {
  const [x, y] = nodeOrigin(node);
  return [x, y + inputPortY(portIdx)];
}
function outputAnchor(node: SopNode, portIdx: number): [number, number] {
  const [x, y] = nodeOrigin(node);
  return [x + NODE_WIDTH, y + outputPortY(portIdx)];
}
function bezier(from: [number, number], to: [number, number]): string {
  const [x1, y1] = from;
  const [x2, y2] = to;
  const dx = Math.max(40, Math.abs(x2 - x1) * 0.5);
  return `M ${x1} ${y1} C ${x1 + dx} ${y1}, ${x2 - dx} ${y2}, ${x2} ${y2}`;
}
function nodeLabel(n: SopNode): string {
  return lookupCatalog(n.kind)?.label ?? n.kind;
}

interface PortRef { nodeUid: number; portIdx: number }

export const SopGraphCanvas: Component = () => {
  const [pan, setPan] = createSignal<[number, number]>([0, 0]);
  const [zoom, setZoom] = createSignal(1);
  const [pendingFrom, setPendingFrom] = createSignal<PortRef | null>(null);
  const [mouseWorld, setMouseWorld] = createSignal<[number, number]>([0, 0]);

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
    startCanvasPan(e);
  }

  function onSvgPointerMove(e: PointerEvent) {
    setMouseWorld(clientToWorld(e.clientX, e.clientY));
  }

  function startCanvasPan(e: PointerEvent) {
    setSelectedNode(null);
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
    setSelectedNode(uid);
    const [startX, startY] = clientToWorld(e.clientX, e.clientY);
    const node = sopGraph().nodes.find((n) => n.uid === uid);
    if (!node) return;
    const [origX, origY] = nodeOrigin(node);

    const onMove = (mv: PointerEvent) => {
      const [wx, wy] = clientToWorld(mv.clientX, mv.clientY);
      moveNode(uid, origX + (wx - startX), origY + (wy - startY));
    };
    const onUp = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
  }

  function onOutputPortClick(e: MouseEvent, nodeUid: number, portIdx: number) {
    e.stopPropagation();
    const pending = pendingFrom();
    if (pending) {
      setPendingFrom(null);
      return;
    }
    setPendingFrom({ nodeUid, portIdx });
  }
  function onInputPortClick(e: MouseEvent, nodeUid: number, portIdx: number) {
    e.stopPropagation();
    const pending = pendingFrom();
    if (!pending) return;
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

  function onCanvasKeyDown(e: KeyboardEvent) {
    if (e.key === 'Delete' || e.key === 'Backspace') {
      const uid = selectedNode();
      if (uid !== null) {
        e.preventDefault();
        removeNode(uid);
        setSelectedNode(null);
      }
    }
  }

  return (
    <svg
      class="graph-canvas"
      ref={svgRef}
      onPointerDown={onSvgPointerDown}
      onPointerMove={onSvgPointerMove}
      onWheel={onWheel}
      onKeyDown={onCanvasKeyDown}
      tabIndex={0}
    >
      <defs>
        <pattern id="grid-sop" width="40" height="40" patternUnits="userSpaceOnUse">
          <path d="M 40 0 L 0 0 0 40" fill="none" stroke="#2a2a2a" stroke-width="1" />
        </pattern>
      </defs>
      <rect width="100%" height="100%" fill="url(#grid-sop)" />
      <g transform={`translate(${pan()[0]} ${pan()[1]}) scale(${zoom()})`}>
        {/* Edges */}
        <For each={sopGraph().connections}>
          {(c: SopConnection) => {
            const fromNode = () => sopGraph().nodes.find((n) => n.uid === c.from_node);
            const toNode = () => sopGraph().nodes.find((n) => n.uid === c.to_node);
            return (
              <Show when={fromNode() && toNode()}>
                <path
                  class="graph-edge"
                  d={bezier(
                    outputAnchor(fromNode()!, c.from_port),
                    inputAnchor(toNode()!, c.to_port)
                  )}
                />
              </Show>
            );
          }}
        </For>

        <Show when={pendingFrom()}>
          {() => {
            const ref = pendingFrom()!;
            const node = sopGraph().nodes.find((n) => n.uid === ref.nodeUid);
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
        <For each={sopGraph().nodes}>
          {(node: SopNode) => {
            const [x, y] = nodeOrigin(node);
            const h = nodeHeight(node);
            const ins = inputPortCount(node.kind);
            const outs = outputPortCount(node.kind);
            const selected = () => selectedNode() === node.uid;
            const category = () => lookupCatalog(node.kind)?.category ?? '';
            return (
              <g
                transform={`translate(${x} ${y})`}
                data-node-uid={node.uid}
                class={`graph-node graph-node-sop-${category()} ${selected() ? 'graph-node-selected' : ''}`}
              >
                <rect class="graph-node-body" width={NODE_WIDTH} height={h} rx={6} />
                <rect class="graph-node-header" width={NODE_WIDTH} height={NODE_HEADER_HEIGHT} rx={6} />
                <text class="graph-node-title" x={10} y={NODE_HEADER_HEIGHT * 0.65}>
                  {nodeLabel(node)}
                </text>
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
      </g>
    </svg>
  );
};
