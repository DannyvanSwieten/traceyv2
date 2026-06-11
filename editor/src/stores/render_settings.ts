// Render-related engine mirrors: PT preview toggle, frame-lock mode, and
// the Render workspace's max-samples / max-bounces / resolution values.
// Each signal is seeded from the engine on init and every setter pushes
// through IPC; toggles roll the signal back when the push fails so the UI
// never shows a state the engine rejected.

import { createSignal } from 'solid-js';
import * as api from '../lib/api';

const [ptPreviewSignal, setPtPreviewSignal] = createSignal(false);
const [frameLockedSignal, setFrameLockedSignal] = createSignal(false);
const [maxSamplesSignal, setMaxSamplesSignal] = createSignal(1024);
const [maxBouncesSignal, setMaxBouncesSignal] = createSignal(8);
// [w, h]. [0, 0] = match viewport pixel size; otherwise the PT renders at
// this fixed resolution and the viewport blit scales to fit.
const [resolutionSignal, setResolutionSignal] = createSignal<[number, number]>([0, 0]);

export const ptPreviewEnabled = ptPreviewSignal;
export const frameLocked = frameLockedSignal;
export const maxSamples = maxSamplesSignal;
export const maxBounces = maxBouncesSignal;
export const renderResolution = resolutionSignal;

// Seed every signal from the engine so the toolbar reflects native state
// (and any project-stored preference) before the user touches anything.
export function initRenderSettings(): void {
  api.getPtPreview()
    .then(setPtPreviewSignal)
    .catch((e) => console.warn('initial pt_preview fetch failed:', e));
  api.timelineGetFrameLocked()
    .then(setFrameLockedSignal)
    .catch((e) => console.warn('initial frame_locked fetch failed:', e));
  api.getMaxSamples().then(setMaxSamplesSignal).catch(() => {});
  api.getMaxBounces().then(setMaxBouncesSignal).catch(() => {});
  api.getPtRenderResolution()
    .then((r) => setResolutionSignal([r.width, r.height]))
    .catch(() => {});
}

// Optimistic flip + native push, rolled back on failure. The native
// handler does a synchronous compile_scene on OFF→ON, which can be a
// visible pause on heavy scenes — the user already clicked, so the wait
// is implicit consent.
export async function setPtPreview(next: boolean): Promise<boolean> {
  setPtPreviewSignal(next);
  try {
    await api.setPtPreview(next);
    return true;
  } catch (e) {
    console.warn('set_pt_preview failed:', e);
    setPtPreviewSignal(!next);
    return false;
  }
}

export async function setFrameLocked(next: boolean): Promise<boolean> {
  setFrameLockedSignal(next);
  try {
    await api.timelineSetFrameLocked(next);
    return true;
  } catch (e) {
    console.warn('set_frame_locked failed:', e);
    setFrameLockedSignal(!next);
    return false;
  }
}

export function setMaxSamples(n: number): void {
  setMaxSamplesSignal(n);
  api.setMaxSamples(n).catch((e) => console.warn('set_max_samples failed:', e));
}

export function setMaxBounces(n: number): void {
  setMaxBouncesSignal(n);
  api.setMaxBounces(n).catch((e) => console.warn('set_max_bounces failed:', e));
}

export function setRenderResolution(w: number, h: number): void {
  setResolutionSignal([w, h]);
  api.setPtRenderResolution(w, h).catch((e) =>
    console.warn('set_pt_render_resolution failed:', e));
}
