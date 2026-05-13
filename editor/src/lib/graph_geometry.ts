// Pure layout math shared across SOP / VOP / DOP / Material graph canvases.
// Orientation-aware: SOPs flow top-to-bottom (inputs along the top edge,
// outputs along the bottom edge); VOP / DOP / Material flow left-to-
// right (inputs along the left edge, outputs along the right edge).
//
// No reactivity, no store imports — just arithmetic. All anchors are in
// world coordinates relative to the node's origin (top-left corner).

export type GraphOrientation = 'horizontal' | 'vertical';

export interface GraphGeometryConfig {
  orientation: GraphOrientation;
  nodeWidth: number;
  headerHeight: number;
  // Horizontal canvases stack ports along the left/right edges, separated
  // by `portRowHeight`. Vertical canvases ignore this — ports spread
  // evenly across the top/bottom edges and node height stays fixed.
  portRowHeight: number;
  // Vertical only: fixed body height under the header (top→bottom flow
  // nodes are usually compact and rectangular, not as tall as the port
  // count would dictate).
  bodyPadding: number;
  portRadius: number;
}

export const DEFAULT_HORIZONTAL: GraphGeometryConfig = {
  orientation: 'horizontal',
  nodeWidth: 130,
  headerHeight: 22,
  portRowHeight: 18,
  bodyPadding: 12,
  portRadius: 5,
};

export const DEFAULT_VERTICAL: GraphGeometryConfig = {
  orientation: 'vertical',
  nodeWidth: 140,
  headerHeight: 24,
  portRowHeight: 18,
  bodyPadding: 10,
  portRadius: 5,
};

// Backwards-compat re-export. Older call sites that don't pass a config
// fall through to the horizontal default (the most common case).
export const DEFAULT_GEOMETRY: GraphGeometryConfig = DEFAULT_HORIZONTAL;

export interface NodeBoxInputs {
  inputCount: number;
  outputCount: number;
}

// Node height. Horizontal: tall enough to stack the max(in,out) ports.
// Vertical: fixed at header + body padding (ports run along edges).
export function nodeHeight(
  io: NodeBoxInputs,
  cfg: GraphGeometryConfig = DEFAULT_HORIZONTAL,
): number {
  if (cfg.orientation === 'vertical') {
    return cfg.headerHeight + cfg.bodyPadding;
  }
  return (
    cfg.headerHeight +
    Math.max(io.inputCount, io.outputCount, 1) * cfg.portRowHeight +
    8
  );
}

// Port anchor coordinates relative to the node's origin (top-left).
//   • Horizontal: input on the LEFT edge at row `idx` of `total` (ignored
//     for spacing — rows stack); output on the RIGHT edge.
//   • Vertical: input on the TOP edge spread across width `total` slots;
//     output on the BOTTOM edge.
export function inputAnchor(
  pos: [number, number],
  idx: number,
  total: number,
  cfg: GraphGeometryConfig = DEFAULT_HORIZONTAL,
): [number, number] {
  if (cfg.orientation === 'vertical') {
    const n = Math.max(total, 1);
    return [pos[0] + ((idx + 1) / (n + 1)) * cfg.nodeWidth, pos[1]];
  }
  return [pos[0], pos[1] + portY(idx, cfg)];
}

export function outputAnchor(
  pos: [number, number],
  idx: number,
  total: number,
  cfg: GraphGeometryConfig = DEFAULT_HORIZONTAL,
): [number, number] {
  if (cfg.orientation === 'vertical') {
    const n = Math.max(total, 1);
    const h = nodeHeight({ inputCount: 0, outputCount: 0 }, cfg);
    return [pos[0] + ((idx + 1) / (n + 1)) * cfg.nodeWidth, pos[1] + h];
  }
  return [pos[0] + cfg.nodeWidth, pos[1] + portY(idx, cfg)];
}

// Horizontal-only helper — vertical canvases shouldn't call this. Kept
// for legacy import paths and the GraphCanvas's port-label positioning.
export function portY(idx: number, cfg: GraphGeometryConfig = DEFAULT_HORIZONTAL): number {
  return cfg.headerHeight + idx * cfg.portRowHeight + cfg.portRowHeight / 2;
}

// Orientation-aware bezier. Horizontal canvases bend on X (handles
// horizontal); vertical canvases bend on Y (handles vertical). Floor
// of 40 keeps short hops from kinking.
export function bezierPath(
  from: [number, number],
  to: [number, number],
  cfg: GraphGeometryConfig = DEFAULT_HORIZONTAL,
): string {
  const [x1, y1] = from;
  const [x2, y2] = to;
  if (cfg.orientation === 'vertical') {
    const dy = Math.max(40, Math.abs(y2 - y1) * 0.5);
    return `M ${x1} ${y1} C ${x1} ${y1 + dy}, ${x2} ${y2 - dy}, ${x2} ${y2}`;
  }
  const dx = Math.max(40, Math.abs(x2 - x1) * 0.5);
  return `M ${x1} ${y1} C ${x1 + dx} ${y1}, ${x2 - dx} ${y2}, ${x2} ${y2}`;
}
