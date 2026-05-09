// Left rail of the SOP graph panel. Server-driven catalog: groups come from
// the C++ list_sop_node_catalog command. Click → drop a fresh node at a
// default position; the user drags it where they want.

import { Component, For, createMemo } from 'solid-js';
import { catalog, makeNode } from '../../lib/sop_graph';
import { addNode, setSelectedNode } from '../../stores/sops';

export const SopNodePalette: Component = () => {
  // Group entries by category, preserving the order they were registered in.
  const groups = createMemo(() => {
    const byCategory = new Map<string, { kind: string; label: string }[]>();
    for (const e of catalog()) {
      if (!byCategory.has(e.category)) byCategory.set(e.category, []);
      byCategory.get(e.category)!.push({ kind: e.kind, label: e.label });
    }
    return Array.from(byCategory.entries());
  });

  function spawn(kind: string) {
    // Drop new nodes near the origin with a small jitter so successive
    // creations don't perfectly overlap. The user can drag from there.
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
    <div class="sop-palette">
      <For each={groups()}>
        {([category, entries]) => (
          <div class="sop-palette-group">
            <h4 class="sop-palette-heading">{category}</h4>
            <For each={entries}>
              {(e) => (
                <button class="sop-palette-item" onClick={() => spawn(e.kind)}>
                  {e.label}
                </button>
              )}
            </For>
          </div>
        )}
      </For>
    </div>
  );
};
