// JS mirror of the engine's ScalarChannel evaluation
// (src/sops/parameter.cpp:52-113) so the curve editor can plot the exact
// curve the engine plays back, without an IPC round-trip per pixel.
//
// Semantics replicated 1:1:
//   • empty channel → 0; single key → its value
//   • pre/post extrapolation: hold | linear (uses first.in / last.out
//     tangent) | cycle (fmod-wrap into [first.t, last.t], negative-safe)
//   • segment interp comes from the LEFT key's mode
//   • bezier = cubic Hermite with tangents (value/sec) scaled by segment dt
//
// Pure module — no state, no store imports beyond the wire types.

import { Extrap, Keyframe } from './sop_graph';

// Cubic Hermite over one segment. `time` in seconds, must satisfy
// a.t <= time <= b.t. Exposed for the curve renderer's per-segment sampling.
export function evalSegment(a: Keyframe, b: Keyframe, time: number): number {
  const range = b.t - a.t;
  if (range <= 0) return a.v;
  const t = (time - a.t) / range;
  switch (a.i) {
    case 'step':
      return a.v;
    case 'linear':
      return a.v + t * (b.v - a.v);
    case 'bezier': {
      const t2 = t * t;
      const t3 = t2 * t;
      const h00 = 2 * t3 - 3 * t2 + 1;
      const h10 = t3 - 2 * t2 + t;
      const h01 = -2 * t3 + 3 * t2;
      const h11 = t3 - t2;
      return h00 * a.v + h10 * range * a.out + h01 * b.v + h11 * range * b.in;
    }
  }
  return a.v;
}

function wrapCycle(time: number, first: number, last: number): number {
  const range = last - first;
  if (range <= 0) return first;
  let off = (time - first) % range;
  if (off < 0) off += range;
  return first + off;
}

// Evaluate a channel at `time` (seconds). `keys` must be sorted by t
// (the store keeps them sorted; animated_channels re-sorts defensively).
export function evalChannel(
  keys: Keyframe[],
  pre: Extrap,
  post: Extrap,
  time: number,
): number {
  if (keys.length === 0) return 0;
  if (keys.length === 1) return keys[0].v;

  const first = keys[0];
  const last = keys[keys.length - 1];

  if (time < first.t) {
    switch (pre) {
      case 'hold':
        return first.v;
      case 'linear':
        return first.v - (first.t - time) * first.in;
      case 'cycle':
        time = wrapCycle(time, first.t, last.t);
        break;
    }
  }
  if (time > last.t) {
    switch (post) {
      case 'hold':
        return last.v;
      case 'linear':
        return last.v + (time - last.t) * last.out;
      case 'cycle':
        time = wrapCycle(time, first.t, last.t);
        break;
    }
  }

  // Bracket: first key strictly after `time`; previous key starts the segment.
  let hi = keys.length;
  let lo = 0;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (time < keys[mid].t) hi = mid;
    else lo = mid + 1;
  }
  if (lo === 0) return first.v;
  if (lo === keys.length) return last.v;
  return evalSegment(keys[lo - 1], keys[lo], time);
}

// Catmull-Rom auto tangent (value/sec) for keys[i] — the "Auto (smooth)"
// interpolation choice. Interior keys take the secant slope between their
// neighbours; endpoints take the one-sided slope; a lone key is flat.
export function autoTangent(keys: Keyframe[], i: number): number {
  if (keys.length < 2) return 0;
  const prev = keys[i - 1];
  const cur = keys[i];
  const next = keys[i + 1];
  const slope = (a: Keyframe, b: Keyframe) =>
    b.t - a.t > 1e-9 ? (b.v - a.v) / (b.t - a.t) : 0;
  if (prev && next) return slope(prev, next);
  if (next) return slope(cur, next);
  if (prev) return slope(prev, cur);
  return 0;
}

// Min/max of the channel over [t0, t1] for fit-to-view: exact key values,
// dense samples across bezier segments (curves overshoot their endpoints),
// and the extrapolated boundary values.
const BEZIER_SAMPLES = 16;

export function channelValueRange(
  keys: Keyframe[],
  pre: Extrap,
  post: Extrap,
  t0: number,
  t1: number,
): { min: number; max: number } {
  if (keys.length === 0) return { min: 0, max: 0 };
  let min = Infinity;
  let max = -Infinity;
  const push = (v: number) => {
    if (v < min) min = v;
    if (v > max) max = v;
  };
  for (const k of keys) {
    if (k.t >= t0 && k.t <= t1) push(k.v);
  }
  for (let s = 0; s + 1 < keys.length; ++s) {
    const a = keys[s];
    const b = keys[s + 1];
    if (a.i !== 'bezier' || b.t < t0 || a.t > t1) continue;
    for (let j = 1; j < BEZIER_SAMPLES; ++j) {
      const t = a.t + ((b.t - a.t) * j) / BEZIER_SAMPLES;
      if (t >= t0 && t <= t1) push(evalSegment(a, b, t));
    }
  }
  // Extrapolated view edges (covers linear extrapolation running off-range,
  // and the all-keys-outside-view case).
  push(evalChannel(keys, pre, post, t0));
  push(evalChannel(keys, pre, post, t1));
  if (!Number.isFinite(min)) return { min: 0, max: 0 };
  return { min, max };
}
