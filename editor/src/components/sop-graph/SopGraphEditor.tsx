// Docked graph editor — hosts both the SOP graph and (when drilled into) the
// VOP graph of an attribute_vop. The breadcrumb composes:
//   Root › Subnet › … › Subnet ›  attribute_vop      (last crumb appears
//                                  only when a VOP is open; clicking any
//                                  SOP crumb pops out of the VOP first.)
//
// Renders inline next to the viewport so geometry edits update the
// path-traced view in real time. The parent gates mount via the
// `sopEditorOpen || isVopEditorOpen` signal.

import { Component, For, Show, createMemo } from 'solid-js';
import { SopGraphPanel } from './SopGraphPanel';
import { AssetPicker } from './AssetPicker';
import {
  currentGraph,
  flushSopGraph,
  navigateTo,
  pathCrumbs,
} from '../../stores/sops';
import {
  closeVopEditor,
  currentHostUid,
  isVopEditorOpen,
} from '../../stores/vops';
import { VopGraphPanel } from '../vop-graph/VopGraphPanel';
import './SopGraphDock.css';
import './SopGraphBreadcrumb.css';

interface SopGraphEditorProps {
  onClose: () => void;
}

export const SopGraphEditor: Component<SopGraphEditorProps> = (props) => {
  // Label for the VOP crumb. attribute_vop has no `name` param so we lean on
  // the uid; the host is always findable in the currently-visible SOP graph
  // because that's where the user double-clicked to drill in.
  const vopCrumbLabel = createMemo<string | null>(() => {
    const host = currentHostUid();
    if (host === null) return null;
    const node = currentGraph().nodes.find((n) => n.uid === host);
    return node ? `vop#${host}` : `vop#${host}`;
  });

  // Close the entire dock. If a VOP is open, flush + reset that first so its
  // pending edits make it to the engine before the SOP push.
  const close = async () => {
    if (isVopEditorOpen()) await closeVopEditor();
    await flushSopGraph();
    props.onClose();
  };

  // Clicking a SOP crumb from inside a VOP exits the VOP first. The VOP only
  // lives at the leaf of the breadcrumb, so any SOP-level click means "go
  // back out of the VOP and then navigate within SOP".
  const onSopCrumbClick = async (depth: number) => {
    if (isVopEditorOpen()) await closeVopEditor();
    navigateTo(depth);
  };

  return (
    <div class="sop-graph-dock" role="region" aria-label="SOP Graph">
      <div class="sop-graph-dock-header">
        <span class="sop-graph-dock-title">
          {isVopEditorOpen() ? 'VOP Graph' : 'SOP Graph'}
        </span>
        <span class="sop-graph-dock-stats">
          {currentGraph().nodes.length} nodes · {currentGraph().connections.length} connections
        </span>
        <button class="sop-graph-dock-close" onClick={close} type="button">
          Close
        </button>
      </div>
      {/* Asset selector — which object's graph you're editing. Hidden inside a VOP
          drill-in (the breadcrumb owns that level). */}
      <Show when={!isVopEditorOpen()}>
        <AssetPicker />
      </Show>
      <nav class="sop-breadcrumb" aria-label="Subnet navigation">
        <button
          type="button"
          class="sop-breadcrumb-crumb"
          onClick={() => onSopCrumbClick(0)}
        >
          Root
        </button>
        <For each={pathCrumbs()}>
          {(crumb, i) => (
            <>
              <span class="sop-breadcrumb-sep">›</span>
              <button
                type="button"
                class="sop-breadcrumb-crumb"
                onClick={() => onSopCrumbClick(i() + 1)}
              >
                {crumb.name}
              </button>
            </>
          )}
        </For>
        <Show when={isVopEditorOpen() && vopCrumbLabel()}>
          <span class="sop-breadcrumb-sep">›</span>
          <button
            type="button"
            class="sop-breadcrumb-crumb"
            aria-current="page"
          >
            {vopCrumbLabel()}
          </button>
        </Show>
      </nav>
      <div class="sop-graph-dock-body">
        <Show when={isVopEditorOpen()} fallback={<SopGraphPanel />}>
          <VopGraphPanel />
        </Show>
      </div>
    </div>
  );
};
