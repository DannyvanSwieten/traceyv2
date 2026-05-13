// Workspace presets — curated dock layouts the user picks via a tab strip.
// Each preset is a named bundle of "which editor docks are open + which
// panel sizes". Storing the bundle here (not in App.tsx) keeps the
// applyWorkspace setter callable from commands / pie-menu wedges.

import { createSignal } from 'solid-js';

export type WorkspaceName =
  | 'modeling'
  | 'shading'
  | 'simulation'
  | 'animation'
  | 'render';

export interface WorkspaceLayout {
  sopOpen: boolean;
  materialOpen: boolean;
  dopOpen: boolean;
  // Dopesheet height is squashed to 0 in 'render' mode so the viewport
  // grabs the full vertical space.
  dopesheetH: number;
}

export const WORKSPACES: Record<WorkspaceName, WorkspaceLayout> = {
  modeling:   { sopOpen: true,  materialOpen: false, dopOpen: false, dopesheetH: 200 },
  shading:    { sopOpen: false, materialOpen: true,  dopOpen: false, dopesheetH: 200 },
  simulation: { sopOpen: false, materialOpen: false, dopOpen: true,  dopesheetH: 200 },
  animation:  { sopOpen: false, materialOpen: false, dopOpen: false, dopesheetH: 280 },
  render:     { sopOpen: false, materialOpen: false, dopOpen: false, dopesheetH: 0   },
};

export const WORKSPACE_LABELS: Record<WorkspaceName, string> = {
  modeling: 'Modeling',
  shading: 'Shading',
  simulation: 'Simulation',
  animation: 'Animation',
  render: 'Render',
};

const [active, setActive] = createSignal<WorkspaceName>('modeling');
export const activeWorkspace = active;
export function setActiveWorkspaceInternal(n: WorkspaceName): void { setActive(n); }
