// Modal wrapper around SopGraphPanel — mirrors MaterialGraphEditor so the
// open/close lifecycle (hide viewport overlay on open, flush pending graph
// push on close) stays consistent across editors.

import { Component, createEffect, Show } from 'solid-js';
import { SopGraphPanel } from './SopGraphPanel';
import { flushSopGraph, sopGraph } from '../../stores/sops';
import * as api from '../../lib/api';
import '../material-graph/MaterialGraphEditor.css';

interface SopGraphEditorProps {
  open: () => boolean;
  onClose: () => void;
}

export const SopGraphEditor: Component<SopGraphEditorProps> = (props) => {
  // Hide the Metal viewport overlay while the modal is up so canvas clicks
  // don't get eaten. Restored when the modal closes.
  createEffect(() => {
    const isOpen = props.open();
    api.setViewportVisible(!isOpen).catch((e) => {
      console.warn('setViewportVisible failed:', e);
    });
  });

  const close = async () => {
    await flushSopGraph();
    props.onClose();
  };

  return (
    <Show when={props.open()}>
      <div class="material-graph-modal" role="dialog" aria-modal="true">
        <div class="material-graph-toolbar">
          <span class="material-graph-title">SOP Graph</span>
          <span class="material-graph-stats">
            {sopGraph().nodes.length} nodes · {sopGraph().connections.length} connections
          </span>
          <button class="material-graph-close" onClick={close} type="button">
            Close
          </button>
        </div>
        <div class="material-graph-body">
          <SopGraphPanel />
        </div>
      </div>
    </Show>
  );
};
