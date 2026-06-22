// USD → SOP subnet import. Thin wrapper over the shared subnet builder
// (import_subnets.ts) — peek the stage, then build a `usd_import` subnet tree.
// Mirrors glTF import; see import_subnets.ts for the shared logic. USD adds two
// things glTF doesn't: animated transforms (baked into keyframe channels) and a
// stage time range (returned so the caller can set the timeline).

import * as api from './api';
import { SopNode } from './sop_graph';
import { buildSubnetTree, ensureCatalog, groupNameFromPath } from './import_subnets';

export interface UsdImportResult {
  subnets: SopNode[];
  // Present when the stage authored animation: fps + frame range so the caller
  // can size the editor timeline to the imported clip.
  timeline?: { fps: number; frameStart: number; frameEnd: number };
}

// Peek the USD stage, then build one root-level subnet per mesh prim (flat
// first slice), wiring a `usd_import` node into each and baking any animated
// world transforms into keyframe channels. Returns the subnets + (if animated)
// the timeline range to apply.
export async function buildSubnetsFromUsd(filePath: string): Promise<UsdImportResult> {
  await ensureCatalog();
  const peek = await api.peekUsd(filePath);
  const fps = peek.time_codes_per_second && peek.time_codes_per_second > 0
    ? peek.time_codes_per_second
    : 24;
  const subnets = buildSubnetTree(peek.roots, filePath, 'usd_import', fps,
    groupNameFromPath(filePath));

  let timeline: UsdImportResult['timeline'] | undefined;
  if (peek.animated) {
    // USD timeCode → seconds (t / fps) → editor frame (seconds * fps + 1).
    // With fps = timeCodesPerSecond these collapse to frame = timeCode + 1.
    const start = peek.start_time_code ?? 0;
    const end = peek.end_time_code ?? 0;
    const frameStart = Math.max(1, Math.round(start) + 1);
    const frameEnd = Math.max(frameStart + 1, Math.round(end) + 1);
    timeline = { fps, frameStart, frameEnd };
  }
  return { subnets, timeline };
}
