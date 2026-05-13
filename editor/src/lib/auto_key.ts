// Animation workspace "auto-key" toggle. When enabled, transform commits
// (grab/rotate/scale via the G/R/S modal, plus inspector edits in the
// future) automatically write a keyframe at the current playhead for the
// affected channels. Inspired by Maya's auto-key mode.
//
// The actual key-writing happens in App.tsx where the selected-actor +
// SOP graph context is already available; this module just owns the
// boolean signal so other parts of the UI can show/hide affordances or
// gate behaviour without prop-drilling.

import { createSignal } from 'solid-js';

const [enabled, setEnabledInternal] = createSignal(false);
export const autoKeyEnabled = enabled;
export function setAutoKey(v: boolean): void { setEnabledInternal(v); }
export function toggleAutoKey(): void { setEnabledInternal((v) => !v); }
