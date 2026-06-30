// Workspace presets — curated dock layouts the user picks via a tab strip.
// Each preset is a named bundle of "which editor docks are open + which
// panel sizes". Storing the bundle here (not in App.tsx) keeps the
// applyWorkspace setter callable from commands / pie-menu wedges.

import { createSignal } from 'solid-js';

// Presets back the department bar: Assets sub-modes (modeling/shading/simulation),
// plus one per shot department (layout/animation/lighting/render).
export type WorkspaceName =
  | 'modeling'
  | 'shading'
  | 'simulation'
  | 'layout'
  | 'animation'
  | 'lighting'
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
  // Layout + Lighting have no dedicated graph dock yet — they work from the
  // viewport + outliner + Resources/properties (and set their USD edit target).
  // Layout is static set-dressing: nothing to keyframe, so the dopesheet collapses
  // (like render). The keyframe editor belongs to Animation, which opens it tall.
  layout:     { sopOpen: false, materialOpen: false, dopOpen: false, dopesheetH: 0   },
  animation:  { sopOpen: false, materialOpen: false, dopOpen: false, dopesheetH: 280 },
  lighting:   { sopOpen: false, materialOpen: false, dopOpen: false, dopesheetH: 200 },
  render:     { sopOpen: false, materialOpen: false, dopOpen: false, dopesheetH: 0   },
};

export const WORKSPACE_LABELS: Record<WorkspaceName, string> = {
  modeling: 'Modeling',
  shading: 'Shading',
  simulation: 'Simulation',
  layout: 'Layout',
  animation: 'Animation',
  lighting: 'Lighting',
  render: 'Render',
};

const [active, setActive] = createSignal<WorkspaceName>('modeling');
export const activeWorkspace = active;
export function setActiveWorkspaceInternal(n: WorkspaceName): void { setActive(n); }
