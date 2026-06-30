import { Component, For, Show, createSignal, createEffect } from 'solid-js';
import * as api from '../../lib/api';
import './StartupProjects.css';

// Startup launcher: lists saved projects (standard location) so the user can open
// one, create a new one (name prompt → standard location), or open a project file
// elsewhere. Dismissable — "Skip" starts with no project (direct-by-default: the
// launcher is an offer, never a gate).
interface StartupProjectsProps {
  open: () => boolean;
  onOpenProject: (path: string) => void; // load an existing .tracey
  onNewProject: () => void; // create (name prompt → save → scaffold)
  onClose: () => void; // skip / start without a project
}

export const StartupProjects: Component<StartupProjectsProps> = (props) => {
  const [projects, setProjects] = createSignal<api.ProjectEntry[]>([]);
  const [loading, setLoading] = createSignal(false);

  // Hide the native Metal viewport layer while the launcher is up (it composites
  // ON TOP of the web content, so an HTML overlay would otherwise be occluded) —
  // same approach as ExportVideoDialog / CommandPalette.
  createEffect(() => {
    api.setViewportVisible(!props.open()).catch(() => {});
  });

  // Refresh the list each time the launcher opens.
  createEffect(() => {
    if (!props.open()) return;
    setLoading(true);
    api
      .listProjects()
      .then(setProjects)
      .catch(() => setProjects([]))
      .finally(() => setLoading(false));
  });

  const openOther = async () => {
    try {
      const sel = await api.openFileDialog('Open Project', [
        { description: 'Tracey Project', extensions: ['tracey', 'json'] },
      ]);
      if (sel) props.onOpenProject(sel);
    } catch {
      /* cancelled */
    }
  };

  return (
    <Show when={props.open()}>
      <div class="startup-overlay" onClick={() => props.onClose()}>
        <div class="startup-card" onClick={(e) => e.stopPropagation()}>
          <div class="startup-header">
            <h1 class="startup-title">Tracey</h1>
            <span class="startup-subtitle">Open a project</span>
          </div>

          <div class="startup-actions">
            <button type="button" class="startup-primary" onClick={() => props.onNewProject()}>
              + New Project
            </button>
            <button type="button" class="startup-secondary" onClick={() => void openOther()}>
              Open Other…
            </button>
          </div>

          <div class="startup-list">
            <Show
              when={projects().length > 0}
              fallback={
                <div class="startup-empty">
                  {loading() ? 'Scanning…' : 'No projects yet — create one to get started.'}
                </div>
              }
            >
              <For each={projects()}>
                {(p) => (
                  <button
                    type="button"
                    class="startup-project"
                    title={p.path}
                    onClick={() => props.onOpenProject(p.path)}
                  >
                    <span class="startup-project-icon">📁</span>
                    <span class="startup-project-name">{p.name}</span>
                    <span class="startup-project-dir">{p.dir}</span>
                  </button>
                )}
              </For>
            </Show>
          </div>

          <button type="button" class="startup-skip" onClick={() => props.onClose()}>
            Skip — start without a project
          </button>
        </div>
      </div>
    </Show>
  );
};
