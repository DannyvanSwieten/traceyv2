// Top-strip workspace tabs. Click a tab to apply that workspace's dock
// layout. Mirrors Blender's workspace-tabs strip — minimal styling, fits
// inline with the existing toolbar.

import { Component, For } from 'solid-js';
import {
  WORKSPACES,
  WORKSPACE_LABELS,
  activeWorkspace,
  type WorkspaceName,
} from '../../lib/workspaces';
import './WorkspaceTabs.css';

interface Props {
  onApply: (name: WorkspaceName) => void;
}

export const WorkspaceTabs: Component<Props> = (props) => {
  const names = Object.keys(WORKSPACES) as WorkspaceName[];
  return (
    <div class="workspace-tabs">
      <For each={names}>
        {(name) => (
          <button
            type="button"
            class="workspace-tab"
            classList={{ 'workspace-tab--active': activeWorkspace() === name }}
            onClick={() => props.onApply(name)}
          >
            {WORKSPACE_LABELS[name]}
          </button>
        )}
      </For>
    </div>
  );
};
