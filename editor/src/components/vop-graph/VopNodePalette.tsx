// Compact node-add dropdown that sits in the VOP canvas toolbar. Mirrors
// SopNodePalette (native <select> + <optgroup>s) so both editors feel the
// same. Drops a freshly-allocated node at a slight jitter; the user drags
// from there.

import { Component, For, createMemo, createSignal } from 'solid-js';
import { catalog, makeNode } from '../../lib/vop_graph';
import { addNode, setSelectedNode } from '../../stores/vops';

export const VopNodePalette: Component = () => {
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
        title="Add a VOP node"
        value={pending()}
        onChange={(e) => {
          const v = e.currentTarget.value;
          spawn(v);
          // Reset to placeholder so the same kind can be picked again.
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
