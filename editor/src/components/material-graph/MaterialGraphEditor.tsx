// Docked material-graph editor. Mirrors the SOP dock's shell so the two
// editors share screen real-estate via a single dock slot in App.tsx —
// the user picks which one is live via toolbar / tab strip, never both
// at once. Previously a full-screen modal; the modal version hid the
// viewport via setViewportVisible, but a docked panel sits next to it
// so the viewport stays live and material edits show their effect
// immediately on the path-traced view.

import { Component, createEffect, onMount, createSignal, Show } from 'solid-js';
import { MaterialGraphCanvas } from './MaterialGraphCanvas';
import { MaterialNodePalette } from './MaterialNodePalette';
import { NodeInspector } from './NodeInspector';
import { MaterialLibrary } from './MaterialLibrary';
import { loadMaterialGraphFromEngine, flushMaterialGraph, materialGraph } from '../../stores/materials';
import './MaterialGraphEditor.css';
// Pull in the .sop-palette-toolbar / .sop-palette-select rules used by
// MaterialNodePalette. Shared with the SOP/VOP editors so all three node
// pickers look identical.
import '../sop-graph/SopGraphPanel.css';

interface MaterialGraphEditorProps {
  open: () => boolean;
  onClose: () => void;
}

export const MaterialGraphEditor: Component<MaterialGraphEditorProps> = (props) => {
  const [loaded, setLoaded] = createSignal(false);

  // Load the active graph the first time the dock opens, then keep it
  // in sync with the engine for the rest of the session.
  createEffect(async () => {
    if (props.open() && !loaded()) {
      await loadMaterialGraphFromEngine();
      setLoaded(true);
    }
  });

  // Eager load: even before the dock is opened the user may have other
  // UI (e.g. an actor-properties Materials picker) that benefits from a
  // populated store. Cheap one-shot at mount.
  onMount(async () => {
    if (!loaded()) {
      try {
        await loadMaterialGraphFromEngine();
        setLoaded(true);
      } catch {
        /* engine may not be ready yet — recovers on first dock open */
      }
    }
  });

  const close = async () => {
    await flushMaterialGraph();
    props.onClose();
  };

  return (
    <Show when={props.open()}>
      <div class="material-graph-dock" role="region" aria-label="Material Graph">
        <div class="material-graph-dock-header">
          <span class="material-graph-dock-title">Material Graph</span>
          <span class="material-graph-dock-stats">
            {materialGraph().nodes.length} nodes · {materialGraph().connections.length} connections
          </span>
          <button class="material-graph-dock-close" onClick={close} type="button">
            Close
          </button>
        </div>
        {/* Inspector on top, canvas+toolbar below — mirrors SopGraphPanel /
            VopGraphPanel so the three editors all read the same. */}
        <div class="material-graph-dock-body">
          <div class="material-graph-inspector-row">
            <NodeInspector />
          </div>
          <div class="material-graph-canvas-col">
            <div class="sop-palette-toolbar">
              <MaterialNodePalette />
              <MaterialLibrary />
            </div>
            <MaterialGraphCanvas />
          </div>
        </div>
      </div>
    </Show>
  );
};
