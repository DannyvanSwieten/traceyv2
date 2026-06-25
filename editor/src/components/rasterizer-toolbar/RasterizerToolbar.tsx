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
  // Composition guides bitmask: bit 0 = thirds, bit 1 = safe areas.
  const [guides, setGuides] = createSignal(0);
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
      const [points, edges, ground, guideMask, bg] = await Promise.all([
        api.getShowPoints().catch(() => false),
        api.getShowEdges().catch(() => false),
        api.getShowGround().catch(() => false),
        api.getCompositionGuides().catch(() => 0),
        api.getBackgroundColor().catch(() => [0.2, 0.3, 0.4, 1.0] as [number, number, number, number]),
      ]);
      setShowPoints(points);
      setShowEdges(edges);
      setShowGround(ground);
      setGuides(guideMask);
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

  // Composition-guide bit toggle. Flips one bit of the guides bitmask and
  // pushes the whole mask to the native side. Reverts the UI on failure.
  const toggleGuide = async (bit: number, label: string): Promise<void> => {
    const prev = guides();
    const next = prev ^ bit;
    setGuides(next);
    try {
      await api.setCompositionGuides(next);
      props.onSettingsChange?.();
    } catch (e) {
      console.warn(`set ${label} failed:`, e);
      setGuides(prev);
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
      {/* Framing — always visible (not gated on `ready()`), since it's the most
          reached navigation action. ⌂ frames the whole scene; Frame Sel frames
          the selection (or all if nothing's selected). The camera glides there
          via the render tick. Mirror the F / Shift+F keyboard shortcuts. */}
      <button
        type="button"
        class="rasterizer-toolbar-button"
        title="Frame the whole scene — Shift+F"
        onClick={() => void api.frameView(false)}
      >
        ⌂ Frame
      </button>
      <button
        type="button"
        class="rasterizer-toolbar-button"
        title="Frame the selected object (or the whole scene if nothing is selected) — F"
        onClick={() => void api.frameView(true)}
      >
        Frame Sel
      </button>
      <span class="rasterizer-toolbar-sep" aria-hidden="true" />
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
        <span class="rasterizer-toolbar-sep" aria-hidden="true" />
        {/* Composition guides — framing aids for shot composition. Overlay
            both the rasterizer and the path-traced view (drawn in NDC). */}
        <button
          type="button"
          class="rasterizer-toolbar-button"
          classList={{ 'rasterizer-toolbar-button--active': (guides() & 1) !== 0 }}
          title="Rule-of-thirds grid overlay"
          onClick={() => toggleGuide(1, 'guides_thirds')}
        >
          Thirds
        </button>
        <button
          type="button"
          class="rasterizer-toolbar-button"
          classList={{ 'rasterizer-toolbar-button--active': (guides() & 2) !== 0 }}
          title="Safe-area guides (action-safe 90% + title-safe 80%)"
          onClick={() => toggleGuide(2, 'guides_safe')}
        >
          Safe
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
