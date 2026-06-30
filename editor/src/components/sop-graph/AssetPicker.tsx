import { createSignal, onMount, onCleanup, For } from 'solid-js';
import * as api from '../../lib/api';
import { showToast } from '../../lib/toasts';
import './AssetPicker.css';

// Asset selector for the SOP dock (Assets / Model). Each asset is its own SOP graph
// ("one graph models one object"); switching swaps which graph the canvas edits and
// the viewport previews. Lives in the SOP dock header so it's only present while
// modeling. (Layout/shots reference PUBLISHED assets — that's R2.)
export function AssetPicker() {
  const [summary, setSummary] = createSignal<api.AssetSummary>({ assets: [], current: null });

  const refresh = () => api.listAssets().then(setSummary).catch(() => {});
  onMount(() => {
    refresh();
    // Re-list after a project load (and any external graph change) so the asset
    // registry restored from the .tracey shows up even if this picker was already
    // mounted — load_scene broadcasts sop_graph_changed once the registry is in.
    const off = api.listen('sop_graph_changed', refresh);
    onCleanup(off);
  });

  const onSwitch = async (id: string) => {
    try { setSummary(await api.switchAsset(id)); } catch (e) { console.error('[asset] switch', e); }
  };
  const onNew = async () => {
    const name = await api.promptText('New Asset', 'Asset name', 'Asset');
    if (!name || !name.trim()) return;
    try { setSummary(await api.createAsset(name.trim())); } catch (e) { console.error('[asset] new', e); }
  };
  const onRename = async () => {
    const cur = summary().current;
    if (!cur) return;
    const current = summary().assets.find((a) => a.id === cur)?.name ?? '';
    const name = await api.promptText('Rename Asset', 'New name', current);
    if (!name || !name.trim()) return;
    try { setSummary(await api.renameAsset(cur, name.trim())); } catch (e) { console.error('[asset] rename', e); }
  };
  const onDelete = async () => {
    const cur = summary().current;
    if (!cur) return;
    try { setSummary(await api.deleteAsset(cur)); } catch (e) { console.error('[asset] delete', e); }
  };
  const onPublish = async () => {
    try {
      const r = await api.publishAsset();
      showToast(`Published “${r.name}”`, { kind: 'success', detail: r.path });
    } catch (e) {
      showToast('Publish failed', { kind: 'error', detail: String(e) });
    }
  };

  return (
    <div class="asset-picker">
      <span class="asset-picker-label">Asset</span>
      <select
        class="asset-picker-select"
        value={summary().current ?? ''}
        title="The asset (object) you're modeling. Each asset is its own SOP graph."
        onChange={(e) => void onSwitch(e.currentTarget.value)}
      >
        <For each={summary().assets}>{(a) => <option value={a.id}>{a.name}</option>}</For>
      </select>
      <button class="asset-picker-btn asset-picker-btn--primary" type="button" title="Create a new asset" onClick={() => void onNew()}>
        + New
      </button>
      <button class="asset-picker-btn" type="button" title="Rename the current asset" onClick={() => void onRename()}>
        Rename
      </button>
      <button class="asset-picker-btn" type="button" title="Delete the current asset" onClick={() => void onDelete()}>
        Delete
      </button>
      <button
        class="asset-picker-btn asset-picker-btn--publish"
        type="button"
        title="Publish this asset to USD so it shows in the Assets tab and can be referenced into a shot"
        onClick={() => void onPublish()}
      >
        Publish
      </button>
    </div>
  );
}
