import { Component, For } from 'solid-js';
import { PALETTE, PaletteEntry } from '../../lib/material_graph';
import { addNode } from '../../stores/materials';
import './NodePalette.css';

interface NodePaletteProps {
  // Where (in graph world coordinates) new nodes should drop. The host
  // typically passes the canvas centre, perturbed slightly per add so they
  // don't all stack at the same point.
  spawnPoint: () => [number, number];
}

export const NodePalette: Component<NodePaletteProps> = (props) => {
  let added = 0;
  const onAdd = (entry: PaletteEntry) => {
    const [cx, cy] = props.spawnPoint();
    // Stagger so successive adds don't pile up.
    const offset = (added++ % 6) * 24;
    addNode(entry.factory([cx + offset, cy + offset]));
  };
  return (
    <div class="node-palette">
      <For each={PALETTE}>
        {(group) => (
          <div class="node-palette-group">
            <div class="node-palette-group-label">{group.group}</div>
            <For each={group.entries}>
              {(entry) => (
                <button
                  class="node-palette-entry"
                  onClick={() => onAdd(entry)}
                  type="button"
                >
                  {entry.label}
                </button>
              )}
            </For>
          </div>
        )}
      </For>
    </div>
  );
};
