// Compact node-add dropdown that mirrors SopNodePalette / VopNodePalette so
// the material graph editor has the same node-browsing affordance as the
// other graph editors (no full-width left-rail palette — the in-canvas tab
// menu + this toolbar select are enough).

import { Component, For, createSignal } from 'solid-js';
import { PALETTE } from '../../lib/material_graph';
import { addNode, setSelectedNode } from '../../stores/materials';

export const MaterialNodePalette: Component = () => {
  const [pending, setPending] = createSignal('');

  // <option value> encodes group:entry indices since labels aren't unique
  // and Constant/Parameter entries are factory-built (not keyed by an op
  // string we could look up later).
  function spawn(value: string) {
    if (!value) return;
    const [gi, ei] = value.split(':').map((x) => parseInt(x, 10));
    const entry = PALETTE[gi]?.entries[ei];
    if (!entry) return;
    const jitter = (): [number, number] => [
      40 + Math.random() * 80,
      40 + Math.random() * 80,
    ];
    const node = entry.factory(jitter());
    addNode(node);
    setSelectedNode(node.uid);
  }

  // No outer wrapper — the editor places this beside MaterialLibrary inside a
  // single shared `.sop-palette-toolbar` strip so the dropdowns and Save As
  // sit on one row.
  return (
    <select
      class="sop-palette-select"
      title="Add a material node"
      value={pending()}
      onChange={(e) => {
        const v = e.currentTarget.value;
        spawn(v);
        // Reset to placeholder so re-picking the same entry fires again.
        setPending('');
        e.currentTarget.value = '';
      }}
    >
      <option value="" disabled>Add Node…</option>
      <For each={PALETTE}>
        {(group, gi) => (
          <optgroup label={group.group}>
            <For each={group.entries}>
              {(entry, ei) => (
                <option value={`${gi()}:${ei()}`}>{entry.label}</option>
              )}
            </For>
          </optgroup>
        )}
      </For>
    </select>
  );
};
