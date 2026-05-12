// Shared "frame to fit" computation used by every graph canvas's A/F
// shortcuts. Given a set of node rects in world coordinates and the
// viewport pixel size, returns the (pan, zoom) that centres the rects
// inside the viewport with a margin. Capped zoom-in so a single tiny
// node doesn't blow up to fill the screen.
//
// Each canvas owns its own pan/zoom signals and node rect shapes — the
// helper takes pre-computed rects to stay agnostic of the underlying
// node type. Callers pass the marquee's `NodeBox` shape (or any
// {minX, minY, maxX, maxY}) and the helper just operates on the bbox.

export interface FrameRect {
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
}

export interface FrameResult {
  pan: [number, number];
  zoom: number;
}

const MIN_ZOOM = 0.25;
const MAX_ZOOM = 2.0;     // refuse to zoom in past 2× — keeps a single node from filling the screen
const DEFAULT_MARGIN = 60;

export function computeFrame(
  rects: FrameRect[],
  viewportW: number,
  viewportH: number,
  margin: number = DEFAULT_MARGIN,
): FrameResult | null {
  if (rects.length === 0) return null;
  if (viewportW <= 0 || viewportH <= 0) return null;

  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  for (const r of rects) {
    if (r.minX < minX) minX = r.minX;
    if (r.minY < minY) minY = r.minY;
    if (r.maxX > maxX) maxX = r.maxX;
    if (r.maxY > maxY) maxY = r.maxY;
  }
  const bw = Math.max(1, maxX - minX);
  const bh = Math.max(1, maxY - minY);

  const availW = Math.max(1, viewportW - 2 * margin);
  const availH = Math.max(1, viewportH - 2 * margin);
  const z = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, Math.min(availW / bw, availH / bh)));

  // Pan such that the world centre maps to the viewport centre.
  // viewport_center = pan + z * world_center  ⇒  pan = viewport_center − z * world_center
  const cx = (minX + maxX) / 2;
  const cy = (minY + maxY) / 2;
  return {
    pan: [viewportW / 2 - z * cx, viewportH / 2 - z * cy],
    zoom: z,
  };
}
