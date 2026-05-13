// Generic palette dropdown — the toolbar widget that lets the user pick
// a node kind and drop a fresh instance onto the canvas. Identical UI
// across SOP / VOP / DOP; the only thing that differs is which catalog
// to enumerate and which `addNode` to call.
//
// Callers supply: a reactive catalog accessor (returns the {kind, label,
// category} entries) and an `onSpawn(kind)` callback that allocates the
// node and adds it. The dropdown itself is dumb — categories come from
// each entry's `category` string, no extra config needed.

import { Component, For, createMemo, createSignal } from 'solid-js';

export interface PaletteCatalogEntry {
  kind: string;
  label: string;
  category: string;
}

export interface NodePaletteProps {
  catalog: () => PaletteCatalogEntry[];
  onSpawn: (kind: string) => void;
  // Plain-text tooltip; defaults to "Add a node". Each graph editor
  // typically passes "Add a SOP node" / "Add a VOP node" / etc.
  title?: string;
}

export const NodePalette: Component<NodePaletteProps> = (props) => {
  const [pending, setPending] = createSignal('');

  const groups = createMemo(() => {
    const byCategory = new Map<string, { kind: string; label: string }[]>();
    for (const e of props.catalog()) {
      if (!byCategory.has(e.category)) byCategory.set(e.category, []);
      byCategory.get(e.category)!.push({ kind: e.kind, label: e.label });
    }
    return Array.from(byCategory.entries());
  });

  return (
    <div class="sop-palette-toolbar">
      <select
        class="sop-palette-select"
        title={props.title ?? 'Add a node'}
        value={pending()}
        onChange={(e) => {
          const v = e.currentTarget.value;
          if (v) props.onSpawn(v);
          // Reset the value so the same kind can be picked again.
          setPending('');
          e.currentTarget.value = '';
        }}
      >
        <option value="" disabled>Add Node…</option>
        <For each={groups()}>
          {([category, entries]) => (
            <optgroup label={category}>
              <For each={entries}>
                {(en) => <option value={en.kind}>{en.label}</option>}
              </For>
            </optgroup>
          )}
        </For>
      </select>
    </div>
  );
};
