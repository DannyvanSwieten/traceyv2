// Top-level DOP graph panel. Same stack layout as SopGraphPanel /
// VopGraphPanel:
//   inspector (top, full width)
//   palette toolbar + cache-status strip
//   canvas (fills the rest)
// Catalog + graph are loaded once on mount; mutations push back through
// the debounced store. Reuses the SOP panel's stylesheet so the chrome
// matches the other editors out of the box.

import { Component, onMount, Show } from 'solid-js';
import { fetchCatalog } from '../../lib/dop_graph';
import {
  loadDopGraphFromEngine,
  cachedToFrame,
  resetDopCache,
} from '../../stores/dops';
import { DopGraphCanvas } from './DopGraphCanvas';
import { DopNodePalette } from './DopNodePalette';
import { DopNodeInspector } from './DopNodeInspector';
import '../sop-graph/SopGraphPanel.css';
import './DopGraphPanel.css';

export const DopGraphPanel: Component = () => {
  onMount(async () => {
    try {
      await fetchCatalog();
    } catch (e) {
      console.error('Failed to fetch DOP node catalog:', e);
    }
    try {
      await loadDopGraphFromEngine();
    } catch (e) {
      console.error('Failed to load DOP graph:', e);
    }
  });

  return (
    <div class="sop-graph-panel">
      <div class="sop-graph-inspector-row">
        <DopNodeInspector />
      </div>
      <div class="sop-graph-canvas-col">
        <div class="dop-toolbar-row">
          <DopNodePalette />
          <Show when={cachedToFrame() > 0}>
            <span class="dop-cache-status">
              cached to frame {cachedToFrame()}
            </span>
          </Show>
          <button
            type="button"
            class="dop-reset-btn"
            title="Drop the simulation cache and resimulate from frame 1"
            onClick={() => resetDopCache()}
          >
            Reset Sim
          </button>
        </div>
        <DopGraphCanvas />
      </div>
    </div>
  );
};
