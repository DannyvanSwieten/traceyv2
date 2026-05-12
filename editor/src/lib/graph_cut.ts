// Geometry helpers for the Houdini-style "hold Y, drag to cut" gesture.
// The bezier control-point formula differs per canvas (SOP flows top-to-
// bottom, VOP / material flow left-to-right), so callers compute their
// own control points and feed sampled polylines back here.

export type Pt = [number, number];

export interface ConnPolyline<K> {
  key: K;
  points: Pt[];
}

// Sample a cubic bezier at `segments + 1` evenly-spaced t values.
export function sampleCubicBezier(
  p0: Pt, p1: Pt, p2: Pt, p3: Pt, segments = 12,
): Pt[] {
  const out: Pt[] = [];
  for (let i = 0; i <= segments; i++) {
    const t = i / segments;
    const mt = 1 - t;
    const x = mt*mt*mt*p0[0] + 3*mt*mt*t*p1[0] + 3*mt*t*t*p2[0] + t*t*t*p3[0];
    const y = mt*mt*mt*p0[1] + 3*mt*mt*t*p1[1] + 3*mt*t*t*p2[1] + t*t*t*p3[1];
    out.push([x, y]);
  }
  return out;
}

// Standard cross-product test. Returns true when (a→b) and (c→d) share an
// interior point. Touching at an endpoint is excluded (denom check covers
// the colinear case as a no-cut).
export function segmentsIntersect(a: Pt, b: Pt, c: Pt, d: Pt): boolean {
  const d1x = b[0]-a[0], d1y = b[1]-a[1];
  const d2x = d[0]-c[0], d2y = d[1]-c[1];
  const denom = d1x*d2y - d1y*d2x;
  if (Math.abs(denom) < 1e-9) return false;
  const dx = a[0]-c[0], dy = a[1]-c[1];
  const t = (d2x*dy - d2y*dx) / denom;
  const u = (d1x*dy - d1y*dx) / denom;
  return t >= 0 && t <= 1 && u >= 0 && u <= 1;
}

// Return the keys of polylines that the segment (a→b) crosses.
export function intersectingConnections<K>(
  a: Pt, b: Pt, polylines: ConnPolyline<K>[],
): K[] {
  const out: K[] = [];
  for (const pl of polylines) {
    const pts = pl.points;
    for (let i = 1; i < pts.length; i++) {
      if (segmentsIntersect(a, b, pts[i-1], pts[i])) { out.push(pl.key); break; }
    }
  }
  return out;
}
