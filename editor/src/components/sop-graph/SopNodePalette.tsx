// Compact node-add dropdown that sits in the SOP canvas toolbar (replacing
// the old left-rail palette). Native <select> with <optgroup>s for category
// grouping — cheap and matches the catalog's natural shape.

import { Component, For, createMemo, createSignal } from 'solid-js';
import { catalog, makeNode } from '../../lib/sop_graph';
import { addNode, setSelectedNode } from '../../stores/sops';

export const SopNodePalette: Component = () => {
  const [pending, setPending] = createSignal('');

  const groups = createMemo(() => {
    const byCategory = new Map<string, { kind: string; label: string }[]>();
    for (const e of catalog()) {
      if (!byCategory.has(e.category)) byCategory.set(e.category, []);
      byCategory.get(e.category)!.push({ kind: e.kind, label: e.label });
    }
    return Array.from(byCategory.entries());
  });

  function spawn(kind: string) {
    if (!kind) return;
    // Slight jitter so successive creations don't perfectly overlap; the
    // user drags from there.
    const jitter = (): [number, number] => [
      40 + Math.random() * 80,
      40 + Math.random() * 80,
    ];
    const node = makeNode(kind, jitter());
    if (!node) return;
    addNode(node);
    setSelectedNode(node.uid);
  }

  return (
    <div class="sop-palette-toolbar">
      <select
        class="sop-palette-select"
        title="Add a SOP node"
        value={pending()}
        onChange={(e) => {
          const v = e.currentTarget.value;
          spawn(v);
          // Reset back to the placeholder so the same kind can be picked
          // again immediately (an unchanged select fires no `change`).
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
