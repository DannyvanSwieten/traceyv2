import { Component } from 'solid-js';
import type { Vec3 } from '../../lib/api';

// Tiny colour-picker swatch. Renders a native <input type="color"> bound
// to a Vec3 (linear RGB, 0..1). The picker round-trips through 8-bit
// hex which loses sub-1/255 precision at the bottom of the channel
// range — fine for the lighting / material defaults where the user is
// dragging hues around, but pair it with the per-channel NumberInputs
// when the user needs an exact value (HDR colours > 1.0 also need the
// numeric inputs since the picker is clamped to LDR).
//
// `onCommit` fires whenever the user picks a new colour (native input's
// "input" event — equivalent to mid-drag updates on the colour palette).

export interface ColorSwatchProps {
  value: () => Vec3;
  onCommit: (rgb: Vec3) => void;
  title?: string;
  class?: string;
}

function clamp01(x: number): number {
  return Math.max(0, Math.min(1, x));
}

function rgbToHex(v: Vec3): string {
  const r = Math.round(clamp01(v.x) * 255);
  const g = Math.round(clamp01(v.y) * 255);
  const b = Math.round(clamp01(v.z) * 255);
  return `#${[r, g, b].map((c) => c.toString(16).padStart(2, '0')).join('')}`;
}

function hexToRgb(hex: string, fallback: Vec3): Vec3 {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex);
  if (!m) return fallback;
  return {
    x: parseInt(m[1], 16) / 255,
    y: parseInt(m[2], 16) / 255,
    z: parseInt(m[3], 16) / 255,
  };
}

export const ColorSwatch: Component<ColorSwatchProps> = (props) => {
  return (
    <input
      type="color"
      class={props.class}
      title={props.title}
      value={rgbToHex(props.value())}
      onInput={(e) => props.onCommit(hexToRgb(e.currentTarget.value, props.value()))}
    />
  );
};
