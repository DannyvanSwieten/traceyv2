// Shared geometry helpers for rubber-band (marquee) selection across the
// SOP / VOP / material graph canvases. All three canvases lay nodes out as
// axis-aligned boxes in world space and share the same gesture; the math
// stays here so each canvas only owns its rendering + store wiring.

export interface MarqueeRect {
  // World-space rectangle in canonical form (min ≤ max on each axis).
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
}

export interface NodeBox<U> {
  uid: U;
  // World-space top-left + size.
  x: number;
  y: number;
  width: number;
  height: number;
}

// Build a normalised rect from two arbitrary world-space corner points.
export function rectFromCorners(
  a: [number, number],
  b: [number, number],
): MarqueeRect {
  return {
    minX: Math.min(a[0], b[0]),
    minY: Math.min(a[1], b[1]),
    maxX: Math.max(a[0], b[0]),
    maxY: Math.max(a[1], b[1]),
  };
}

// Treat a node as "hit" when its axis-aligned bounding box overlaps the
// marquee rect (Houdini/Maya-style — touching the box is enough; the user
// doesn't have to fully enclose the node).
export function rectIntersectsBox<U>(rect: MarqueeRect, box: NodeBox<U>): boolean {
  if (box.x + box.width  < rect.minX) return false;
  if (box.x              > rect.maxX) return false;
  if (box.y + box.height < rect.minY) return false;
  if (box.y              > rect.maxY) return false;
  return true;
}

// Convenience: return the uids of every node whose box intersects the rect.
export function nodesInRect<U>(rect: MarqueeRect, nodes: NodeBox<U>[]): U[] {
  const out: U[] = [];
  for (const n of nodes) if (rectIntersectsBox(rect, n)) out.push(n.uid);
  return out;
}
