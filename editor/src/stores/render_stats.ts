// Solid store for the per-tick render-stats broadcast. The native
// side emits `render_stats` ~4 Hz from render_tick once a viewport
// renderer exists. The profiler tab reads these signals so the
// "live" numbers (FPS, triangles, instances, sample count) sit
// alongside the per-cook timing table without competing for screen
// real estate.

import { createSignal } from 'solid-js';
import * as api from '../lib/api';

export interface RenderStats {
  fps: number;
  render_time_ms: number;     // last path-tracer dispatch on the inset
  // Wall-clock buckets for the current render_tick (EMA-smoothed
  // server-side). tick_ms is the whole render_tick body; the others
  // attribute it to specific phases so a gap between `tick_ms` and
  // the sum of the slices is the unmeasured cost (vsync stall, cook
  // worker contention, etc.).
  tick_ms: number;
  rebuild_ms: number;         // drain_cook_result + apply_emitted
  raster_ms: number;          // rasterizer dispatch + wait
  present_ms: number;         // swapchain acquire + composite + present (vsync)
  triangles: number;          // BLAS triangles across all instances
  instances: number;          // TLAS instances (== visible actors with geo)
  bvh_nodes: number;
  samples: number;            // current accumulator sample count
  max_samples: number;        // cap before accumulation freezes
  receivedAt: number;
}

const [stats, setStats] = createSignal<RenderStats | null>(null);
export const renderStats = stats;

api.listen('render_stats', (msg) => {
  const rec = msg as Record<string, unknown>;
  const num = (k: string) => (typeof rec[k] === 'number' ? (rec[k] as number) : 0);
  setStats({
    fps:            num('fps'),
    render_time_ms: num('render_time_ms'),
    tick_ms:        num('tick_ms'),
    rebuild_ms:     num('rebuild_ms'),
    raster_ms:      num('raster_ms'),
    present_ms:     num('present_ms'),
    triangles:      num('triangles'),
    instances:      num('instances'),
    bvh_nodes:      num('bvh_nodes'),
    samples:        num('samples'),
    max_samples:    num('max_samples'),
    receivedAt:     Date.now(),
  });
});
