import { Component, createSignal, onMount, onCleanup, Show } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import { listen, UnlistenFn } from '@tauri-apps/api/event';
import { open, save } from '@tauri-apps/plugin-dialog';
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
import { NodeGraph, NodeGraphHandle } from './components/node-graph/NodeGraph';
import { ResizeHandle } from './components/resize-handle/ResizeHandle';
import { getAssets, addAsset, removeAsset } from './stores/assets';
import './App.css';

const App: Component = () => {
  const [currentScene, setCurrentScene] = createSignal<string | null>(null);
  const [isLoading, setIsLoading] = createSignal(false);
  const [actors, setActors] = createSignal<Actor[]>([]);
  const [nodeGraphHandle, setNodeGraphHandle] = createSignal<NodeGraphHandle | undefined>();
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
  // Panel sizes (resizable)
  const [leftPanelWidth, setLeftPanelWidth] = createSignal(600);
  const [middlePanelWidth, setMiddlePanelWidth] = createSignal(400);
  const [rightPanelWidth, setRightPanelWidth] = createSignal(300);
  const [hierarchyHeight, setHierarchyHeight] = createSignal(300);
  const [bottomPanelHeight, setBottomPanelHeight] = createSignal(150);

  let unlistenImport: UnlistenFn | undefined;
  let unlistenExport: UnlistenFn | undefined;
  let unlistenSave: UnlistenFn | undefined;
  let unlistenSaveAs: UnlistenFn | undefined;
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
        // Add to assets list - user can drag it into the scene when ready
        addAsset(selected);
      }
    } catch (error) {
      console.error('Failed to import asset:', error);
    }
  };

  const handleSave = async () => {
    try {
      await invoke('project_save');
      console.log('Project saved successfully');
    } catch (error) {
      console.error('Failed to save project:', error);
    }
  };

  const handleSaveAs = async () => {
    try {
      const selected = await save({
        filters: [
          {
            name: 'Tracey Scene',
            extensions: ['json'],
          },
        ],
      });

      if (selected && typeof selected === 'string') {
        await invoke('save_scene', { path: selected });
        console.log('Scene saved to:', selected);
      }
    } catch (error) {
      console.error('Failed to save scene:', error);
    }
  };

  const handleProjectOpened = async () => {
    setProjectOpen(true);

    // Load scene data from project
    try {
      const loadedActors = await invoke<Actor[]>('get_all_actors');
      setActors(loadedActors);

      // Compile scene (empty scenes are now supported and render sky only)
      await invoke('compile_scene');

      // Mark scene as loaded and render
      const viewport = viewportRef();
      if (viewport) {
        viewport.markSceneLoaded();
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
    // Use compile_scene_no_sync to preserve primitives added directly to C++ scene
    try {
      await invoke('compile_scene_no_sync');
      const viewport = viewportRef();
      if (viewport) {
        viewport.render();
      }
    } catch (error) {
      console.error('Failed to recompile scene after transform change:', error);
    }
  };

  const handleObjectAdded = async (actor: Actor) => {
    // Refresh all actors from backend to get updated parent-child relationships
    // (the root's children list needs to include the new actor)
    try {
      const updated = await invoke<Actor[]>('get_all_actors');
      setActors(updated);
    } catch (error) {
      // Fallback to just appending if refresh fails
      setActors((prev) => [...prev, actor]);
    }

    // Select the new actor
    setSelectedActorId(actor.id);

    // Compile scene and render
    // Use compile_scene_no_sync because the primitive was already added to the C++ scene
    try {
      await invoke('compile_scene_no_sync');
      const viewport = viewportRef();
      if (viewport) {
        // Mark scene as loaded so render will work (needed for first object in empty scene)
        viewport.markSceneLoaded();
        viewport.render();
      }

      // Refresh the NodeGraph to show the new actor
      const handle = nodeGraphHandle();
      if (handle) {
        await handle.refresh();
      }
    } catch (error) {
      console.error('Failed to compile scene after adding object:', error);
    }
  };

  const handleAssetDropped = async (assetPath: string) => {
    try {
      // Add the asset to the scene (without clearing existing actors)
      await invoke('add_gltf_to_scene', { path: assetPath });

      // Get all actors after import
      const loadedActors = await invoke<Actor[]>('get_all_actors');
      setActors(loadedActors);

      // Compile and render (use compile_scene_no_sync to preserve C++ geometry)
      await invoke('compile_scene_no_sync');
      const viewport = viewportRef();
      console.log('handleAssetDropped: viewport =', viewport, 'calling render...');
      if (viewport) {
        viewport.markSceneLoaded();
        console.log('handleAssetDropped: calling viewport.render()');
        viewport.render();
      } else {
        console.error('handleAssetDropped: viewport is undefined!');
      }

      // Refresh the NodeGraph to show the new actors
      const handle = nodeGraphHandle();
      if (handle) {
        await handle.refresh();
      }
    } catch (error) {
      console.error('Failed to load asset:', error);
    }
  };

  const handleActorRemove = async (actorId: number) => {
    try {
      console.log('handleActorRemove: Removing actor', actorId);

      // Remove from backend (this removes from both Rust and C++ scenes and recompiles)
      await invoke('delete_actor', { actorId });
      console.log('handleActorRemove: Actor removed from backend');

      // Reload actor list from backend to reflect all changes (including removed children)
      const loadedActors = await invoke<Actor[]>('get_all_actors');
      setActors(loadedActors);
      console.log('handleActorRemove: Actors reloaded, count:', loadedActors.length);

      // Deselect if this actor was selected
      if (selectedActorId() === actorId) {
        setSelectedActorId(null);
      }

      // Render with the updated scene
      const viewport = viewportRef();
      console.log('handleActorRemove: viewport =', viewport);
      if (viewport) {
        console.log('handleActorRemove: Calling viewport.render()');
        viewport.render();
      } else {
        console.error('handleActorRemove: viewport is undefined!');
      }

      // Refresh the NodeGraph to show the updated context
      const handle = nodeGraphHandle();
      if (handle) {
        await handle.refresh();
      }
    } catch (error) {
      console.error('Failed to remove actor:', error);
    }
  };

  const handleActorReorder = async (
    actorId: number,
    parentId: number | null,
    newIndex: number
  ) => {
    try {
      // Call backend to reorder
      await invoke('reorder_child', {
        parentId,
        childId: actorId,
        newIndex,
      });

      // Refresh actors to get updated order
      const updated = await invoke<Actor[]>('get_all_actors');
      setActors(updated);
    } catch (error) {
      console.error('Failed to reorder actor:', error);
    }
  };

  const handleSetParent = async (actorId: number, newParentId: number | null) => {
    try {
      // Call backend to set parent
      await invoke('set_actor_parent', {
        actorId,
        parentId: newParentId,
      });

      // Refresh actors to get updated hierarchy
      const updated = await invoke<Actor[]>('get_all_actors');
      setActors(updated);

      // Recompile and render (use compile_scene to sync Rust -> C++)
      await invoke('compile_scene');
      const viewport = viewportRef();
      if (viewport) {
        viewport.render();
      }
    } catch (error) {
      console.error('Failed to set parent:', error);
    }
  };

  const handleActorNodeCreated = async () => {
    try {
      // Refresh actors list to show newly created ActorNode
      const updated = await invoke<Actor[]>('get_all_actors');
      setActors(updated);

      // Mark scene as loaded so camera controls work (same as handleObjectAdded)
      const viewport = viewportRef();
      if (viewport) {
        viewport.markSceneLoaded();
      }
    } catch (error) {
      console.error('Failed to refresh actors after ActorNode creation:', error);
    }
  };

  const handleNavigateToActorNode = async (actorId: number) => {
    try {
      // Navigate to the ActorNode in the graph
      await invoke('navigate_to_actor_node', { actorNodeUid: actorId });

      // Refresh the NodeGraph to show the new context
      const handle = nodeGraphHandle();
      if (handle) {
        await handle.refresh();
      }
    } catch (error) {
      console.error('Failed to navigate to ActorNode:', error);
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
    unlistenSave = await listen('menu-save', () => {
      console.log('menu-save event received!');
      handleSave();
    });
    unlistenSaveAs = await listen('menu-save-as', () => {
      console.log('menu-save-as event received!');
      handleSaveAs();
    });
    console.log('Menu event listeners set up');

    // Load initial actors (including root) on startup
    try {
      const initialActors = await invoke<Actor[]>('get_all_actors');
      setActors(initialActors);
      console.log('Loaded initial actors:', initialActors.length);
    } catch (error) {
      console.error('Failed to load initial actors:', error);
    }

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
    unlistenSave?.();
    unlistenSaveAs?.();
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

        <div class="main-content-wrapper">
          <div class="main-content">
            {/* Viewport */}
            <div class="viewport-panel">
              <Viewport
                ref={setViewportRef}
                cameraPosition={cameraPosition}
                onCameraPositionChange={setCameraPosition}
                onAssetDropped={handleAssetDropped}
              />
            </div>

            <ResizeHandle
              direction="horizontal"
              onResize={(delta) => {
                // Optional: can be used to adjust flex-basis if needed
              }}
            />

            {/* Middle Panel - Scene Hierarchy + Node Graph */}
            <div class="middle-panel-stack">
              {/* Scene Hierarchy */}
              <div class="hierarchy-panel panel" style={{ height: `${hierarchyHeight()}px` }}>
                <h3>Scene Hierarchy</h3>
                <div class="panel-scroll-content">
                  <SceneHierarchy
                    actors={actors}
                    selectedActorId={selectedActorId}
                    onActorSelect={setSelectedActorId}
                    isLoading={isLoading}
                    onAssetDropped={handleAssetDropped}
                    onActorRemove={handleActorRemove}
                    onActorReorder={handleActorReorder}
                    onSetParent={handleSetParent}
                    onNavigateToNode={handleNavigateToActorNode}
                  />
                </div>
              </div>

              <ResizeHandle
                direction="vertical"
                onResize={(delta) => setHierarchyHeight((h) => Math.max(100, Math.min(600, h - delta)))}
              />

              {/* Node Graph */}
              <div class="node-graph-panel-flex">
                <NodeGraph
                  onEvaluate={async () => {
                    await invoke('compile_scene_no_sync');
                    const viewport = viewportRef();
                    if (viewport) {
                      viewport.render();
                    }
                  }}
                  onActorNodeCreated={handleActorNodeCreated}
                  onActorNodeSelected={setSelectedActorId}
                  onReady={setNodeGraphHandle}
                />
              </div>
            </div>

            <ResizeHandle
              direction="horizontal"
              onResize={(delta) => setMiddlePanelWidth((w) => Math.max(250, Math.min(800, w + delta)))}
            />

            {/* Properties Panel */}
            <div class="right-panel panel" style={{ width: `${rightPanelWidth()}px` }}>
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

          {/* Bottom Resources Panel */}
          <ResizeHandle
            direction="vertical"
            onResize={(delta) => setBottomPanelHeight((h) => Math.max(100, Math.min(400, h - delta)))}
          />
          <div class="bottom-resources-panel" style={{ height: `${bottomPanelHeight()}px` }}>
            <h3>Resources</h3>
            <ResourcesBrowser
              assets={getAssets()}
              currentAssetPath={currentScene}
              onAssetSelect={(asset) => handleAssetDropped(asset.path)}
              onAssetRemove={removeAsset}
            />
          </div>
        </div>
      </div>
    </Show>
  );
};

export default App;
