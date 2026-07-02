import { Component, For, Show, createMemo, createSignal, onMount, onCleanup } from 'solid-js';
import * as api from '../../lib/api';
import { activeWorkspace, type WorkspaceName } from '../../lib/workspaces';
import './DepartmentBar.css';

// The top-of-window department bar — the film pipeline as navigation. Picking a
// department applies its tool layout (via the existing workspace presets) AND, when a
// shot is open, sets the USD edit target so edits author into that department's layer.
// Replaces the old workspace tabs + the shot department dropdown (one axis, not two).
// See the mock + docs/pipeline_layout.md.

interface SubMode {
  label: string;
  preset: WorkspaceName;
}
interface Dept {
  id: string;
  label: string;
  hue: string; // small wayfinding dot (pipeline color), not the active accent
  preset?: WorkspaceName; // direct preset (departments without sub-modes)
  shotLayer?: string; // USD layer this department authors into (shot mode)
  subModes?: SubMode[];
}

// Ordered left → right like a production: build → dress → animate → light → render.
const DEPARTMENTS: Dept[] = [
  {
    id: 'assets',
    label: 'Assets',
    hue: '#a78bfa',
    subModes: [
      { label: 'Model', preset: 'modeling' },
      { label: 'Look', preset: 'shading' },
      { label: 'Sim', preset: 'simulation' },
    ],
  },
  { id: 'layout', label: 'Layout', hue: '#5cc8ff', preset: 'layout', shotLayer: 'layout' },
  { id: 'animation', label: 'Animation', hue: '#34d399', preset: 'animation', shotLayer: 'anim' },
  { id: 'lighting', label: 'Lighting', hue: '#fbbf24', preset: 'lighting', shotLayer: 'lighting' },
  { id: 'rendering', label: 'Rendering', hue: '#f472b6', preset: 'render', shotLayer: 'render' },
];

// Derive the highlighted department/sub-mode from the shared workspace signal, so the
// bar stays in sync no matter who applied the workspace (bar, command palette, pie menu).
const PRESET_TO_DEPT: Record<WorkspaceName, { dept: string; sub?: string }> = {
  modeling: { dept: 'assets', sub: 'Model' },
  shading: { dept: 'assets', sub: 'Look' },
  simulation: { dept: 'assets', sub: 'Sim' },
  layout: { dept: 'layout' },
  animation: { dept: 'animation' },
  lighting: { dept: 'lighting' },
  render: { dept: 'rendering' },
};

interface Props {
  onApplyWorkspace: (name: WorkspaceName) => void;
}

export const DepartmentBar: Component<Props> = (props) => {
  const [shotOpen, setShotOpen] = createSignal(false);
  const [shotLayer, setShotLayer] = createSignal<string | null>(null); // active USD layer

  onMount(() => {
    api.getShotState().then((s) => { setShotOpen(s.open); setShotLayer(s.active); }).catch(() => {});
    const off = api.listen('shot_state', (msg) => {
      const s = (msg as { state?: api.ShotState }).state;
      if (s) { setShotOpen(s.open); setShotLayer(s.active); }
    });
    onCleanup(off);
  });

  const activeDept = createMemo(() => PRESET_TO_DEPT[activeWorkspace()]?.dept ?? 'assets');
  const activeSub = createMemo(() => PRESET_TO_DEPT[activeWorkspace()]?.sub ?? 'Model');

  const pick = (d: Dept, preset: WorkspaceName) => {
    props.onApplyWorkspace(preset); // tool layout (always)
    if (shotOpen() && d.shotLayer) {
      // Shot department: set the edit target — this also RESUMES a suspended shot
      // (the native side recomposes so the viewport flips back to the shot).
      api.setActiveDepartment(d.shotLayer).catch(() => {});
    } else if (shotOpen() && d.id === 'assets') {
      // Assets with a shot open: SUSPEND the shot — the viewport switches to the
      // current asset's cooked preview (one engine scene = one mode at a time;
      // without this the shot recompose wiped the asset preview on every tick).
      api.suspendShot().catch(() => {});
    }
  };

  return (
    <div class="dept-bar" role="tablist">
      <For each={DEPARTMENTS}>
        {(d) => (
          <div class="dept-group" classList={{ 'dept-group--active': activeDept() === d.id }}>
            <button
              type="button"
              class="dept-tab"
              role="tab"
              aria-selected={activeDept() === d.id}
              classList={{ 'dept-tab--active': activeDept() === d.id }}
              title={
                d.shotLayer
                  ? `${d.label} — edits author into ${d.shotLayer}.usd when a shot is open`
                  : d.label
              }
              onClick={() => pick(d, d.preset ?? d.subModes![0].preset)}
            >
              <span class="dept-hue" style={{ background: d.hue }} />
              {d.label}
              <Show when={shotOpen() && d.shotLayer && shotLayer() === d.shotLayer}>
                <span class="dept-editing" title="Active edit target">●</span>
              </Show>
            </button>
            <Show when={d.subModes}>
              <div class="dept-submodes" classList={{ 'dept-submodes--dim': activeDept() !== d.id }}>
                <For each={d.subModes}>
                  {(sm) => (
                    <button
                      type="button"
                      class="dept-submode"
                      classList={{ 'dept-submode--active': activeDept() === d.id && activeSub() === sm.label }}
                      onClick={() => pick(d, sm.preset)}
                    >
                      {sm.label}
                    </button>
                  )}
                </For>
              </div>
            </Show>
          </div>
        )}
      </For>
    </div>
  );
};
