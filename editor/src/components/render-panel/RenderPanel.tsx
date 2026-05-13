// Render workspace's main settings panel. Lives in the dock slot
// (where the SOP / VOP / Material / DOP graph editors normally sit)
// so the workspace reads as "viewport on the left, render controls
// on the right" — the path-tracer-focused layout the user expects
// after switching to the Render tab.
//
// Wiring stays in App.tsx: the parent owns the maxSamples / bounces /
// resolution signals (so values survive workspace switches and round-
// trip through save_scene), and just hands callbacks to this panel.

import { Component, For } from 'solid-js';
import * as api from '../../lib/api';
import './RenderPanel.css';

interface RenderPanelProps {
  maxSamples: () => number;
  setMaxSamples: (n: number) => void;
  maxBounces: () => number;
  setMaxBounces: (n: number) => void;
  // Path-tracer resolution. [0, 0] is the "match viewport" sentinel
  // — the engine treats it as no-override and renders at viewport
  // dimensions.
  resolution: () => [number, number];
  setResolution: (w: number, h: number) => void;
  onResetRender: () => void;
}

// Standard preview / output resolutions. Kept verbatim from the
// previous WorkspaceBar placement so the saved-scene resolution
// values still match the labels.
const RESOLUTION_PRESETS: { label: string; w: number; h: number }[] = [
  { label: 'Viewport',   w: 0,    h: 0    },
  { label: '480p',       w: 854,  h: 480  },
  { label: '720p',       w: 1280, h: 720  },
  { label: '1080p',      w: 1920, h: 1080 },
  { label: '1440p',      w: 2560, h: 1440 },
  { label: '4K',         w: 3840, h: 2160 },
];

export const RenderPanel: Component<RenderPanelProps> = (props) => {
  return (
    <div class="render-panel">
      <h3 class="render-panel-title">Render</h3>

      <section class="render-panel-section">
        <h4 class="render-panel-section-title">Output</h4>
        <div class="render-panel-row">
          <label class="render-panel-label" for="render-panel-resolution">
            Resolution
          </label>
          <select
            id="render-panel-resolution"
            class="render-panel-select"
            title="Path-tracer render resolution. Viewport matches the live window; named presets render at fixed dimensions."
            value={`${props.resolution()[0]}x${props.resolution()[1]}`}
            onChange={(e) => {
              const [w, h] = e.currentTarget.value.split('x').map((n) => parseInt(n, 10));
              props.setResolution(w || 0, h || 0);
            }}
          >
            <For each={RESOLUTION_PRESETS}>
              {(p) => (
                <option value={`${p.w}x${p.h}`}>
                  {p.w === 0 ? p.label : `${p.label} (${p.w}×${p.h})`}
                </option>
              )}
            </For>
          </select>
        </div>
      </section>

      <section class="render-panel-section">
        <h4 class="render-panel-section-title">Path Tracer</h4>
        <div class="render-panel-row">
          <label class="render-panel-label" for="render-panel-samples">
            Samples
          </label>
          <div class="render-panel-slider-row">
            <input
              id="render-panel-samples"
              type="range"
              min="16"
              max="8192"
              step="16"
              value={props.maxSamples()}
              onInput={(e) => props.setMaxSamples(parseInt(e.currentTarget.value))}
            />
            <span class="render-panel-value">{props.maxSamples()}</span>
          </div>
        </div>
        <div class="render-panel-row">
          <label class="render-panel-label" for="render-panel-bounces">
            Bounces
          </label>
          <div class="render-panel-slider-row">
            <input
              id="render-panel-bounces"
              type="range"
              min="1"
              max="16"
              value={props.maxBounces()}
              onInput={(e) => props.setMaxBounces(parseInt(e.currentTarget.value))}
            />
            <span class="render-panel-value">{props.maxBounces()}</span>
          </div>
        </div>
      </section>

      <section class="render-panel-section">
        <h4 class="render-panel-section-title">Actions</h4>
        <div class="render-panel-actions">
          <button
            type="button"
            class="render-panel-button"
            onClick={props.onResetRender}
            title="Restart accumulation from sample 0 — useful after a camera or material edit during a long render."
          >
            Reset Accumulation
          </button>
          <button
            type="button"
            class="render-panel-button render-panel-button--accent"
            onClick={() => {
              // High-quality preset: bump samples + bounces to
              // convergence quality. Clamps to the same ceilings the
              // slider already enforces.
              props.setMaxSamples(4096);
              props.setMaxBounces(8);
              api.setMaxSamples(4096).catch(() => {});
              api.setMaxBounces(8).catch(() => {});
            }}
            title="Boost samples and bounces to a convergence-quality preset."
          >
            High Quality Preset
          </button>
        </div>
      </section>
    </div>
  );
};
