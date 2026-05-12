// Solid store for per-node cook timings. Mirrors the broadcast shape the
// native side emits from drain_cook_result after every successful cook.
//
// One snapshot per cook — latest wins. The profiler tab in the resources
// browser renders this signal sorted by `ms` desc so the slow nodes float
// to the top.

import { createSignal } from 'solid-js';
import * as api from '../lib/api';

export interface NodeCookTimingRow {
  node_uid: number;
  parent_node_uid: number;  // 0 when at the root graph
  kind: string;
  name: string;             // node's `name` param if any, else ""
  ms: number;
}

export interface CookProfile {
  totalMs: number;
  rows: NodeCookTimingRow[];
  // Wall-clock when the snapshot was received; useful for "last cook
  // 3.4s ago" UX later.
  receivedAt: number;
}

const [profile, setProfile] = createSignal<CookProfile | null>(null);
export const cookProfile = profile;

// Wire the broadcast on module load. The native side emits this once per
// cook completion (drain_cook_result in editor_server.cpp). If the user
// has the profiler tab open, the next signal read picks up the new data.
api.listen('cook_timings', (msg) => {
  const rawRows = msg.rows;
  if (!Array.isArray(rawRows)) return;
  const rows: NodeCookTimingRow[] = rawRows.map((r) => {
    const rec = r as Record<string, unknown>;
    return {
      node_uid:        typeof rec.node_uid === 'number' ? rec.node_uid : 0,
      parent_node_uid: typeof rec.parent_node_uid === 'number' ? rec.parent_node_uid : 0,
      kind:            typeof rec.kind === 'string' ? rec.kind : '',
      name:            typeof rec.name === 'string' ? rec.name : '',
      ms:              typeof rec.ms === 'number' ? rec.ms : 0,
    };
  });
  setProfile({
    totalMs: typeof msg.total_ms === 'number' ? msg.total_ms : rows.reduce((s, r) => s + r.ms, 0),
    rows,
    receivedAt: Date.now(),
  });
});
