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
  // The asset/texture browser (left dock, below the outliner). Assets are
  // *managed* in the Assets departments and *referenced/dressed* in Layout, so
  // it shows there; in Animation/Lighting/Rendering it's just clutter — you're
  // working with objects already in the scene, not browsing the library.
  browserOpen: boolean;
  // Department-aware outliner filter. The outliner shows only what THIS department
  // works on: 'lights' (Lighting — your lights, not buried in geometry), 'geometry'
  // (Layout/Animation — the placed objects you dress/animate), or 'all' (Assets —
  // the whole asset). Keeps the tree focused on the task instead of the full scene.
  outliner: 'all' | 'geometry' | 'lights';
}

export const WORKSPACES: Record<WorkspaceName, WorkspaceLayout> = {
  modeling:   { sopOpen: true,  materialOpen: false, dopOpen: false, dopesheetH: 200, browserOpen: true,  outliner: 'all'      },
  shading:    { sopOpen: false, materialOpen: true,  dopOpen: false, dopesheetH: 200, browserOpen: true,  outliner: 'all'      },
  simulation: { sopOpen: false, materialOpen: false, dopOpen: true,  dopesheetH: 200, browserOpen: true,  outliner: 'all'      },
  // Layout + Lighting have no dedicated graph dock yet — they work from the
  // viewport + outliner + Resources/properties (and set their USD edit target).
  // Layout is static set-dressing: nothing to keyframe, so the dopesheet collapses
  // (like render). The keyframe editor belongs to Animation, which opens it tall.
  // Layout is where you reference assets → browser ON; the rest work on objects
  // already placed → browser OFF. Lighting's dopesheet collapses too — it's about
  // placing/tuning lights, not keyframing.
  layout:     { sopOpen: false, materialOpen: false, dopOpen: false, dopesheetH: 0,   browserOpen: true,  outliner: 'geometry' },
  animation:  { sopOpen: false, materialOpen: false, dopOpen: false, dopesheetH: 280, browserOpen: false, outliner: 'geometry' },
  lighting:   { sopOpen: false, materialOpen: false, dopOpen: false, dopesheetH: 0,   browserOpen: false, outliner: 'lights'   },
  render:     { sopOpen: false, materialOpen: false, dopOpen: false, dopesheetH: 0,   browserOpen: false, outliner: 'all'      },
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
