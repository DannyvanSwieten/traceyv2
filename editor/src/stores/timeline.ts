// Solid store for the playhead. The native side owns the canonical clock
// (advances in render_tick); this store mirrors that state via the
// `timeline_tick` broadcast and pushes transport commands back through the
// IPC wrappers in lib/api.ts.
//
// Mirrors the shape of stores/sops.ts: createSignal + listen() subscription
// + thin async mutators. No debouncing here — playhead seeks are user-paced.

import { createSignal } from 'solid-js';
import * as api from '../lib/api';

export type LoopMode = api.LoopMode;
export type TimelineState = api.TimelineState;

const DEFAULT_STATE: TimelineState = {
  fps: 24,
  frame_start: 1,
  frame_end: 240,
  current_time: 0,
  playing: false,
  loop: 'loop',
};

const [state, setState] = createSignal<TimelineState>(DEFAULT_STATE);
export const timeline = state;

// ── Auto-key mode ────────────────────────────────────────────────────────
// Houdini/Maya-style "autokey": while on, every parameter edit at the
// current playhead writes a keyframe to that channel. UI value-edit code
// (ActorProperties transform sliders, SopNodeInspector ParamRow inputs)
// checks this flag after the edit lands; the keyframe IPC fires with the
// new value, no extra click needed. Default off so an unsuspecting drag
// doesn't dirty the animation.
const [autoKeyInternal, setAutoKeyInternal] = createSignal(false);
export const autoKey = autoKeyInternal;
export function setAutoKey(v: boolean): void {
  setAutoKeyInternal(v);
}
export function toggleAutoKey(): void {
  setAutoKeyInternal((v) => !v);
}

// Persisted last for the LAYOUT_STORAGE_KEY hook in App.tsx; the timeline
// store itself just exposes the signal and lets the caller hydrate.

// ── Frame <-> seconds helpers ────────────────────────────────────────────
// Frame numbering is 1-based (Houdini convention). Time at frame N is
// (N-1)/fps. Helpers stay in this file so components don't need to know
// the formula.

export function secondsForFrame(frame: number, fps = state().fps): number {
  return fps > 0 ? (frame - 1) / fps : 0;
}
export function frameForSeconds(seconds: number, fps = state().fps): number {
  return fps > 0 ? seconds * fps + 1 : 1;
}
export function currentFrame(): number {
  return frameForSeconds(state().current_time, state().fps);
}

// Snap a seconds value to the nearest whole frame.
export function snapToFrame(seconds: number, fps = state().fps): number {
  return secondsForFrame(Math.round(frameForSeconds(seconds, fps)), fps);
}

// ── Initial fetch + broadcast wiring ─────────────────────────────────────

let booted = false;
export async function bootTimeline(): Promise<void> {
  if (booted) return;
  booted = true;
  try {
    const fresh = await api.timelineGet();
    setState(fresh);
  } catch (e) {
    console.warn('timelineGet failed, using defaults:', e);
  }
}

// Native broadcasts at ~30 Hz while playing; we just fold the incoming
// {time, playing} into our local mirror. No bouncing back to the server.
api.listen('timeline_tick', (msg) => {
  const time = typeof msg.time === 'number' ? msg.time : null;
  const playing = typeof msg.playing === 'boolean' ? msg.playing : null;
  if (time === null && playing === null) return;
  setState((s) => ({
    ...s,
    current_time: time ?? s.current_time,
    playing: playing ?? s.playing,
  }));
});

// ── Mutators ─────────────────────────────────────────────────────────────

export async function setRange(fps: number, frameStart: number, frameEnd: number): Promise<void> {
  await api.timelineSetRange(fps, frameStart, frameEnd);
  setState((s) => ({
    ...s,
    fps,
    frame_start: frameStart,
    frame_end: frameEnd,
    current_time: Math.min(
      Math.max(s.current_time, secondsForFrame(frameStart, fps)),
      secondsForFrame(frameEnd + 1, fps),
    ),
  }));
}

export async function seekFrame(frame: number): Promise<void> {
  // Clamp to the configured range; the native side also clamps but we want
  // the optimistic update to match what the server will end up with.
  const s = state();
  const clamped = Math.min(Math.max(frame, s.frame_start), s.frame_end);
  await api.timelineSetPlayhead({ frame: clamped });
  setState((cur) => ({
    ...cur,
    current_time: secondsForFrame(clamped, cur.fps),
    playing: false,
  }));
}

export async function seekSeconds(time: number): Promise<void> {
  await api.timelineSetPlayhead({ time });
  setState((s) => ({ ...s, current_time: time, playing: false }));
}

export async function play(): Promise<void> {
  await api.timelinePlay();
  setState((s) => ({ ...s, playing: true }));
}

export async function pause(): Promise<void> {
  await api.timelinePause();
  setState((s) => ({ ...s, playing: false }));
}

export async function togglePlayPause(): Promise<void> {
  if (state().playing) await pause();
  else await play();
}

export async function setLoopMode(mode: LoopMode): Promise<void> {
  await api.timelineSetLoop(mode);
  setState((s) => ({ ...s, loop: mode }));
}

// ── Keyframe convenience wrappers ────────────────────────────────────────
// Most callers want "set a key for axis X of a vec3 at the current playhead".
// These thin helpers keep components from re-writing the same arg shapes.

export async function setKeyAtPlayhead(args: {
  nodeUid: number;
  paramName: string;
  component: number;
  value: number;
  interp?: api.Interp;
}): Promise<void> {
  const t = state().current_time;
  await api.paramSetKeyframe({
    nodeUid: args.nodeUid,
    paramName: args.paramName,
    component: args.component,
    time: t,
    value: args.value,
    interp: args.interp ?? 'linear',
  });
}

export async function deleteKeyAtPlayhead(args: {
  nodeUid: number;
  paramName: string;
  component: number;
  fps?: number;
}): Promise<boolean> {
  // Snap to the nearest frame so a click matches the on-screen marker even
  // if the playhead wasn't perfectly aligned.
  const t = snapToFrame(state().current_time, args.fps ?? state().fps);
  return api.paramDeleteKeyframe({
    nodeUid: args.nodeUid,
    paramName: args.paramName,
    component: args.component,
    time: t,
  });
}
