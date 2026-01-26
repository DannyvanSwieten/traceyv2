import { Component, createSignal, onMount, For, Show } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import { open } from '@tauri-apps/plugin-dialog';
import './ProjectManager.css';

interface RecentProject {
  name: string;
  path: string;
  last_opened: string;
  thumbnail: string | null;
}

interface ProjectManagerProps {
  onProjectOpened: () => void;
  onSkip?: () => void;
}

export const ProjectManager: Component<ProjectManagerProps> = (props) => {
  const [projectName, setProjectName] = createSignal('');
  const [recentProjects, setRecentProjects] = createSignal<RecentProject[]>([]);
  const [loading, setLoading] = createSignal(false);
  const [error, setError] = createSignal<string | null>(null);

  onMount(async () => {
    await loadRecentProjects();
  });

  const loadRecentProjects = async () => {
    try {
      const recent = await invoke<RecentProject[]>('get_recent_projects');
      setRecentProjects(recent);
    } catch (e) {
      console.error('Failed to load recent projects:', e);
    }
  };

  const handleNewProject = async () => {
    const name = projectName().trim();
    if (!name) {
      setError('Please enter a project name');
      return;
    }

    try {
      setLoading(true);
      setError(null);

      // Ask user to select parent directory
      const directory = await open({
        directory: true,
        title: 'Select Project Location',
      });

      if (!directory) {
        setLoading(false);
        return;
      }

      // Create new project
      const projectPath = await invoke<string>('project_new', {
        name,
        directory,
      });

      console.log('Created project:', projectPath);

      // Reload recent projects
      await loadRecentProjects();

      // Notify parent that project is opened
      props.onProjectOpened();
    } catch (e) {
      setError(`Failed to create project: ${e}`);
      console.error('Project creation failed:', e);
    } finally {
      setLoading(false);
    }
  };

  const handleOpenProject = async () => {
    try {
      setLoading(true);
      setError(null);

      const file = await open({
        filters: [
          {
            name: 'Tracey Project',
            extensions: ['tracey'],
          },
        ],
        title: 'Open Project',
      });

      if (!file || typeof file !== 'string') {
        setLoading(false);
        return;
      }

      await invoke('project_open', { projectPath: file });

      // Reload recent projects
      await loadRecentProjects();

      // Notify parent that project is opened
      props.onProjectOpened();
    } catch (e) {
      setError(`Failed to open project: ${e}`);
      console.error('Project open failed:', e);
    } finally {
      setLoading(false);
    }
  };

  const handleOpenRecent = async (path: string) => {
    try {
      setLoading(true);
      setError(null);

      await invoke('project_open', { projectPath: path });

      // Reload recent projects
      await loadRecentProjects();

      // Notify parent that project is opened
      props.onProjectOpened();
    } catch (e) {
      setError(`Failed to open project: ${e}`);
      console.error('Project open failed:', e);
    } finally {
      setLoading(false);
    }
  };

  const handleRemoveRecent = async (e: MouseEvent, path: string) => {
    e.stopPropagation();

    try {
      await invoke('remove_recent_project', { projectPath: path });
      await loadRecentProjects();
    } catch (e) {
      console.error('Failed to remove recent project:', e);
    }
  };

  const formatDate = (dateStr: string): string => {
    try {
      const date = new Date(dateStr);
      return date.toLocaleDateString();
    } catch {
      return 'Unknown';
    }
  };

  return (
    <div class="project-manager">
      <div class="project-manager-header">
        <h1>Welcome to Tracey</h1>
        <p>Create or open a project to get started</p>
      </div>

      <Show when={error()}>
        <div class="error-message">{error()}</div>
      </Show>

      <div class="project-actions">
        <div class="new-project-section">
          <h2>New Project</h2>
          <div class="new-project-form">
            <input
              type="text"
              placeholder="Project name"
              value={projectName()}
              onInput={(e) => setProjectName(e.currentTarget.value)}
              disabled={loading()}
              onKeyPress={(e) => {
                if (e.key === 'Enter') handleNewProject();
              }}
            />
            <button onClick={handleNewProject} disabled={loading() || !projectName().trim()}>
              Create Project
            </button>
          </div>
        </div>

        <div class="open-project-section">
          <h2>Open Existing Project</h2>
          <button onClick={handleOpenProject} disabled={loading()}>
            Browse for Project...
          </button>
        </div>
      </div>

      <Show when={props.onSkip}>
        <div class="skip-section">
          <button class="skip-button" onClick={props.onSkip}>
            Continue without a project
          </button>
        </div>
      </Show>

      <div class="recent-projects-section">
        <h2>Recent Projects</h2>
        <Show
          when={recentProjects().length > 0}
          fallback={<p class="no-recent">No recent projects</p>}
        >
          <div class="recent-projects-list">
            <For each={recentProjects()}>
              {(project) => (
                <div
                  class="recent-project-item"
                  onClick={() => handleOpenRecent(project.path)}
                >
                  <div class="project-info">
                    <div class="project-name">{project.name}</div>
                    <div class="project-path">{project.path}</div>
                    <div class="project-date">{formatDate(project.last_opened)}</div>
                  </div>
                  <button
                    class="remove-btn"
                    onClick={(e) => handleRemoveRecent(e, project.path)}
                    title="Remove from recent"
                  >
                    ×
                  </button>
                </div>
              )}
            </For>
          </div>
        </Show>
      </div>

      <Show when={loading()}>
        <div class="loading-overlay">
          <div class="loading-spinner"></div>
        </div>
      </Show>
    </div>
  );
};
