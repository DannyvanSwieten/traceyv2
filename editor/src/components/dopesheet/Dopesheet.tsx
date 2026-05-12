import { Component, For, Index, Show, createEffect, createMemo, createSignal, onCleanup } from 'solid-js';
import * as api from '../../lib/api';
import { AnimatedChannel, listAnimatedChannels } from '../../lib/animated_channels';
import { sopGraph } from '../../stores/sops';
import {
  currentFrame,
  frameForSeconds,
  secondsForFrame,
  seekFrame,
  snapToFrame,
  timeline,
} from '../../stores/timeline';
import './Dopesheet.css';

// Module-local clipboard for key copy/paste. Lives outside the component so
// the same value survives unmount/remount of the dopesheet and is shared
// across right-click menus on different keys / channels.
interface CopiedKey {
  v: number;
  in: number;
  out: number;
  i: api.Interp;
}
const [copiedKey, setCopiedKey] = createSignal<CopiedKey | null>(null);

// Pixels reserved for the channel-list column on the left. Mirrored in CSS.
const CHANNEL_LIST_WIDTH = 180;
const ROW_HEIGHT = 22;

interface DragState {
  channel: AnimatedChannel;
  fromTime: number;
  // Set to true once the pointer has moved enough to commit the drag, so a
  // simple click doesn't trigger a no-op retime.
  committed: boolean;
  trackEl: HTMLElement;
}

// Bottom-of-window animation editor. Channel list on the left, frame ruler
// across the top, key diamonds on each row. Click on a diamond to select +
// drag-to-retime; double-click deletes; clicking the ruler scrubs.
//
// Inserting new keys is *not* done from the dopesheet in v1 — that goes
// through the keyframe dot in the inspector. The dopesheet only edits keys
// that already exist.
export const Dopesheet: Component = () => {
  const channels = createMemo(() => listAnimatedChannels(sopGraph()));

  const tickFrames = createMemo(() => {
    const t = timeline();
    const span = Math.max(t.frame_end - t.frame_start, 1);
    // Aim for ~10 visible labels regardless of range; round to nice steps.
    const targetCount = 10;
    const rough = span / targetCount;
    const niceSteps = [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000];
    let step = niceSteps[niceSteps.length - 1];
    for (const s of niceSteps) {
      if (s >= rough) { step = s; break; }
    }
    const ticks: number[] = [];
    for (let f = t.frame_start; f <= t.frame_end; f += step) ticks.push(f);
    if (ticks[ticks.length - 1] !== t.frame_end) ticks.push(t.frame_end);
    return ticks;
  });

  // Convert (frame) ↔ pixel-x within a track row, given the track's width.
  const fracForFrame = (frame: number): number => {
    const t = timeline();
    const span = Math.max(t.frame_end - t.frame_start, 1);
    return (frame - t.frame_start) / span;
  };
  const frameForX = (x: number, width: number): number => {
    const t = timeline();
    const span = Math.max(t.frame_end - t.frame_start, 1);
    const frac = Math.min(Math.max(x / Math.max(width, 1), 0), 1);
    return t.frame_start + frac * span;
  };

  let drag: DragState | null = null;

  // Live drag overlay position; rendered as a "ghost" diamond following the
  // pointer until release commits the move.
  let dragGhost: HTMLDivElement | undefined;
  const setGhostFrame = (frame: number) => {
    if (!dragGhost) return;
    dragGhost.style.setProperty('--ghost-frac', String(fracForFrame(frame)));
    dragGhost.style.removeProperty('display');
  };
  const hideGhost = () => {
    if (dragGhost) dragGhost.style.display = 'none';
  };

  const onKeyPointerDown = (
    e: PointerEvent,
    channel: AnimatedChannel,
    keyTime: number,
  ) => {
    e.stopPropagation();
    e.preventDefault();
    const trackEl = (e.currentTarget as HTMLElement).closest('.dopesheet-track') as HTMLElement | null;
    if (!trackEl) return;
    drag = { channel, fromTime: keyTime, committed: false, trackEl };
    trackEl.setPointerCapture(e.pointerId);
    setGhostFrame(frameForSeconds(keyTime, timeline().fps));
  };

  const onTrackPointerMove = (e: PointerEvent) => {
    if (!drag) return;
    const rect = drag.trackEl.getBoundingClientRect();
    const f = snapFrameToInt(frameForX(e.clientX - rect.left, rect.width));
    setGhostFrame(f);
    if (!drag.committed) drag.committed = true;
  };

  const onTrackPointerUp = async (e: PointerEvent) => {
    if (!drag) return;
    const trackEl = drag.trackEl;
    trackEl.releasePointerCapture(e.pointerId);
    hideGhost();
    if (!drag.committed) { drag = null; return; }

    const rect = trackEl.getBoundingClientRect();
    const targetFrame = snapFrameToInt(frameForX(e.clientX - rect.left, rect.width));
    const targetTime = snapToFrame(secondsForFrame(targetFrame, timeline().fps));
    const fromTime = drag.fromTime;
    const ch = drag.channel;
    drag = null;

    if (Math.abs(targetTime - fromTime) < 1e-6) return;
    try {
      await api.paramMoveKeyframe({
        nodeUid: ch.nodeUid,
        paramName: ch.paramName,
        component: ch.component,
        fromTime,
        toTime: targetTime,
      });
    } catch (err) {
      console.warn('move keyframe failed:', err);
    }
  };

  const onKeyDoubleClick = async (
    e: MouseEvent,
    channel: AnimatedChannel,
    keyTime: number,
  ) => {
    e.stopPropagation();
    try {
      await api.paramDeleteKeyframe({
        nodeUid: channel.nodeUid,
        paramName: channel.paramName,
        component: channel.component,
        time: keyTime,
      });
    } catch (err) {
      console.warn('delete keyframe failed:', err);
    }
  };

  // ── Right-click context menu ─────────────────────────────────────────
  // Per-key actions: change interpolation, copy/paste, delete. The menu
  // captures the channel + key snapshot at open time, so subsequent actions
  // don't race with a graph reload mid-menu.
  interface ContextState {
    x: number;
    y: number;
    channel: AnimatedChannel;
    key: AnimatedChannel['keys'][number];
  }
  const [ctxMenu, setCtxMenu] = createSignal<ContextState | null>(null);
  let ctxMenuEl: HTMLDivElement | undefined;
  // Reactively write the menu position to CSS variables (mirrors the
  // playhead-position / dopesheet-height pattern, avoids inline-style lint).
  createEffect(() => {
    const m = ctxMenu();
    if (!m || !ctxMenuEl) return;
    ctxMenuEl.style.setProperty('--menu-x', `${m.x}px`);
    ctxMenuEl.style.setProperty('--menu-y', `${m.y}px`);
  });

  const onKeyContextMenu = (
    e: MouseEvent,
    channel: AnimatedChannel,
    key: AnimatedChannel['keys'][number],
  ) => {
    e.preventDefault();
    e.stopPropagation();
    setCtxMenu({ x: e.clientX, y: e.clientY, channel, key });
  };

  const closeCtxMenu = () => setCtxMenu(null);

  // Dismiss the menu on outside click or Escape. Listeners attach only
  // while the menu is open so we don't keep dead handlers on the window.
  createEffect(() => {
    if (!ctxMenu()) return;
    const onDocPointerDown = (e: PointerEvent) => {
      const t = e.target as Node;
      if (ctxMenuEl && ctxMenuEl.contains(t)) return;
      closeCtxMenu();
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') closeCtxMenu();
    };
    window.addEventListener('pointerdown', onDocPointerDown, true);
    window.addEventListener('keydown', onKey);
    onCleanup(() => {
      window.removeEventListener('pointerdown', onDocPointerDown, true);
      window.removeEventListener('keydown', onKey);
    });
  });

  // Replace the key at its current time with the same value/tangents but a
  // new interp. setKey on the C++ side matches by time and overwrites.
  const setKeyInterp = async (m: ContextState, interp: api.Interp) => {
    closeCtxMenu();
    try {
      await api.paramSetKeyframe({
        nodeUid: m.channel.nodeUid,
        paramName: m.channel.paramName,
        component: m.channel.component,
        time: m.key.t,
        value: m.key.v,
        interp,
        inTangent: m.key.in,
        outTangent: m.key.out,
      });
    } catch (err) {
      console.warn('change interp failed:', err);
    }
  };

  const copyKey = (m: ContextState) => {
    setCopiedKey({ v: m.key.v, in: m.key.in, out: m.key.out, i: m.key.i as api.Interp });
    closeCtxMenu();
  };

  // Paste replaces the right-clicked key's properties with the copied
  // ones (time stays put — "make this key look like that one"). For
  // "insert a fresh key at the playhead", the keyframe dot in the
  // inspector still works.
  const pasteKey = async (m: ContextState) => {
    const buf = copiedKey();
    if (!buf) return;
    closeCtxMenu();
    try {
      await api.paramSetKeyframe({
        nodeUid: m.channel.nodeUid,
        paramName: m.channel.paramName,
        component: m.channel.component,
        time: m.key.t,
        value: buf.v,
        interp: buf.i,
        inTangent: buf.in,
        outTangent: buf.out,
      });
    } catch (err) {
      console.warn('paste key failed:', err);
    }
  };

  const deleteKeyMenu = async (m: ContextState) => {
    closeCtxMenu();
    try {
      await api.paramDeleteKeyframe({
        nodeUid: m.channel.nodeUid,
        paramName: m.channel.paramName,
        component: m.channel.component,
        time: m.key.t,
      });
    } catch (err) {
      console.warn('delete keyframe failed:', err);
    }
  };

  // Scrub on the frame ruler (and on track rows that aren't being key-dragged).
  const onRulerPointerDown = (e: PointerEvent) => {
    if (drag) return;
    const target = e.currentTarget as HTMLElement;
    target.setPointerCapture(e.pointerId);
    scrubFromEvent(e, target);
  };
  const onRulerPointerMove = (e: PointerEvent) => {
    if (!(e.buttons & 1)) return;
    const target = e.currentTarget as HTMLElement;
    if (target.hasPointerCapture(e.pointerId)) {
      scrubFromEvent(e, target);
    }
  };
  const onRulerPointerUp = (e: PointerEvent) => {
    const target = e.currentTarget as HTMLElement;
    if (target.hasPointerCapture(e.pointerId)) target.releasePointerCapture(e.pointerId);
  };
  const scrubFromEvent = (e: PointerEvent, target: HTMLElement) => {
    const rect = target.getBoundingClientRect();
    const f = snapFrameToInt(frameForX(e.clientX - rect.left, rect.width));
    seekFrame(f).catch((err) => console.warn('scrub failed:', err));
  };

  // Live-update the playhead column position via CSS variable instead of an
  // inline style — same pattern Playbar uses.
  let lanesEl: HTMLDivElement | undefined;
  createEffect(() => {
    if (!lanesEl) return;
    lanesEl.style.setProperty(
      '--playhead-frac',
      String(fracForFrame(currentFrame())),
    );
  });

  return (
    <div class="dopesheet">
      <div class="dopesheet-header">
        <div class="dopesheet-header-channels">
          Animation Editor
          <span class="dopesheet-header-count">
            {channels().length} channel{channels().length === 1 ? '' : 's'}
          </span>
        </div>
        <div
          class="dopesheet-ruler"
          onPointerDown={onRulerPointerDown}
          onPointerMove={onRulerPointerMove}
          onPointerUp={onRulerPointerUp}
        >
          <Index each={tickFrames()}>
            {(f) => (
              <div
                class="dopesheet-tick"
                ref={(el) =>
                  el.style.setProperty('--frac', String(fracForFrame(f())))
                }
              >
                <span class="dopesheet-tick-label">{f()}</span>
              </div>
            )}
          </Index>
        </div>
      </div>

      <div class="dopesheet-body">
        <div class="dopesheet-channel-list">
          <For each={channels()}>
            {(ch) => (
              <div class="dopesheet-channel-row" title={ch.label}>
                <span class="dopesheet-channel-label">{ch.label}</span>
              </div>
            )}
          </For>
          <Show when={channels().length === 0}>
            <div class="dopesheet-empty">
              No animated channels.
              <br />
              Click a keyframe diamond next to a parameter to start animating.
            </div>
          </Show>
        </div>

        <div
          class="dopesheet-lanes"
          ref={lanesEl}
          onPointerMove={onTrackPointerMove}
          onPointerUp={onTrackPointerUp}
        >
          <For each={channels()}>
            {(ch) => (
              <div class="dopesheet-track">
                <For each={ch.keys}>
                  {(k) => (
                    <button
                      type="button"
                      class="dopesheet-key"
                      title={`Frame ${Math.round(frameForSeconds(k.t, timeline().fps))}: ${k.v.toFixed(3)}`}
                      ref={(el) =>
                        el.style.setProperty(
                          '--frac',
                          String(fracForFrame(frameForSeconds(k.t, timeline().fps))),
                        )
                      }
                      onPointerDown={(e) => onKeyPointerDown(e, ch, k.t)}
                      onDblClick={(e) => onKeyDoubleClick(e, ch, k.t)}
                      onContextMenu={(e) => onKeyContextMenu(e, ch, k)}
                    >
                      <span class="dopesheet-key-diamond" />
                    </button>
                  )}
                </For>
              </div>
            )}
          </For>

          <div class="dopesheet-playhead" />
          <div ref={dragGhost} class="dopesheet-drag-ghost" />
        </div>
      </div>

      <Show when={ctxMenu()}>
        {(m) => (
          <div
            ref={ctxMenuEl}
            class="dopesheet-ctx-menu"
          >
            <div class="dopesheet-ctx-section-label">Interpolation</div>
            <button
              type="button"
              class={'dopesheet-ctx-item' + (m().key.i === 'step' ? ' is-current' : '')}
              onClick={() => setKeyInterp(m(), 'step')}
            >
              <span class="dopesheet-ctx-mark">{m().key.i === 'step' ? '✓' : ''}</span>
              Step
            </button>
            <button
              type="button"
              class={'dopesheet-ctx-item' + (m().key.i === 'linear' ? ' is-current' : '')}
              onClick={() => setKeyInterp(m(), 'linear')}
            >
              <span class="dopesheet-ctx-mark">{m().key.i === 'linear' ? '✓' : ''}</span>
              Linear
            </button>
            <button
              type="button"
              class={'dopesheet-ctx-item' + (m().key.i === 'bezier' ? ' is-current' : '')}
              onClick={() => setKeyInterp(m(), 'bezier')}
            >
              <span class="dopesheet-ctx-mark">{m().key.i === 'bezier' ? '✓' : ''}</span>
              Bezier
            </button>
            <div class="dopesheet-ctx-separator" />
            <button
              type="button"
              class="dopesheet-ctx-item"
              onClick={() => copyKey(m())}
            >
              <span class="dopesheet-ctx-mark" />
              Copy
            </button>
            <button
              type="button"
              class="dopesheet-ctx-item"
              disabled={!copiedKey()}
              onClick={() => pasteKey(m())}
            >
              <span class="dopesheet-ctx-mark" />
              Paste
            </button>
            <div class="dopesheet-ctx-separator" />
            <button
              type="button"
              class="dopesheet-ctx-item dopesheet-ctx-danger"
              onClick={() => deleteKeyMenu(m())}
            >
              <span class="dopesheet-ctx-mark" />
              Delete
            </button>
          </div>
        )}
      </Show>
    </div>
  );
};

function snapFrameToInt(frame: number): number {
  return Math.round(frame);
}

// Re-export the row + list widths so App.tsx can size the panel sensibly.
export const DOPESHEET_MIN_HEIGHT = 80;
export const DOPESHEET_DEFAULT_HEIGHT = 180;
export const DOPESHEET_MAX_HEIGHT = 600;
export const DOPESHEET_CHANNEL_LIST_WIDTH = CHANNEL_LIST_WIDTH;
export const DOPESHEET_ROW_HEIGHT = ROW_HEIGHT;
