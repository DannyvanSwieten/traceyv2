// Top-level SOP graph panel. Three-column layout:
//   [palette] [canvas] [inspector]
// All three share the SOP store; mutations there debounce-push the full
// graph JSON back to the host, which re-cooks and broadcasts scene_changed.

import { Component, onMount } from 'solid-js';
import { fetchCatalog, syncNextUid } from '../../lib/sop_graph';
import { loadSopGraphFromEngine, sopGraph } from '../../stores/sops';
import { SopGraphCanvas } from './SopGraphCanvas';
import { SopNodePalette } from './SopNodePalette';
import { SopNodeInspector } from './SopNodeInspector';
import './SopGraphPanel.css';

export const SopGraphPanel: Component = () => {
  onMount(async () => {
    // Load the catalog first (palette / inspector depend on it), then the
    // current scene's SOP graph. Order matters: deserializing nodes whose
    // kind isn't in the catalog yet would fall through to defaults.
    try {
      await fetchCatalog();
    } catch (e) {
      console.error('Failed to fetch SOP node catalog:', e);
    }
    await loadSopGraphFromEngine();
    // Make sure the local uid allocator is past everything the host shipped.
    for (const n of sopGraph().nodes) syncNextUid(n.uid);
  });

  return (
    <div class="sop-graph-panel">
      <div class="sop-graph-palette-col">
        <SopNodePalette />
      </div>
      <div class="sop-graph-canvas-col">
        <SopGraphCanvas />
      </div>
      <div class="sop-graph-inspector-col">
        <SopNodeInspector />
      </div>
    </div>
  );
};
