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
  lookupCatalog,
} from '../../lib/sop_graph';
import {
  connectToObjectOutput,
  currentGraph,
  enterSubnet,
  exitSubnet,
  moveNode,
  addConnection,
  removeNode,
  selectedNode,
  selectedNodes,
  setSelectedNode,
  setSelectedNodes,
  toggleSelectedNode,
  isNodeSelected,
} from '../../stores/sops';
import { openVopEditor } from '../../stores/vops';
import {
  rectFromCorners,
  nodesInRect,
  type MarqueeRect,
  type NodeBox,
} from '../../lib/graph_canvas_marquee';
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

interface PortRef { nodeUid: number; portIdx: number }

export const SopGraphCanvas: Component = () => {
  const [pan, setPan] = createSignal<[number, number]>([0, 0]);
  const [zoom, setZoom] = createSignal(1);
  const [pendingFrom, setPendingFrom] = createSignal<PortRef | null>(null);
  const [mouseWorld, setMouseWorld] = createSignal<[number, number]>([0, 0]);
  // Houdini-style: hold Space to pan. Drag on empty canvas without Space
  // does a rubber-band (marquee) select instead.
  const [spaceDown, setSpaceDown] = createSignal(false);
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

  function onSvgPointerDown(e: PointerEvent) {
    if (e.button !== 0) return;
    e.preventDefault();
    // Space-held drag pans the canvas, even when starting over a node or
    // port — matches the Houdini convention of Space being a pan modifier.
    if (spaceDown()) { startCanvasPan(e); return; }
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

  // Space-key tracking lives on `window` so the pan-modifier works even when
  // focus has wandered to a sibling panel (inspector, palette dropdown, etc.).
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

      if (e.key === ' ' || e.code === 'Space') {
        e.preventDefault();
        if (!e.repeat) setSpaceDown(true);
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
    };
    const onUp = (e: KeyboardEvent) => {
      if (e.key === ' ' || e.code === 'Space') setSpaceDown(false);
    };
    // Window blur / tab-switch can drop the keyup event; treat blur as "Space
    // released" so the user doesn't end up stuck in pan mode.
    const onBlur = () => setSpaceDown(false);
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

  return (
    <svg
      class="graph-canvas"
      classList={{ 'graph-canvas--panning': spaceDown() }}
      ref={svgRef}
      onPointerDown={onSvgPointerDown}
      onPointerMove={onSvgPointerMove}
      onWheel={onWheel}
    >
      <defs>
        <pattern id="grid-sop" width="40" height="40" patternUnits="userSpaceOnUse">
          <path d="M 40 0 L 0 0 0 40" fill="none" stroke="#2a2a2a" stroke-width="1" />
        </pattern>
      </defs>
      <rect width="100%" height="100%" fill="url(#grid-sop)" />
      <g transform={`translate(${pan()[0]} ${pan()[1]}) scale(${zoom()})`}>
        {/* Edges */}
        <For each={currentGraph().connections}>
          {(c: SopConnection) => {
            const fromNode = () => currentGraph().nodes.find((n) => n.uid === c.from_node);
            const toNode = () => currentGraph().nodes.find((n) => n.uid === c.to_node);
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
            const node = currentGraph().nodes.find((n) => n.uid === ref.nodeUid);
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
        <For each={currentGraph().nodes}>
          {(node: SopNode) => {
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
                class={`graph-node graph-node-sop-${category()} ${selected() ? 'graph-node-selected' : ''}`}
                onDblClick={(e) => onNodeDoubleClick(e, node)}
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
                      cx={portX(i, ins)}
                      cy={0}
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
                      cx={portX(i, outs)}
                      cy={h}
                      r={PORT_RADIUS}
                      onClick={(e) => onOutputPortClick(e, node.uid, i)}
                    />
                  )}
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
      </g>
    </svg>
  );
};
