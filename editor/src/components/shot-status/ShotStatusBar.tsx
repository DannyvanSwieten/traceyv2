import { createSignal, onMount, onCleanup, Show, For } from 'solid-js';
import * as api from '../../lib/api';
import { selectedActor } from '../../stores/actors';
import { activeWorkspace } from '../../lib/workspaces';
import './ShotStatusBar.css';

// Always-visible CONTEXT bar. Answers, at a glance: am I editing an Asset or composing
// a Shot? which one? which department, and where do my edits go? what's selected? The
// active department's hue accents the bar (via a CSS-variable class) so the mode reads
// by colour. Also folds in the shot New/Open/Save/Close + the layer stack.

// Shot department (USD layer) → display label. Hue lives in CSS (.shotbar--accent-*).
const LAYER_LABEL: Record<string, string> = {
  layout: 'Layout', anim: 'Animation', lighting: 'Lighting', render: 'Rendering',
};
// Asset sub-mode from the active workspace.
const SUBMODE: Record<string, string> = { modeling: 'Model', shading: 'Look', simulation: 'Sim' };

export function ShotStatusBar() {
  const [s, setS] = createSignal<api.ShotState>({ open: false, departments: [], active: null, name: null });
  const [assetName, setAssetName] = createSignal<string>('');
  const [busy, setBusy] = createSignal(false);

  const run = async (fn: () => Promise<unknown>) => {
    setBusy(true);
    try {
      await fn();
      setS(await api.getShotState());
    } catch (e) {
      console.error('[shot]', e);
      try { alert('Shot command failed: ' + (e as Error).message); } catch { /* WKWebView */ }
    } finally {
      setBusy(false);
    }
  };

  const refreshAsset = () =>
    api.listAssets()
      .then((a) => setAssetName(a.assets.find((x) => x.id === a.current)?.name ?? ''))
      .catch(() => setAssetName(''));

  onMount(() => {
    api.getShotState().then(setS).catch(() => {});
    void refreshAsset();
    const offShot = api.listen('shot_state', (msg) => {
      const st = (msg as { state?: api.ShotState }).state;
      if (st) setS(st);
    });
    // Asset switch / rename broadcast sop_graph_changed / assets_changed.
    const offGraph = api.listen('sop_graph_changed', () => void refreshAsset());
    const offAssets = api.listen('assets_changed', () => void refreshAsset());
    onCleanup(() => { offShot(); offGraph(); offAssets(); });
  });

  const activeLayer = () => s().active ?? '';
  const deptLabel = () => LAYER_LABEL[activeLayer()] ?? activeLayer();
  // Suspended = shot open but paused while editing an asset → the bar reads as
  // ASSET mode (that's what the viewport shows), with a small "shot paused" chip.
  const shotActive = () => s().open && !s().suspended;
  // Which accent (CSS var) to apply: the active shot layer, or 'asset' otherwise.
  const accentKey = () => (shotActive() ? (activeLayer() || 'layout') : 'asset');
  const submode = () => SUBMODE[activeWorkspace()] ?? '';
  const selectedName = () => {
    const n = selectedActor()?.name ?? '';
    return n.includes('/') ? n.slice(n.lastIndexOf('/') + 1) : n;
  };

  return (
    <div
      class="shotbar"
      classList={{ 'shotbar--shot': shotActive(), [`shotbar--accent-${accentKey()}`]: true }}
    >
      <Show
        when={shotActive()}
        fallback={
          <>
            <span class="shotbar-mode shotbar-mode--asset">◆ Asset</span>
            <span class="shotbar-name">{assetName() || '—'}</span>
            <Show when={submode()}><span class="shotbar-dept">{submode()}</span></Show>
            <Show when={selectedName()}>
              <span class="shotbar-selected">· selected <b>{selectedName()}</b></span>
            </Show>
            <Show
              when={s().open}
              fallback={
                <span class="shotbar-hint">a static object — animate it by referencing it into a shot</span>
              }
            >
              <span class="shotbar-hint">
                ▣ shot “{s().name ?? 'shot'}” paused — pick a shot department (Layout/Animation/…) to resume
              </span>
            </Show>
            <span class="shotbar-spacer" />
            <Show when={!s().open}>
              <button
                type="button"
                class="shotbar-btn shotbar-btn--primary"
                disabled={busy()}
                title="Create a shot (layout / anim / lighting / render layers) and enter shot mode."
                onClick={() => void run(() => api.createShot())}
              >
                + New Shot
              </button>
              <button type="button" class="shotbar-btn" disabled={busy()} onClick={() => void run(() => api.openShot())}>
                Open Shot
              </button>
            </Show>
          </>
        }
      >
        <span class="shotbar-mode">▣ Shot</span>
        <span class="shotbar-name">{s().name ?? 'shot'}</span>
        <span class="shotbar-dept">{deptLabel()}</span>
        <span class="shotbar-arrow">→ edits go to <b>{activeLayer() || '?'}.usd</b></span>
        <Show when={selectedName()}>
          <span class="shotbar-selected">· selected <b>{selectedName()}</b></span>
        </Show>
        <span class="shotbar-spacer" />
        <div class="shotbar-stack" title="Department layers, strongest opinion first">
          <For each={s().departments}>
            {(d) => (
              <span class="shotbar-layer" classList={{ 'shotbar-layer--edit': activeLayer() === d }}>
                {d}
                <Show when={activeLayer() === d}><b class="shotbar-edit-tag">edit</b></Show>
              </span>
            )}
          </For>
        </div>
        <button type="button" class="shotbar-btn" disabled={busy()} title="Write all department layers to disk." onClick={() => void run(() => api.saveShot())}>
          Save Shot
        </button>
        <button type="button" class="shotbar-btn" disabled={busy()} title="Leave shot mode." onClick={() => void run(() => api.closeShot())}>
          Close
        </button>
      </Show>
    </div>
  );
}
