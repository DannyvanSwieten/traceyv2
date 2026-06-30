import { createSignal, onMount, onCleanup, Show } from 'solid-js';
import {
  ShotState,
  getShotState,
  createShot,
  openShot,
  closeShot,
  saveShot,
  listen,
} from '../../lib/api';

// Shot lifecycle controls (USD shot mode). New/Open enters shot mode (the composed
// stage becomes the working scene); Save/Close manage it. The active DEPARTMENT (which
// USD layer edits author into) is chosen in the DepartmentBar, not here. Parallel/
// opt-in: until New/Open is used, nothing here affects the procedural workflow.
export function ShotControls() {
  const [state, setState] = createSignal<ShotState>({ open: false, departments: [], active: null });
  const [busy, setBusy] = createSignal(false);

  const run = async (fn: () => Promise<ShotState | boolean>) => {
    setBusy(true);
    try {
      const r = await fn();
      if (typeof r === 'object') setState(r);
      else setState(await getShotState());
    } catch (e) {
      // alert() may be a no-op in the WKWebView; log regardless.
      console.error('[shot]', e);
      // eslint-disable-next-line no-alert
      try { alert('Shot command failed: ' + (e as Error).message); } catch { /* ignore */ }
    } finally {
      setBusy(false);
    }
  };

  onMount(async () => {
    try { setState(await getShotState()); } catch { /* no USD build → leave closed */ }
    const off = listen('shot_state', (msg) => {
      const s = (msg as { state?: ShotState }).state;
      if (s) setState(s);
    });
    onCleanup(off);
  });

  return (
    <div class="shot-controls">
      <Show
        when={state().open}
        fallback={
          <>
            <button
              class="toolbar-button"
              type="button"
              disabled={busy()}
              title="Create a USD shot (layout / anim / lighting / render layers) in the project folder and enter shot mode."
              onClick={() => void run(() => createShot())}
            >
              New Shot
            </button>
            <button
              class="toolbar-button"
              type="button"
              disabled={busy()}
              title="Open the project's shot.usda and enter shot mode."
              onClick={() => void run(() => openShot())}
            >
              Open Shot
            </button>
          </>
        }
      >
        <span class="shot-controls__label">Shot</span>
        <button
          class="toolbar-button"
          type="button"
          disabled={busy()}
          title="Save all department layers to disk."
          onClick={() => void run(() => saveShot())}
        >
          Save Shot
        </button>
        <button
          class="toolbar-button"
          type="button"
          disabled={busy()}
          title="Leave shot mode."
          onClick={() => void run(() => closeShot())}
        >
          Close
        </button>
      </Show>
    </div>
  );
}
