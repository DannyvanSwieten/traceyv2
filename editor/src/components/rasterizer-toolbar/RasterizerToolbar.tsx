// Thin sub-toolbar with viewport-overlay toggles for the rasterizer:
// points, edges, ground grid. Sits directly under the main toolbar so
// the controls are reachable from every workspace (Model, Animation,
// Render, etc.) — they're really viewport-display preferences rather
// than per-workspace render settings, and the Render workspace's
// inspector keeps its own path-tracer-only knobs (max samples,
// bounces, resolution).
//
// Each toggle round-trips through the native side: the rasterizer
// reads the flags via `RenderEngine::show_*()` on the next draw, so
// flipping any of them invalidates the path-tracer accumulator
// implicitly via `onSettingsChange` (the App-level callback the
// existing RenderSettings panel uses for the same reason).

import { Component, createSignal, onMount, Show } from 'solid-js';
import * as api from '../../lib/api';
import './RasterizerToolbar.css';

interface RasterizerToolbarProps {
  onSettingsChange?: () => void;
}

export const RasterizerToolbar: Component<RasterizerToolbarProps> = (props) => {
  // Local mirrors of the native flags. Seeded on mount so the toggle
  // states reflect whatever the editor was launched / re-loaded with.
  const [showPoints, setShowPoints] = createSignal(false);
  const [showEdges, setShowEdges] = createSignal(false);
  const [showGround, setShowGround] = createSignal(false);
  // Background color in `#rrggbb` form so it can drive the native
  // <input type="color"> directly. Linear-space values from the
  // engine get encoded for the picker; sRGB conversion is deliberately
  // skipped (the rasterizer's framebuffer is Unorm, so linear values
  // pass through unchanged — close enough for a viewport background
  // hint without a full gamma round-trip).
  const [bgHex, setBgHex] = createSignal('#334d66');
  const [ready, setReady] = createSignal(false);

  // Helpers for hex ↔ float[3] conversion. Channels are clamped to
  // [0,1] before encoding so anything HDR the engine returns still
  // produces a valid picker swatch (the picker only does LDR anyway).
  const floatToHex = (r: number, g: number, b: number): string => {
    const enc = (c: number) => {
      const v = Math.max(0, Math.min(1, c));
      const byte = Math.round(v * 255);
      return byte.toString(16).padStart(2, '0');
    };
    return `#${enc(r)}${enc(g)}${enc(b)}`;
  };
  const hexToFloat = (hex: string): [number, number, number] => {
    // Strip optional leading '#' and accept either 3- or 6-digit form.
    let s = hex.startsWith('#') ? hex.slice(1) : hex;
    if (s.length === 3) {
      s = s.split('').map((c) => c + c).join('');
    }
    const r = parseInt(s.slice(0, 2), 16) / 255;
    const g = parseInt(s.slice(2, 4), 16) / 255;
    const b = parseInt(s.slice(4, 6), 16) / 255;
    return [r, g, b];
  };

  onMount(async () => {
    try {
      const [points, edges, ground, bg] = await Promise.all([
        api.getShowPoints().catch(() => false),
        api.getShowEdges().catch(() => false),
        api.getShowGround().catch(() => false),
        api.getBackgroundColor().catch(() => [0.2, 0.3, 0.4, 1.0] as [number, number, number, number]),
      ]);
      setShowPoints(points);
      setShowEdges(edges);
      setShowGround(ground);
      setBgHex(floatToHex(bg[0], bg[1], bg[2]));
    } catch (e) {
      console.warn('rasterizer-toolbar: initial fetch failed', e);
    } finally {
      setReady(true);
    }
  });

  const handleBgChange = async (hex: string) => {
    setBgHex(hex);
    const [r, g, b] = hexToFloat(hex);
    try {
      await api.setBackgroundColor([r, g, b]);
      props.onSettingsChange?.();
    } catch (e) {
      console.warn('set_background_color failed:', e);
    }
  };

  // Generic toggle helper. The button uses `toolbar-button--active`
  // class for the on-state styling so we match the look of the main
  // toolbar's toggle buttons (PT Preview, Every Frame).
  const toggle = async (
    current: boolean,
    setter: (v: boolean) => void,
    push: (v: boolean) => Promise<unknown>,
    label: string,
  ): Promise<void> => {
    const next = !current;
    setter(next);
    try {
      await push(next);
      props.onSettingsChange?.();
    } catch (e) {
      console.warn(`set ${label} failed:`, e);
      setter(current);  // revert UI state on failure
    }
  };

  return (
    <div class="rasterizer-toolbar">
      <Show when={ready()}>
        <button
          type="button"
          class="rasterizer-toolbar-button"
          classList={{ 'rasterizer-toolbar-button--active': showPoints() }}
          title="Show vertex point splats over the shaded surface"
          onClick={() => toggle(showPoints(), setShowPoints, api.setShowPoints, 'show_points')}
        >
          Points
        </button>
        <button
          type="button"
          class="rasterizer-toolbar-button"
          classList={{ 'rasterizer-toolbar-button--active': showEdges() }}
          title="Show triangle edges (wireframe overlay)"
          onClick={() => toggle(showEdges(), setShowEdges, api.setShowEdges, 'show_edges')}
        >
          Edges
        </button>
        <button
          type="button"
          class="rasterizer-toolbar-button"
          classList={{ 'rasterizer-toolbar-button--active': showGround() }}
          title="Show the reference ground grid on y=0"
          onClick={() => toggle(showGround(), setShowGround, api.setShowGround, 'show_ground')}
        >
          Ground
        </button>
        {/* Vertical separator between toggle group and color group —
            cheap visual cue that these are different categories of
            control. */}
        <span class="rasterizer-toolbar-sep" aria-hidden="true" />
        <label class="rasterizer-toolbar-color" title="Viewport background color">
          <span class="rasterizer-toolbar-color-label">Background</span>
          {/* Native browser color picker. The visible swatch comes from
              the `<input>` itself; we just style its dimensions in
              CSS. The `onInput` event fires during drag, so the
              viewport updates live as the user scrubs the picker. */}
          <input
            type="color"
            class="rasterizer-toolbar-color-input"
            value={bgHex()}
            onInput={(e) => handleBgChange(e.currentTarget.value)}
          />
        </label>
      </Show>
    </div>
  );
};
