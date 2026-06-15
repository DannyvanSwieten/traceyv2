// Workspace-specific quick-action strip rendered just below the workspace
// tabs. Adapts to the active workspace so each tab earns its real estate
// instead of being a glorified "hide-some-panels" preset.
//
// Wiring keeps the actual operations in App.tsx where the global signals
// (selected actor, sop graph, viewport ref) already live; this component
// is render-only.
//
// Render workspace settings (samples / bounces / resolution / preset
// buttons) deliberately do NOT live here — they're hosted in the dock
// slot by RenderPanel so the workspace reads as "viewport on the left,
// render controls on the right" and the toolbar strip stays compact.

import { Component, Show } from 'solid-js';
import { activeWorkspace } from '../../lib/workspaces';
import { autoKeyEnabled, toggleAutoKey } from '../../lib/auto_key';
import './WorkspaceBar.css';

interface AnimationProps {
  onSetKey: () => void;
}

export interface WorkspaceBarProps {
  animation: AnimationProps;
}

export const WorkspaceBar: Component<WorkspaceBarProps> = (props) => {
  return (
    <Show when={activeWorkspace() === 'animation'}>
      <div class="workspace-bar">
        <button
          type="button"
          class="workspace-bar-toggle"
          classList={{ 'workspace-bar-toggle--on': autoKeyEnabled() }}
          onClick={() => toggleAutoKey()}
          title="When on, parameter edits and G/R/S transform commits automatically key at the playhead. Same toggle as the playbar's AK."
        >
          <span class="workspace-bar-dot" classList={{ 'workspace-bar-dot--on': autoKeyEnabled() }} />
          Auto-Key
        </button>
        <button
          type="button"
          class="workspace-bar-button"
          onClick={props.animation.onSetKey}
          title="Key the selected actor's pose at the current playhead."
        >
          Set Key
          <span class="workspace-bar-hint">K</span>
        </button>
      </div>
    </Show>
  );
};
