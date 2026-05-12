import { Component, createEffect, onMount } from 'solid-js';
import {
  autoKey,
  bootTimeline,
  currentFrame,
  frameForSeconds,
  seekFrame,
  setLoopMode,
  setRange,
  timeline,
  toggleAutoKey,
  togglePlayPause,
} from '../../stores/timeline';
import './Playbar.css';

const ICON_SKIP_BACK = '⏮';
const ICON_STEP_BACK = '◀◀';
const ICON_PLAY = '▶';
const ICON_PAUSE = '⏸';
const ICON_STEP_FWD = '▶▶';
const ICON_SKIP_FWD = '⏭';

// Bottom-of-window playbar. The native side owns the clock, so this UI is
// thin: read the timeline store, send transport commands. Scrubbing snaps to
// whole frames (matching Houdini's playbar feel).
export const Playbar: Component = () => {
  onMount(bootTimeline);

  // Reactive write of the playhead position to a CSS variable on the track
  // element. Avoids inline `style={...}` while still tracking signal changes.
  let trackEl: HTMLDivElement | undefined;
  createEffect(() => {
    if (trackEl) trackEl.style.setProperty('--playhead-frac', String(playheadFrac()));
  });

  const onScrub = (e: PointerEvent) => {
    if (!(e.buttons & 1)) return;  // primary button only
    const track = e.currentTarget as HTMLElement;
    const rect = track.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const t = timeline();
    const frac = Math.min(Math.max(x / Math.max(rect.width, 1), 0), 1);
    const frame = Math.round(
      t.frame_start + frac * (t.frame_end - t.frame_start),
    );
    seekFrame(frame).catch((err) => console.warn('scrub failed:', err));
  };

  const onScrubStart = (e: PointerEvent) => {
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    onScrub(e);
  };

  const onScrubEnd = (e: PointerEvent) => {
    (e.currentTarget as HTMLElement).releasePointerCapture(e.pointerId);
  };

  const playheadFrac = (): number => {
    const t = timeline();
    const span = Math.max(t.frame_end - t.frame_start, 1);
    const f = currentFrame();
    return Math.min(Math.max((f - t.frame_start) / span, 0), 1);
  };

  const onFrameInput = (e: Event) => {
    const f = parseFloat((e.currentTarget as HTMLInputElement).value);
    if (Number.isFinite(f)) seekFrame(Math.round(f));
  };

  const onFpsInput = (e: Event) => {
    const fps = parseFloat((e.currentTarget as HTMLInputElement).value);
    if (!Number.isFinite(fps) || fps <= 0) return;
    const t = timeline();
    setRange(fps, t.frame_start, t.frame_end);
  };

  const onStartInput = (e: Event) => {
    const v = parseInt((e.currentTarget as HTMLInputElement).value, 10);
    if (!Number.isFinite(v)) return;
    const t = timeline();
    setRange(t.fps, v, Math.max(t.frame_end, v));
  };

  const onEndInput = (e: Event) => {
    const v = parseInt((e.currentTarget as HTMLInputElement).value, 10);
    if (!Number.isFinite(v)) return;
    const t = timeline();
    setRange(t.fps, Math.min(t.frame_start, v), v);
  };

  const onLoopChange = (e: Event) => {
    const mode = (e.currentTarget as HTMLSelectElement).value as
      | 'once'
      | 'loop'
      | 'pingpong';
    setLoopMode(mode);
  };

  return (
    <div class="playbar">
      <div class="playbar-transport">
        <button
          type="button"
          class="playbar-btn"
          title="Go to start"
          onClick={() => seekFrame(timeline().frame_start)}
        >
          {ICON_SKIP_BACK}
        </button>
        <button
          type="button"
          class="playbar-btn"
          title="Step back one frame"
          onClick={() => seekFrame(Math.round(currentFrame()) - 1)}
        >
          {ICON_STEP_BACK}
        </button>
        <button
          type="button"
          class="playbar-btn playbar-btn-primary"
          title={timeline().playing ? 'Pause' : 'Play'}
          onClick={() => togglePlayPause()}
        >
          {timeline().playing ? ICON_PAUSE : ICON_PLAY}
        </button>
        <button
          type="button"
          class="playbar-btn"
          title="Step forward one frame"
          onClick={() => seekFrame(Math.round(currentFrame()) + 1)}
        >
          {ICON_STEP_FWD}
        </button>
        <button
          type="button"
          class="playbar-btn"
          title="Go to end"
          onClick={() => seekFrame(timeline().frame_end)}
        >
          {ICON_SKIP_FWD}
        </button>
      </div>

      <div class="playbar-range">
        <input
          class="playbar-num"
          type="number"
          step="1"
          title="Frame range start"
          value={timeline().frame_start}
          onChange={onStartInput}
        />
      </div>

      <div
        class="playbar-track"
        ref={trackEl}
        role="slider"
        aria-label="Timeline scrubber"
        aria-valuemin={String(timeline().frame_start)}
        aria-valuemax={String(timeline().frame_end)}
        aria-valuenow={String(Math.round(currentFrame()))}
        onPointerDown={onScrubStart}
        onPointerMove={onScrub}
        onPointerUp={onScrubEnd}
      >
        <div class="playbar-playhead" />
      </div>

      <div class="playbar-range">
        <input
          class="playbar-num"
          type="number"
          step="1"
          title="Frame range end"
          value={timeline().frame_end}
          onChange={onEndInput}
        />
      </div>

      <div class="playbar-frame">
        <input
          class="playbar-num playbar-frame-input"
          type="number"
          step="1"
          title="Current frame"
          value={Math.round(frameForSeconds(timeline().current_time, timeline().fps))}
          onChange={onFrameInput}
        />
        <span class="playbar-frame-total">/ {timeline().frame_end}</span>
      </div>

      <div class="playbar-fps">
        <input
          class="playbar-num"
          type="number"
          step="1"
          min="1"
          title="Frames per second"
          value={timeline().fps}
          onChange={onFpsInput}
        />
        <span class="playbar-fps-label">fps</span>
      </div>

      <button
        type="button"
        class={'playbar-btn playbar-autokey' + (autoKey() ? ' is-on' : '')}
        title={
          autoKey()
            ? 'Auto-key ON — param edits write keyframes at the playhead. Click to disable.'
            : 'Auto-key OFF — click to enable. Param edits will then auto-keyframe at the current frame.'
        }
        onClick={() => toggleAutoKey()}
      >
        AK
      </button>

      <select
        class="playbar-loop"
        title="Loop mode"
        value={timeline().loop}
        onChange={onLoopChange}
      >
        <option value="once">Once</option>
        <option value="loop">Loop</option>
        <option value="pingpong">Ping-pong</option>
      </select>
    </div>
  );
};
