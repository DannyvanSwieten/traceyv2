import { Component, createEffect, onMount, createSignal, Show } from 'solid-js';
import { GraphCanvas } from './GraphCanvas';
import { NodePalette } from './NodePalette';
import { NodeInspector } from './NodeInspector';
import { MaterialLibrary } from './MaterialLibrary';
import { loadMaterialGraphFromEngine, flushMaterialGraph, materialGraph } from '../../stores/materials';
import * as api from '../../lib/api';
import './MaterialGraphEditor.css';

interface MaterialGraphEditorProps {
  open: () => boolean;
  onClose: () => void;
}

export const MaterialGraphEditor: Component<MaterialGraphEditorProps> = (props) => {
  const [loaded, setLoaded] = createSignal(false);

  // Load the active graph once when first opened.
  onMount(async () => {
    if (!loaded()) {
      await loadMaterialGraphFromEngine();
      setLoaded(true);
    }
  });

  // The native Metal viewport overlay sits on top of the WebView and would
  // otherwise swallow clicks in the canvas region. Hide it while the modal is
  // open and restore it on close.
  createEffect(() => {
    const isOpen = props.open();
    api.setViewportVisible(!isOpen).catch((e) => {
      console.warn('setViewportVisible failed:', e);
    });
  });

  const close = async () => {
    await flushMaterialGraph();
    props.onClose();
  };

  // New nodes spawn near the centre of the visible canvas. We don't have
  // direct access to canvas pan/zoom from here, so use a fixed world-space
  // anchor; in practice users pan/drag right after.
  const spawnPoint = (): [number, number] => [240, 200];

  return (
    <Show when={props.open()}>
      <div class="material-graph-modal" role="dialog" aria-modal="true">
        <div class="material-graph-toolbar">
          <span class="material-graph-title">Material Graph</span>
          <span class="material-graph-stats">
            {materialGraph().nodes.length} nodes · {materialGraph().connections.length} connections
          </span>
          <button class="material-graph-close" onClick={close} type="button">
            Close
          </button>
        </div>
        <div class="material-graph-body">
          <div class="material-graph-left-pane">
            <MaterialLibrary />
            <NodePalette spawnPoint={spawnPoint} />
          </div>
          <div class="material-graph-canvas-host">
            <GraphCanvas />
          </div>
          <NodeInspector />
        </div>
      </div>
    </Show>
  );
};
