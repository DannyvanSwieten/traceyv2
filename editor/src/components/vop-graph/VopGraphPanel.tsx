// Top-level VOP graph panel. Same stack layout as SopGraphPanel:
//   inspector (top, full width, fixed-ish height)
//   toolbar with node-add dropdown
//   canvas (fills the rest)
// Catalog is loaded once on mount; the graph itself is loaded by the host
// via loadVopGraphFromEngine(hostUid) before the panel mounts.

import { Component, onMount } from 'solid-js';
import { fetchCatalog } from '../../lib/vop_graph';
import { VopGraphCanvas } from './VopGraphCanvas';
import { VopNodePalette } from './VopNodePalette';
import { VopNodeInspector } from './VopNodeInspector';
// Reuse the SOP panel's CSS — same row/column rules.
import '../sop-graph/SopGraphPanel.css';

export const VopGraphPanel: Component = () => {
  onMount(async () => {
    try {
      await fetchCatalog();
    } catch (e) {
      console.error('Failed to fetch VOP node catalog:', e);
    }
  });

  return (
    <div class="sop-graph-panel">
      <div class="sop-graph-inspector-row">
        <VopNodeInspector />
      </div>
      <div class="sop-graph-canvas-col">
        <VopNodePalette />
        <VopGraphCanvas />
      </div>
    </div>
  );
};
