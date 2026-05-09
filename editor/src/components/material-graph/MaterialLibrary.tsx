import { Component, For, createSignal, onMount, Show } from 'solid-js';
import * as api from '../../lib/api';
import { ShaderGraph } from '../../lib/material_graph';
import {
  materialGraph,
  replaceGraph,
  flushMaterialGraph,
  materialLibraryEntries,
  refreshMaterialLibrary,
} from '../../stores/materials';
import './MaterialLibrary.css';

// Per-user library of named graphs. Names are sanitized server-side; the UI
// just keeps the user honest with a quick client-side check before round-trip.
const SAFE_NAME = /^[A-Za-z0-9_\- ]{1,96}$/;

export const MaterialLibrary: Component = () => {
  const [error, setError] = createSignal<string | null>(null);
  const [busy, setBusy] = createSignal(false);

  // WKWebView blocks window.prompt/confirm by default (no WKUIDelegate
  // implemented for those panels), so we use inline UI instead.
  const [savePending, setSavePending] = createSignal(false);
  const [pendingName, setPendingName] = createSignal('');
  const [confirmDelete, setConfirmDelete] = createSignal<string | null>(null);

  let nameInputRef: HTMLInputElement | undefined;

  onMount(refreshMaterialLibrary);

  const beginSaveAs = () => {
    setError(null);
    setPendingName('');
    setSavePending(true);
    queueMicrotask(() => nameInputRef?.focus());
  };

  const cancelSaveAs = () => {
    setSavePending(false);
    setPendingName('');
  };

  const commitSaveAs = async () => {
    const name = pendingName().trim();
    if (!name) {
      cancelSaveAs();
      return;
    }
    if (!SAFE_NAME.test(name)) {
      setError('Name must be alphanumerics, spaces, _ or - (max 96 chars).');
      return;
    }
    setError(null);
    setBusy(true);
    try {
      // Flush any pending debounced edits so the on-disk copy matches the canvas.
      await flushMaterialGraph();
      await api.saveMaterialGraphAs(name, JSON.stringify(materialGraph()));
      await refreshMaterialLibrary();
      cancelSaveAs();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  const onSaveKey = (e: KeyboardEvent) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      commitSaveAs();
    } else if (e.key === 'Escape') {
      e.preventDefault();
      cancelSaveAs();
    }
  };

  const onLoad = async (name: string) => {
    setError(null);
    setBusy(true);
    try {
      const json = await api.loadMaterialGraphFromLibrary(name);
      const parsed = JSON.parse(json) as ShaderGraph;
      replaceGraph(parsed);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  const requestDelete = (name: string, e: MouseEvent) => {
    e.stopPropagation();
    setConfirmDelete(name);
  };

  const cancelDelete = () => setConfirmDelete(null);

  const commitDelete = async () => {
    const name = confirmDelete();
    if (!name) return;
    setBusy(true);
    try {
      await api.deleteMaterialGraphFromLibrary(name);
      await refreshMaterialLibrary();
      setConfirmDelete(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="material-library">
      <div class="material-library-header">
        <span class="material-library-title">Library</span>
        <button
          type="button"
          class="material-library-save"
          onClick={beginSaveAs}
          disabled={busy() || savePending()}
        >
          Save As…
        </button>
      </div>

      <Show when={savePending()}>
        <div class="material-library-input-row">
          <input
            ref={nameInputRef}
            class="material-library-input"
            type="text"
            placeholder="graph name"
            value={pendingName()}
            onInput={(e) => setPendingName(e.currentTarget.value)}
            onKeyDown={onSaveKey}
            disabled={busy()}
          />
          <button
            type="button"
            class="material-library-input-ok"
            onClick={commitSaveAs}
            disabled={busy() || !pendingName().trim()}
          >
            Save
          </button>
          <button
            type="button"
            class="material-library-input-cancel"
            onClick={cancelSaveAs}
            disabled={busy()}
          >
            Cancel
          </button>
        </div>
      </Show>

      <Show when={error()}>
        <div class="material-library-error">{error()}</div>
      </Show>

      <ul class="material-library-list">
        <For each={materialLibraryEntries()} fallback={<li class="material-library-empty">No saved graphs yet.</li>}>
          {(name) => (
            <li class="material-library-row">
              <Show
                when={confirmDelete() === name}
                fallback={
                  <>
                    <button
                      type="button"
                      class="material-library-name"
                      onClick={() => onLoad(name)}
                      disabled={busy()}
                      title="Load this graph"
                    >
                      {name}
                    </button>
                    <button
                      type="button"
                      class="material-library-delete"
                      onClick={(e) => requestDelete(name, e)}
                      disabled={busy()}
                      title="Delete from library"
                    >
                      ×
                    </button>
                  </>
                }
              >
                <span class="material-library-confirm-label">Delete "{name}"?</span>
                <button
                  type="button"
                  class="material-library-confirm-yes"
                  onClick={commitDelete}
                  disabled={busy()}
                >
                  Delete
                </button>
                <button
                  type="button"
                  class="material-library-confirm-no"
                  onClick={cancelDelete}
                  disabled={busy()}
                >
                  Cancel
                </button>
              </Show>
            </li>
          )}
        </For>
      </ul>
    </div>
  );
};
