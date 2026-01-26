import { Component, createSignal, onMount, onCleanup, Show } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import { listen, UnlistenFn } from '@tauri-apps/api/event';
import { open } from '@tauri-apps/plugin-dialog';
import { Viewport, ViewportHandle, CameraPosition } from './components/viewport/Viewport';
import {
  SceneHierarchy,
  Actor,
} from './components/scene-hierarchy/SceneHierarchy';
import { ResourcesBrowser } from './components/resources-browser/ResourcesBrowser';
import { RenderSettings } from './components/render-settings/RenderSettings';
import { CameraControls } from './components/camera-controls/CameraControls';
import { ActorProperties, Transform } from './components/actor-properties/ActorProperties';
import { AddObjectMenu } from './components/add-object-menu/AddObjectMenu';
import { ProjectManager } from './components/project-manager/ProjectManager';
import { getAssets, addAsset, removeAsset } from './stores/assets';
import './App.css';

const App: Component = () => {
  const [currentScene, setCurrentScene] = createSignal<string | null>(null);
  const [isLoading, setIsLoading] = createSignal(false);
  const [actors, setActors] = createSignal<Actor[]>([]);
  const [selectedActorId, setSelectedActorId] = createSignal<number | null>(
    null
  );
  const [cameraPosition, setCameraPosition] = createSignal<CameraPosition>({
    x: 0,
    y: 0,
    z: 3,
  });
  const [projectOpen, setProjectOpen] = createSignal(false);
  const [viewportRef, setViewportRef] = createSignal<ViewportHandle | undefined>();
  let unlistenImport: UnlistenFn | undefined;
  let unlistenExport: UnlistenFn | undefined;
  let shaderCheckInterval: number | undefined;

  const loadScene = async (path: string) => {
    if (isLoading() || path === currentScene()) return;

    setIsLoading(true);
    setCurrentScene(path);
    setActors([]);
    setSelectedActorId(null);

    try {
      const viewport = viewportRef();
      if (viewport) {
        await viewport.loadScene(path);
      }

      const loadedActors = await invoke<Actor[]>('get_all_actors');
      setActors(loadedActors);
    } catch (error) {
      console.error('Failed to load scene:', error);
    } finally {
      setIsLoading(false);
    }
  };

  const handleImport = async () => {
    try {
      const selected = await open({
        multiple: false,
        filters: [
          {
            name: 'glTF',
            extensions: ['gltf', 'glb'],
          },
        ],
      });

      if (selected && typeof selected === 'string') {
        // Just load directly - don't copy to project for now
        addAsset(selected);
        await loadScene(selected);
      }
    } catch (error) {
      console.error('Failed to import asset:', error);
    }
  };

  const handleProjectOpened = async () => {
    setProjectOpen(true);

    // Load scene data from project
    try {
      const loadedActors = await invoke<Actor[]>('get_all_actors');
      setActors(loadedActors);

      // Compile scene
      await invoke('compile_scene');

      const viewport = viewportRef();
      if (viewport) {
        viewport.render();
      }
    } catch (error) {
      console.error('Failed to load project scene:', error);
    }
  };

  const handleSkipProject = () => {
    // Continue without opening a project (legacy mode)
    setProjectOpen(true);
  };

  const handleTransformChange = async (actorId: number, transform: Transform) => {
    // Update local actors state
    setActors((prev) =>
      prev.map((actor) =>
        actor.id === actorId ? { ...actor, transform } : actor
      )
    );

    // Recompile scene and re-render
    try {
      await invoke('compile_scene');
      const viewport = viewportRef();
      if (viewport) {
        viewport.render();
      }
    } catch (error) {
      console.error('Failed to recompile scene after transform change:', error);
    }
  };

  const handleObjectAdded = async (actor: Actor) => {
    // Add to local actors state
    setActors((prev) => [...prev, actor]);

    // Select the new actor
    setSelectedActorId(actor.id);

    // Compile scene and render
    try {
      await invoke('compile_scene');
      const viewport = viewportRef();
      if (viewport) {
        viewport.render();
      }
    } catch (error) {
      console.error('Failed to compile scene after adding object:', error);
    }
  };

  onMount(async () => {
    console.log('Setting up menu event listeners...');
    unlistenImport = await listen('menu-import', () => {
      console.log('menu-import event received!');
      handleImport();
    });
    unlistenExport = await listen('menu-export', () => {
      console.log('menu-export event received!');
    });
    console.log('Menu event listeners set up');

    // Start shader hot reload polling (every 2 seconds)
    shaderCheckInterval = window.setInterval(async () => {
      if (!projectOpen()) return;

      try {
        const modified = await invoke<string[]>('project_check_shaders');
        if (modified.length > 0) {
          console.log('Shaders modified, pipeline rebuilt:', modified);

          // Re-compile scene with new shaders
          await invoke('compile_scene');

          const viewport = viewportRef();
          if (viewport) {
            viewport.render();
          }
        }
      } catch (error) {
        // Silently ignore errors (project might not be open)
      }
    }, 2000);
  });

  onCleanup(() => {
    unlistenImport?.();
    unlistenExport?.();
    if (shaderCheckInterval !== undefined) {
      window.clearInterval(shaderCheckInterval);
    }
  });

  return (
    <Show
      when={projectOpen()}
      fallback={<ProjectManager onProjectOpened={handleProjectOpened} onSkip={handleSkipProject} />}
    >
      <div class="app">
        <div class="toolbar">
          <h1>Tracey Editor</h1>
          <AddObjectMenu onObjectAdded={handleObjectAdded} />
        </div>

        <div class="main-content">
          <div class="left-panel panel">
            <h3>Scene Hierarchy</h3>
            <SceneHierarchy
              actors={actors}
              selectedActorId={selectedActorId}
              onActorSelect={setSelectedActorId}
              isLoading={isLoading}
            />
          </div>

          <div class="center-area">
            <div class="viewport-container">
              <Viewport
                ref={setViewportRef}
                cameraPosition={cameraPosition}
                onCameraPositionChange={setCameraPosition}
              />
            </div>
            <ResourcesBrowser
              assets={getAssets()}
              currentAssetPath={currentScene}
              onAssetSelect={(asset) => loadScene(asset.path)}
              onAssetRemove={removeAsset}
            />
          </div>

          <div class="right-panel panel">
            <h3>Properties</h3>
            <ActorProperties
              selectedActorId={selectedActorId}
              actors={actors}
              onTransformChange={handleTransformChange}
            />
            <CameraControls
              position={cameraPosition}
              onPositionChange={setCameraPosition}
            />
            <RenderSettings
              onSettingsChange={() => {
                const viewport = viewportRef();
                if (viewport) viewport.render();
              }}
              viewportHandle={viewportRef()}
            />
          </div>
        </div>
      </div>
    </Show>
  );
};

export default App;
