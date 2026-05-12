// Top-level SOP graph panel. New stack layout:
//   inspector (top, full width, fixed-ish height)
//   toolbar with node-add dropdown
//   canvas (fills the rest)
// The old left-rail palette has been replaced with a compact dropdown so
// the canvas gets the full dock width.

import { Component, onMount } from 'solid-js';
import { fetchCatalog, makeNode, syncNextUidRecursive } from '../../lib/sop_graph';
import {
  addNode,
  currentGraph,
  loadSopGraphFromEngine,
  sopGraph,
} from '../../stores/sops';
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
    // Seed the uid allocator past every uid in the (potentially nested)
    // tree so locally-allocated uids don't collide with nested ones.
    syncNextUidRecursive(sopGraph());

    // Seed a default object_output when the root graph is empty so a fresh
    // scene is immediately renderable (the path tracer needs at least one
    // emitted actor). A subsequent glTF import lands as additional subnets
    // alongside this output — the leftover orphan is a minor UX cost, much
    // smaller than the "blank scene, no idea why nothing renders" foot-gun
    // that comes from leaving the graph empty.
    if (currentGraph().nodes.length === 0) {
      const out = makeNode('object_output', [120, 120]);
      if (out) addNode(out);
    }
  });

  return (
    <div class="sop-graph-panel">
      <div class="sop-graph-inspector-row">
        <SopNodeInspector />
      </div>
      <div class="sop-graph-canvas-col">
        <SopNodePalette />
        <SopGraphCanvas />
      </div>
    </div>
  );
};
