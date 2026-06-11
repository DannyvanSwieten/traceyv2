// App-global input: pointer tracking, the Blender-style modal grab
// session (G/R/S + axis locks + Enter/Esc), the hold-Q pie menu, and the
// window-level keyboard dispatcher (command palette, undo/redo, timeline
// transport, K-to-key). Extracted from App.tsx so layout and input stop
// sharing one component.
//
// installAppInput() wires every listener and returns a cleanup. The one
// dependency that can't be imported — the viewport re-render hook — is
// injected via options.

import * as api from './api';
import {
  startGrab,
  updateGrab,
  setAxis,
  setOrigin as setGrabOrigin,
  commitGrab,
  cancelGrab,
  grabState,
  isGrabActive,
  type Axis,
} from './viewport_grab';
import {
  openPieMenu,
  commitPieMenu,
  dismissPieMenu,
  updatePieMenuCursor,
  setPieMenuWedges,
  isPieMenuOpen,
} from './pie_menu';
import { toggleCommandPalette } from './command_palette';
import { autoKeyEnabled } from './auto_key';
import {
  keySelectedActorPose,
  refreshActors,
  selectedActor,
} from '../stores/actors';
import { redo, undo } from '../stores/sops';
import {
  currentFrame,
  seekFrame,
  timeline,
  togglePlayPause,
} from '../stores/timeline';

// Latest pointer position in client coords, updated globally on
// pointermove. The modal grab uses this both as the grab origin (so it
// doesn't snap to wherever the cursor was at app start) and as the
// per-frame delta source; the keyboard dispatcher uses it to open the
// pie menu under the cursor.
export const lastViewportPointer = { x: 0, y: 0 };

// Whether the next native pointer broadcast after grab-activation should
// be treated as the origin. The Metal-view pointer coords don't agree
// with browser clientX/Y so we re-seed the grab's startX/Y from the
// first sample.
let needGrabOrigin = false;

// Wrappers that flip the native grab-active flag alongside the JS state.
// Mandatory because pointer events over the Metal viewport never reach
// the WebView — the engine has to broadcast pointer state back to us.
export async function startGrabSession(
  kind: 'translate' | 'rotate' | 'scale',
  actorId: number,
  transform: api.Transform,
  pointerX: number,
  pointerY: number,
): Promise<void> {
  // Flip the native gate FIRST so the engine stops orbiting the camera
  // even if startGrab (which awaits a getCamera roundtrip) takes a few
  // frames. Without this leading call, dragging during the window
  // between "G pressed" and "grab fully armed" still pans / orbits.
  try {
    await api.setViewportGrabActive(true);
  } catch (e) {
    console.error('setViewportGrabActive(true) failed:', e);
    return;
  }
  try {
    await startGrab(kind, actorId, transform, pointerX, pointerY);
  } catch (e) {
    console.error('startGrab failed:', e);
    api.setViewportGrabActive(false).catch(() => {});
  }
}

export function endGrabSession(mode: 'commit' | 'cancel'): void {
  if (mode === 'commit') {
    commitGrab();
    if (autoKeyEnabled()) keySelectedActorPose();
  } else {
    cancelGrab().catch((err) => console.error('cancelGrab failed:', err));
  }
  api.setViewportGrabActive(false).catch(() => {});
}

// Start a grab on the currently selected actor at the last-known cursor
// position. Used by the pie menu's Translate/Rotate/Scale wedges.
function grabActiveActor(kind: 'translate' | 'rotate' | 'scale'): void {
  const a = selectedActor();
  if (!a) return;
  startGrab(kind, a.id, a.transform,
            lastViewportPointer.x, lastViewportPointer.y).catch(() => {});
}

export interface AppInputOptions {
  // Re-render the viewport after a display-toggle wedge (points / edges /
  // ground) flips engine state.
  renderViewport: () => void;
}

export function installAppInput(opts: AppInputOptions): () => void {
  // ── Pointer tracking + modal-grab pointer handling ──────────────────
  const onPointerMove = (e: PointerEvent) => {
    lastViewportPointer.x = e.clientX;
    lastViewportPointer.y = e.clientY;
    if (isGrabActive()) {
      const next = updateGrab(e.clientX, e.clientY);
      const g = grabState();
      if (next && g) {
        api.setActorTransform(g.actorId, next).catch(() => {});
      }
    }
    if (isPieMenuOpen()) {
      updatePieMenuCursor(e.clientX, e.clientY);
    }
  };
  // Left-click commits, right-click cancels — matches Blender. Only
  // covers events that DO reach the WebView (i.e. on dock/panel area);
  // events over the Metal viewport are handled by the native pointer
  // broadcast below.
  const onPointerDown = (e: PointerEvent) => {
    if (!isGrabActive()) return;
    if (e.button === 0) {
      e.preventDefault();
      endGrabSession('commit');
    } else if (e.button === 2) {
      e.preventDefault();
      endGrabSession('cancel');
    }
  };
  window.addEventListener('pointermove', onPointerMove);
  window.addEventListener('pointerdown', onPointerDown, { capture: true });

  // Native pointer-state broadcast: fires per render_tick while the grab
  // is active. Drives updateGrab so the actor follows the cursor.
  // Click-to-commit is intentionally NOT wired here — too easy to
  // misfire when the user click-drags (their camera-orbit instinct).
  // Use Enter to commit, Esc to cancel.
  const unlistenViewportPointer = api.listen('viewport_pointer', (msg) => {
    if (!isGrabActive()) return;
    const x = msg.x as number;
    const y = msg.y as number;
    if (needGrabOrigin) {
      setGrabOrigin(x, y);
      needGrabOrigin = false;
      return;
    }
    const next = updateGrab(x, y);
    const g = grabState();
    if (next && g) api.setActorTransform(g.actorId, next).catch(() => {});
  });

  // ── Pie menu wedges ──────────────────────────────────────────────────
  // Eight wedges arranged radially — the operations users reach for most
  // often while their eyes are on the viewport. Q opens the menu at the
  // cursor; releasing Q over a wedge commits it.
  setPieMenuWedges([
    { label: 'Translate', run: () => grabActiveActor('translate') },
    { label: 'Rotate',    run: () => grabActiveActor('rotate') },
    { label: 'Scale',     run: () => grabActiveActor('scale') },
    { label: 'Toggle Points', run: async () => {
      try {
        const cur = await api.getShowPoints();
        await api.setShowPoints(!cur);
        opts.renderViewport();
      } catch (e) { console.error('toggle points:', e); }
    } },
    { label: 'Toggle Edges', run: async () => {
      try {
        const cur = await api.getShowEdges();
        await api.setShowEdges(!cur);
        opts.renderViewport();
      } catch (e) { console.error('toggle edges:', e); }
    } },
    { label: 'Toggle Ground', run: async () => {
      try {
        const cur = await api.getShowGround();
        await api.setShowGround(!cur);
        opts.renderViewport();
      } catch (e) { console.error('toggle ground:', e); }
    } },
    { label: 'Persp View', run: () => { api.setCameraView('persp').catch(() => {}); } },
    { label: 'Commands…', run: toggleCommandPalette },
  ]);

  // ── Window-level keyboard dispatcher ─────────────────────────────────
  // Not in the native menu because that would unconditionally swallow the
  // keys and break native text-input editing inside the inspector. Here
  // we only intercept when focus is NOT on an editable element.
  //
  // Transport shortcuts:
  //   Space          → play / pause
  //   ←  / →         → step one frame
  //   Home / End     → jump to range start / end
  //   K              → key the selected actor's pose at the playhead
  const onAppKeyDown = (e: KeyboardEvent) => {
    const target = e.target as HTMLElement | null;
    const tag = target?.tagName;
    const editable =
      tag === 'INPUT' ||
      tag === 'TEXTAREA' ||
      target?.isContentEditable === true;
    if (editable) return;

    // Command palette: Cmd+K / Ctrl+K, also Cmd+P / Ctrl+P (VS Code
    // convention). Toggle so a second press dismisses it.
    if ((e.metaKey || e.ctrlKey) && !e.altKey && !e.shiftKey) {
      const k = e.key.toLowerCase();
      if (k === 'k' || k === 'p') {
        e.preventDefault();
        toggleCommandPalette();
        return;
      }
    }

    // Hold-Q pie menu: open on keydown at the current cursor, commit
    // wedge on keyup. e.repeat gate prevents auto-repeat from spamming
    // openPieMenu() while the key is held down.
    if (!e.metaKey && !e.ctrlKey && !e.altKey) {
      const k = e.key.toLowerCase();
      if (k === 'q' && !e.repeat) {
        if (!isPieMenuOpen()) {
          e.preventDefault();
          openPieMenu(lastViewportPointer.x, lastViewportPointer.y);
          return;
        }
      }
    }

    // Blender-style modal transform (G/R/S + X/Y/Z + Enter/Esc). Only
    // fires when an actor is selected and no modifiers are held.
    if (!e.metaKey && !e.ctrlKey && !e.altKey) {
      const k = e.key.toLowerCase();
      if (isGrabActive()) {
        if (k === 'x' || k === 'y' || k === 'z') {
          e.preventDefault();
          setAxis(k as Axis);
          return;
        }
        if (e.key === 'Escape') {
          e.preventDefault();
          endGrabSession('cancel');
          return;
        }
        if (e.key === 'Enter') {
          e.preventDefault();
          endGrabSession('commit');
          return;
        }
      } else if (k === 'g' || k === 'r' || k === 's') {
        const actor = selectedActor();
        if (actor) {
          e.preventDefault();
          const kind = k === 'g' ? 'translate' : k === 'r' ? 'rotate' : 'scale';
          // The first native pointer broadcast becomes the origin —
          // see needGrabOrigin in the viewport_pointer listener.
          needGrabOrigin = true;
          startGrabSession(kind, actor.id, actor.transform,
                           lastViewportPointer.x, lastViewportPointer.y)
            .catch((err) => console.error('startGrabSession failed:', err));
          return;
        }
      }
    }

    // Undo / redo — these have a modifier and shouldn't conflict with
    // anything else.
    const isUndoCombo =
      (e.metaKey || e.ctrlKey) && !e.altKey && e.key.toLowerCase() === 'z';
    if (isUndoCombo) {
      e.preventDefault();
      const op = e.shiftKey ? redo : undo;
      op()
        .then((applied) => {
          if (applied) void refreshActors('refresh after undo/redo');
        })
        .catch((err) => console.warn('undo/redo failed:', err));
      return;
    }

    // The SOP graph canvas owns its own Space-to-pan + Delete shortcuts.
    // When the canvas has focus, defer to it so we don't double-handle.
    const insideSopDock = target?.closest?.('.sop-graph-dock');
    if (insideSopDock) return;

    // Transport — no modifiers (so Cmd+Arrow etc. fall through to OS).
    if (e.metaKey || e.ctrlKey || e.altKey) return;

    const t = timeline();
    switch (e.key) {
      case ' ':
        e.preventDefault();
        togglePlayPause();
        break;
      case 'ArrowLeft':
        e.preventDefault();
        seekFrame(Math.round(currentFrame()) - 1);
        break;
      case 'ArrowRight':
        e.preventDefault();
        seekFrame(Math.round(currentFrame()) + 1);
        break;
      case 'Home':
        e.preventDefault();
        seekFrame(t.frame_start);
        break;
      case 'End':
        e.preventDefault();
        seekFrame(t.frame_end);
        break;
      case 'k':
      case 'K':
        e.preventDefault();
        keySelectedActorPose();
        break;
    }
  };
  // Q keyup commits the pie menu's currently-hovered wedge. Window-level
  // so a press-and-flick that ends with focus elsewhere still fires.
  const onAppKeyUp = (e: KeyboardEvent) => {
    if (e.key.toLowerCase() === 'q' && isPieMenuOpen()) {
      e.preventDefault();
      commitPieMenu();
    }
  };
  window.addEventListener('keydown', onAppKeyDown);
  window.addEventListener('keyup', onAppKeyUp);

  return () => {
    window.removeEventListener('pointermove', onPointerMove);
    window.removeEventListener('pointerdown', onPointerDown, { capture: true } as EventListenerOptions);
    window.removeEventListener('keydown', onAppKeyDown);
    window.removeEventListener('keyup', onAppKeyUp);
    unlistenViewportPointer();
    dismissPieMenu();
    // Never leave the native grab gate stuck on — a teardown mid-grab
    // would otherwise disable camera input until restart.
    if (isGrabActive()) {
      cancelGrab().catch(() => {});
      api.setViewportGrabActive(false).catch(() => {});
    }
  };
}
