// Compact toolbar variant of the material library: a "Load saved…" dropdown
// and a "Save As…" button (with an inline name-input flow when active). No
// outer wrapper — the host editor places this beside MaterialNodePalette in
// a single shared toolbar strip.
//
// The previous full-pane variant carried per-row delete with confirm UI;
// that's not exposed here while we settle on the new layout. Re-add if
// missed — the underlying api.deleteMaterialGraphFromLibrary still exists.

import { Component, For, createSignal, onMount, Show } from 'solid-js';
import * as api from '../../lib/api';
import { ShaderGraph } from '../../lib/material_graph';
import {
  materialGraph,
  replaceGraph,
  flushMaterialGraph,
  materialLibraryEntries,
  refreshMaterialLibrary,
  materialCurrentName,
  setMaterialCurrentName,
} from '../../stores/materials';
import './MaterialLibrary.css';

const SAFE_NAME = /^[A-Za-z0-9_\- ]{1,96}$/;

export const MaterialLibrary: Component = () => {
  const [error, setError] = createSignal<string | null>(null);
  const [busy, setBusy] = createSignal(false);

  const [savePending, setSavePending] = createSignal(false);
  const [pendingName, setPendingName] = createSignal('');

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
    if (!name) { cancelSaveAs(); return; }
    if (!SAFE_NAME.test(name)) {
      setError('Name must be alphanumerics, spaces, _ or - (max 96 chars).');
      return;
    }
    setError(null);
    setBusy(true);
    try {
      await flushMaterialGraph();
      await api.saveMaterialGraphAs(name, JSON.stringify(materialGraph()));
      // From now on this is the "current" graph — Save (no-As) overwrites
      // this entry without re-prompting.
      setMaterialCurrentName(name);
      await refreshMaterialLibrary();
      cancelSaveAs();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  // Overwrite the current library entry in place. No prompt. Falls back to
  // the Save As flow when nothing's loaded yet (so the very first save still
  // collects a name).
  const saveCurrent = async () => {
    const name = materialCurrentName();
    if (!name) { beginSaveAs(); return; }
    setError(null);
    setBusy(true);
    try {
      await flushMaterialGraph();
      await api.saveMaterialGraphAs(name, JSON.stringify(materialGraph()));
      await refreshMaterialLibrary();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  };

  const onSaveKey = (e: KeyboardEvent) => {
    if (e.key === 'Enter') { e.preventDefault(); commitSaveAs(); }
    else if (e.key === 'Escape') { e.preventDefault(); cancelSaveAs(); }
  };

  const onLoadChange = async (e: Event) => {
    const select = e.currentTarget as HTMLSelectElement;
    const name = select.value;
    if (!name) return;
    setError(null);
    setBusy(true);
    try {
      const json = await api.loadMaterialGraphFromLibrary(name);
      const parsed = JSON.parse(json) as ShaderGraph;
      replaceGraph(parsed);
      setMaterialCurrentName(name);
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setBusy(false);
      // Reset to placeholder so the same item can be reloaded by re-picking it.
      select.value = '';
    }
  };

  return (
    <>
      <Show
        when={!savePending()}
        fallback={
          <>
            <input
              ref={nameInputRef}
              class="sop-palette-select"
              type="text"
              placeholder="graph name"
              value={pendingName()}
              onInput={(e) => setPendingName(e.currentTarget.value)}
              onKeyDown={onSaveKey}
              disabled={busy()}
              autocomplete="off"
              autocorrect="off"
              spellcheck={false}
            />
            <button
              type="button"
              class="sop-palette-select"
              onClick={commitSaveAs}
              disabled={busy() || !pendingName().trim()}
            >
              Save
            </button>
            <button
              type="button"
              class="sop-palette-select"
              onClick={cancelSaveAs}
              disabled={busy()}
            >
              Cancel
            </button>
          </>
        }
      >
        <select
          class="sop-palette-select"
          title="Load a saved material graph"
          value=""
          onChange={onLoadChange}
          disabled={busy() || materialLibraryEntries().length === 0}
        >
          <option value="" disabled>
            {materialLibraryEntries().length === 0 ? 'Library empty' : 'Load saved…'}
          </option>
          <For each={materialLibraryEntries()}>
            {(name) => <option value={name}>{name}</option>}
          </For>
        </select>
        {/* "Save" overwrites the current library entry in place. Disabled
            until either a graph has been loaded or saved-as at least once
            (otherwise there's no name to save to — Save As collects one). */}
        <button
          type="button"
          class="sop-palette-select"
          onClick={saveCurrent}
          disabled={busy() || !materialCurrentName()}
          title={
            materialCurrentName()
              ? `Save to "${materialCurrentName()}"`
              : 'No active library entry — use Save As to name one'
          }
        >
          Save
        </button>
        <button
          type="button"
          class="sop-palette-select"
          onClick={beginSaveAs}
          disabled={busy()}
        >
          Save As…
        </button>
      </Show>
      <Show when={error()}>
        <span class="material-library-error">{error()}</span>
      </Show>
    </>
  );
};
