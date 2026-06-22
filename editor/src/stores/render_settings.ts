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
// Path-tracer backend: 'auto' | 'metal' (GPU) | 'cpu'.
const [ptBackendSignal, setPtBackendSignal] = createSignal<api.PtBackend>('auto');

// OIDN denoise preference (a UI/output setting, not engine state — applied at
// still/sequence render time). Persisted to localStorage so it survives a
// reload. Defaults ON when the build links OIDN. `denoiserAvailable` mirrors
// whether the native build linked OIDN at all (TRACEY_WITH_OIDN); the UI greys
// the toggle out when false.
const DENOISE_KEY = 'tracey.denoiseEnabled';
const [denoiseSignal, setDenoiseSignal] = createSignal<boolean>(
  localStorage.getItem(DENOISE_KEY) !== '0'
);
const [denoiserAvailableSignal, setDenoiserAvailableSignal] = createSignal(false);

export const ptPreviewEnabled = ptPreviewSignal;
export const frameLocked = frameLockedSignal;
export const maxSamples = maxSamplesSignal;
export const maxBounces = maxBouncesSignal;
export const renderResolution = resolutionSignal;
export const ptBackend = ptBackendSignal;
export const denoiseEnabled = denoiseSignal;
export const denoiserAvailable = denoiserAvailableSignal;

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
  api.getPtBackend()
    .then((b) => setPtBackendSignal(b as api.PtBackend))
    .catch(() => {});
  api.getDenoiserAvailable()
    .then(setDenoiserAvailableSignal)
    .catch(() => {});
}

// Pure UI/output preference — no engine push; persisted for next session and
// read at render time by the still/sequence export paths.
export function setDenoiseEnabled(next: boolean): void {
  setDenoiseSignal(next);
  try {
    localStorage.setItem(DENOISE_KEY, next ? '1' : '0');
  } catch { /* private mode / quota — non-fatal */ }
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

// Optimistic set + push, rolled back on failure. Recreates the path tracer
// engine-side and restarts accumulation on the chosen backend.
export function setPtBackend(b: api.PtBackend): void {
  const prev = ptBackendSignal();
  setPtBackendSignal(b);
  api.setPtBackend(b).catch((e) => {
    console.warn('set_pt_backend failed:', e);
    setPtBackendSignal(prev);
  });
}
