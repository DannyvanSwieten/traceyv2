import { Component, createSignal, onMount, onCleanup } from 'solid-js';
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
  let viewportRef: ViewportHandle | undefined;
  let unlistenImport: UnlistenFn | undefined;
  let unlistenExport: UnlistenFn | undefined;

  const loadScene = async (path: string) => {
    if (isLoading() || path === currentScene()) return;

    setIsLoading(true);
    setCurrentScene(path);
    setActors([]);
    setSelectedActorId(null);

    try {
      if (viewportRef) {
        await viewportRef.loadScene(path);
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
        addAsset(selected);
        await loadScene(selected);
      }
    } catch (error) {
      console.error('Failed to open file dialog:', error);
    }
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
      if (viewportRef) {
        viewportRef.render();
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
      if (viewportRef) {
        viewportRef.render();
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
  });

  onCleanup(() => {
    unlistenImport?.();
    unlistenExport?.();
  });

  return (
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
              ref={(ref) => (viewportRef = ref)}
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
          <RenderSettings onSettingsChange={() => viewportRef?.render()} />
        </div>
      </div>
    </div>
  );
};

export default App;
