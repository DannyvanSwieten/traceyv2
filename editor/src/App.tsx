import { Component, Show, createEffect, createSignal, onMount, onCleanup } from 'solid-js';
import * as api from './lib/api';
import { Viewport, ViewportHandle, CameraPosition } from './components/viewport/Viewport';
import {
  SceneHierarchy,
  Actor,
} from './components/scene-hierarchy/SceneHierarchy';
import { ResourcesBrowser } from './components/resources-browser/ResourcesBrowser';
import { RenderSettings } from './components/render-settings/RenderSettings';
import { CameraControls } from './components/camera-controls/CameraControls';
import { ActorProperties, Transform } from './components/actor-properties/ActorProperties';
import { MaterialGraphEditor } from './components/material-graph/MaterialGraphEditor';
import { SopGraphEditor } from './components/sop-graph/SopGraphEditor';
import { ExportVideoDialog } from './components/export-video/ExportVideoDialog';
import { Splitter } from './components/splitter/Splitter';
import { Playbar } from './components/playbar/Playbar';
import {
  Dopesheet,
  DOPESHEET_DEFAULT_HEIGHT,
  DOPESHEET_MAX_HEIGHT,
  DOPESHEET_MIN_HEIGHT,
} from './components/dopesheet/Dopesheet';
import { getAssets, addAsset, removeAsset } from './stores/assets';
import {
  addNode,
  flushSopGraph,
  isCooking,
  loadSopGraphFromEngine,
  moveNodeAnywhere,
  nodeIsSubnet,
  redo,
  removeNodeAnywhere,
  sopGraph,
  undo,
} from './stores/sops';
import { buildSubnetsFromGltf } from './lib/gltf_import';
import { fetchCatalog as fetchSopCatalog, findNodeRecursive } from './lib/sop_graph';
import { isVopEditorOpen } from './stores/vops';
import {
  currentFrame,
  seekFrame,
  setKeyAtPlayhead,
  timeline,
  togglePlayPause,
} from './stores/timeline';
import './App.css';

const clamp = (v: number, lo: number, hi: number) => Math.min(Math.max(v, lo), hi);

// Persisted UI layout. Panel widths/heights are stored as one JSON blob in
// localStorage so the editor reopens at the size the user last left it. Bump
// the key suffix when the shape changes to avoid loading stale fields.
const LAYOUT_STORAGE_KEY = 'tracey-layout-v1';
interface PersistedLayout {
  leftPanelW?: number;
  rightPanelW?: number;
  sopDockW?: number;
  browserH?: number;
  dopesheetH?: number;
}
function loadPersistedLayout(): PersistedLayout {
  try {
    const s = localStorage.getItem(LAYOUT_STORAGE_KEY);
    return s ? (JSON.parse(s) as PersistedLayout) : {};
  } catch {
    return {};
  }
}

const App: Component = () => {
  const [currentScene, setCurrentScene] = createSignal<string | null>(null);
  const [actors, setActors] = createSignal<Actor[]>([]);
  const [selectedActorId, setSelectedActorId] = createSignal<number | null>(
    null
  );
  // Mirror the active selection to the native server so the orbital camera
  // pivots around the selected actor. Fire-and-forget; selection state lives
  // on the frontend.
  createEffect(() => {
    api.selectActor(selectedActorId()).catch((err) => {
      console.warn('select_actor failed:', err);
    });
  });
  const [cameraPosition, setCameraPosition] = createSignal<CameraPosition>({
    x: 0,
    y: 0,
    z: 3,
  });
  const [materialEditorOpen, setMaterialEditorOpen] = createSignal(false);
  const [sopEditorOpen, setSopEditorOpen] = createSignal(false);
  const [exportVideoOpen, setExportVideoOpen] = createSignal(false);
  // Resizable panel sizes — seeded from localStorage so the layout survives
  // across sessions. Min/max stay loose enough for laptop displays.
  const persisted = loadPersistedLayout();
  const [leftPanelW, setLeftPanelW] = createSignal(persisted.leftPanelW ?? 250);
  const [rightPanelW, setRightPanelW] = createSignal(persisted.rightPanelW ?? 300);
  // Width of the docked graph editor (shared between SOP and VOP — drilling
  // into an attribute_vop swaps the contents in place rather than opening a
  // second dock).
  const [sopDockW, setSopDockW] = createSignal(persisted.sopDockW ?? 600);
  const [browserH, setBrowserH] = createSignal(persisted.browserH ?? 150);
  const [dopesheetH, setDopesheetH] = createSignal(
    persisted.dopesheetH ?? DOPESHEET_DEFAULT_HEIGHT,
  );

  // Persist layout changes. createEffect tracks all five signals and writes
  // the merged blob on every drag-end (Splitter writes mid-drag too, which
  // is fine — localStorage is cheap and the writes are tiny).
  createEffect(() => {
    const blob: PersistedLayout = {
      leftPanelW: leftPanelW(),
      rightPanelW: rightPanelW(),
      sopDockW: sopDockW(),
      browserH: browserH(),
      dopesheetH: dopesheetH(),
    };
    try {
      localStorage.setItem(LAYOUT_STORAGE_KEY, JSON.stringify(blob));
    } catch (e) {
      console.warn('failed to persist UI layout:', e);
    }
  });
  let viewportRef: ViewportHandle | undefined;
  let unlistenImport: (() => void) | undefined;
  let unlistenExport: (() => void) | undefined;

  // Import is just "remember this file" — adds an entry to the asset browser
  // and stops. Pulling geometry into the scene is a separate action the user
  // triggers from the asset row (handleLoadAsset). This split lets the user
  // line up several files first, then decide which to actually load (and
  // re-load the same asset multiple times for repeated instances).
  const handleImport = async () => {
    try {
      const selected = await api.openFileDialog('Import glTF', [
        { description: 'glTF', extensions: ['gltf', 'glb'] },
      ]);
      if (!selected) return;
      addAsset(selected);
    } catch (error) {
      console.error('Failed to open file dialog:', error);
    }
  };

  // Load an asset that's already in the browser: peek its glTF hierarchy
  // and drop a recursive subnet tree into the SOP graph at the current
  // navigation path (root if no subnet entered). The next cook produces a
  // parented actor tree mirroring the file. Safe to call multiple times on
  // the same asset — each call creates a fresh subnet subtree with new uids.
  const handleLoadAsset = async (asset: { id?: string; path: string }) => {
    try {
      // Snapshot the actor count so we can tell whether the cook actually
      // produced anything. If it doesn't grow within a couple seconds the
      // import silently failed downstream (gltf_import couldn't open the
      // file, mesh name mismatch, etc.) and the viewport would otherwise
      // stay blank with no indication of why.
      const beforeCount = actors().length;

      const subnets = await buildSubnetsFromGltf(asset.path);
      for (const s of subnets) addNode(s);
      // Skip the 300ms debounce so the cook fires immediately. Without
      // this, the user sees the subnet shapes appear in the canvas but
      // nothing changes in the viewport for a third of a second — long
      // enough to feel broken.
      await flushSopGraph();
      if (viewportRef) viewportRef.render();

      // Wait for the next scene_changed (the engine emits one per cook
      // completion), with a generous timeout so we don't hang the UI on a
      // truly stuck cook. Then verify the actor list actually grew — empty
      // imports show as "blank viewport" which is the symptom we're
      // diagnosing.
      const sawSceneChanged = await new Promise<boolean>((resolve) => {
        let done = false;
        const timer = setTimeout(() => {
          if (done) return;
          done = true;
          unlisten();
          resolve(false);
        }, 3000);
        const unlisten = api.listen('scene_changed', () => {
          if (done) return;
          done = true;
          clearTimeout(timer);
          unlisten();
          resolve(true);
        });
      });

      if (!sawSceneChanged) {
        console.error('Load timed out — no scene_changed within 3s for', asset.path);
        window.alert(
          `Load timed out:\n${asset.path}\n\nThe engine did not finish cooking within 3 seconds. Check the editor console for backend errors.`,
        );
        return;
      }
      // Refresh actor list now that the cook is applied.
      try {
        const fresh = await api.getAllActors();
        setActors(fresh);
        if (fresh.length <= beforeCount) {
          console.warn('Load completed but actor count did not grow:', asset.path);
          window.alert(
            `Loaded ${asset.path} but no geometry actors appeared.\n\nLikely causes: file is corrupt, mesh name mismatch, or the engine couldn't open the file. Check the editor console.`,
          );
        }
      } catch (e) {
        console.warn('actor refresh after load failed:', e);
      }
    } catch (e) {
      // The asset list survives across sessions (localStorage), so common
      // failure here is a stale path: the file got moved/deleted, or the
      // user opened the editor on a different machine. Surface it instead
      // of console.warn'ing into the void, and offer to drop the entry.
      const msg = e instanceof Error ? e.message : String(e);
      console.error('glTF subnet import failed for', asset.path, ':', msg);
      const drop = window.confirm(
        `Failed to load asset:\n${asset.path}\n\n${msg}\n\nRemove this asset from the browser?`,
      );
      if (drop && asset.id) removeAsset(asset.id);
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
      await api.compileScene();
      if (viewportRef) {
        viewportRef.render();
      }
    } catch (error) {
      console.error('Failed to recompile scene after transform change:', error);
    }
  };

  let unlistenOpenScene: (() => void) | undefined;
  let unlistenSaveScene: (() => void) | undefined;
  let unlistenSaveSceneAs: (() => void) | undefined;
  let unlistenSceneChanged: (() => void) | undefined;

  // Last path the user opened from or saved to. Cmd+S writes to this
  // directly when set; Cmd+Shift+S (or first save) always prompts and
  // updates it.
  const [currentScenePath, setCurrentScenePath] = createSignal<string | null>(null);

  const handleOpenScene = async () => {
    try {
      const selected = await api.openFileDialog('Open Scene', [
        { description: 'Tracey Scene', extensions: ['tracey', 'json'] },
      ]);
      if (!selected) return;
      await api.loadScene(selected);
      setCurrentScenePath(selected);
      // Refresh the actor list — the backend broadcasts scene_changed but
      // the actors signal is owned in App.tsx and has no listener.
      try {
        const fresh = await api.getAllActors();
        setActors(fresh);
      } catch (e) {
        console.warn('actor refresh after load failed:', e);
      }
      if (viewportRef) viewportRef.render();
    } catch (e) {
      console.error('Open Scene failed:', e);
    }
  };

  // Save (Cmd+S) — write to currentScenePath without a dialog. Falls back
  // to a Save-As prompt on the first save (or if the path was somehow
  // cleared) so the user isn't stuck with no way to write.
  const handleSaveScene = async () => {
    const dest = currentScenePath();
    if (dest) {
      try {
        await api.saveScene(dest);
      } catch (e) {
        console.error('Save Scene failed:', e);
      }
      return;
    }
    await handleSaveSceneAs();
  };

  // Save As (Cmd+Shift+S) — always prompt for a destination and remember
  // it so subsequent Cmd+S writes silently.
  const handleSaveSceneAs = async () => {
    try {
      const selected = await api.saveFileDialog(
        'Save Scene As',
        currentScenePath() ?? 'scene.tracey',
        [{ description: 'Tracey Scene', extensions: ['tracey', 'json'] }],
      );
      if (!selected) return;
      await api.saveScene(selected);
      setCurrentScenePath(selected);
    } catch (e) {
      console.error('Save Scene As failed:', e);
    }
  };

  onMount(() => {
    unlistenImport = api.listen('menu-import', () => handleImport());
    unlistenExport = api.listen('menu-export', () => {
      // File → Export… (Cmd+E) opens the video export dialog.
      setExportVideoOpen(true);
    });
    unlistenOpenScene = api.listen('menu-open-scene', () => handleOpenScene());
    unlistenSaveScene = api.listen('menu-save-scene', () => handleSaveScene());
    unlistenSaveSceneAs = api.listen('menu-save-scene-as', () => handleSaveSceneAs());

    // Keep the scene hierarchy live: every SOP cook re-emits actors and the
    // server broadcasts `scene_changed`. Without this, adding e.g. an
    // object_output node in the docked SOP graph leaves the hierarchy stale
    // until the dock is closed (which is the only other refresh path).
    unlistenSceneChanged = api.listen('scene_changed', () => {
      api.getAllActors()
        .then(setActors)
        .catch((e) => console.warn('actor refresh after scene_changed failed:', e));
    });

    // Hydrate the SOP node catalog eagerly. The catalog is what `makeNode`
    // resolves kinds against; any code path that constructs SOP nodes
    // (notably gltf_import.ts on asset load) silently produces null nodes
    // before the catalog arrives. Loading at startup means importers and
    // shortcuts work regardless of whether the user has opened the SOP
    // dock yet.
    fetchSopCatalog().catch((e) =>
      console.warn('initial SOP catalog fetch failed:', e),
    );

    // Hydrate the SOP graph store eagerly so the inspector's keyframe dots
    // can read channel state without first opening the SOP editor.
    loadSopGraphFromEngine().catch((e) =>
      console.warn('initial SOP graph load failed:', e),
    );

    // App-level Cmd+Z / Cmd+Shift+Z + timeline transport shortcuts. We don't
    // put these in the native menu because that would unconditionally
    // swallow them and break native text-input editing inside the inspector.
    // Here we only intercept when focus is NOT on an editable element — text
    // fields keep their browser shortcuts, everything else (canvas,
    // hierarchy, toolbar) gets the app behaviour.
    //
    // Transport shortcuts:
    //   Space          → play / pause
    //   ←  / →         → step one frame
    //   Home / End     → jump to range start / end
    //   K              → key the selected actor's pose (translate / rotate /
    //                    scale, all 3 axes each) at the current playhead
    const onAppKeyDown = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement | null;
      const tag = target?.tagName;
      const editable =
        tag === 'INPUT' ||
        tag === 'TEXTAREA' ||
        target?.isContentEditable === true;
      if (editable) return;

      // Undo / redo first — these have a modifier and shouldn't conflict with
      // anything else.
      const isUndoCombo =
        (e.metaKey || e.ctrlKey) && !e.altKey && e.key.toLowerCase() === 'z';
      if (isUndoCombo) {
        e.preventDefault();
        const op = e.shiftKey ? redo : undo;
        op()
          .then((applied) => {
            if (!applied) return;
            api.getAllActors()
              .then(setActors)
              .catch((err) =>
                console.warn('actor refresh after undo/redo failed:', err),
              );
          })
          .catch((err) => console.warn('undo/redo failed:', err));
        return;
      }

      // The SOP graph canvas owns its own Space-to-pan + Delete shortcuts.
      // When the canvas has focus, defer to it so we don't double-handle.
      const insideSopDock = target?.closest?.('.sop-graph-dock');
      if (insideSopDock) return;

      // Transport — no modifiers (so Cmd+Arrow etc. fall through to OS).
      if (e.metaKey || e.ctrlKey || e.altKey) return;

      const t = timeline();
      switch (e.key) {
        case ' ':
          e.preventDefault();
          togglePlayPause();
          break;
        case 'ArrowLeft':
          e.preventDefault();
          seekFrame(Math.round(currentFrame()) - 1);
          break;
        case 'ArrowRight':
          e.preventDefault();
          seekFrame(Math.round(currentFrame()) + 1);
          break;
        case 'Home':
          e.preventDefault();
          seekFrame(t.frame_start);
          break;
        case 'End':
          e.preventDefault();
          seekFrame(t.frame_end);
          break;
        case 'k':
        case 'K': {
          e.preventDefault();
          const id = selectedActorId();
          if (id === null) return;
          const actor = actors().find((a) => a.id === id);
          if (!actor || actor.sop_node_uid == null) return;
          const uid = actor.sop_node_uid;
          // Pose snapshot: translate.x/y/z + scale.x/y/z from the live actor
          // transform; rotate_euler_deg.x/y/z from the SOP node directly
          // (actor.transform stores rotation as a quaternion and we keep
          // euler authoritative).
          const tx = actor.transform.position;
          const sc = actor.transform.scale;
          const writes: Promise<void>[] = [
            setKeyAtPlayhead({ nodeUid: uid, paramName: 'translate', component: 0, value: tx.x }),
            setKeyAtPlayhead({ nodeUid: uid, paramName: 'translate', component: 1, value: tx.y }),
            setKeyAtPlayhead({ nodeUid: uid, paramName: 'translate', component: 2, value: tx.z }),
            setKeyAtPlayhead({ nodeUid: uid, paramName: 'scale',     component: 0, value: sc.x }),
            setKeyAtPlayhead({ nodeUid: uid, paramName: 'scale',     component: 1, value: sc.y }),
            setKeyAtPlayhead({ nodeUid: uid, paramName: 'scale',     component: 2, value: sc.z }),
          ];
          const node = findNodeRecursive(sopGraph(), uid);
          const rot = node?.params['rotate_euler_deg'];
          if (rot && rot.type === 'vec3') {
            writes.push(setKeyAtPlayhead({ nodeUid: uid, paramName: 'rotate_euler_deg', component: 0, value: rot.value[0] }));
            writes.push(setKeyAtPlayhead({ nodeUid: uid, paramName: 'rotate_euler_deg', component: 1, value: rot.value[1] }));
            writes.push(setKeyAtPlayhead({ nodeUid: uid, paramName: 'rotate_euler_deg', component: 2, value: rot.value[2] }));
          }
          Promise.allSettled(writes).catch(() => {});
          break;
        }
      }
    };
    window.addEventListener('keydown', onAppKeyDown);
    unlistenKeydown = () => window.removeEventListener('keydown', onAppKeyDown);
  });

  let unlistenKeydown: (() => void) | undefined;

  onCleanup(() => {
    unlistenImport?.();
    unlistenExport?.();
    unlistenOpenScene?.();
    unlistenSaveScene?.();
    unlistenSaveSceneAs?.();
    unlistenSceneChanged?.();
    unlistenKeydown?.();
  });

  return (
    <div class="app">
      <div class="toolbar">
        <h1>Tracey Editor</h1>
        <button
          class="toolbar-button"
          type="button"
          onClick={() => setMaterialEditorOpen(true)}
        >
          Material Graph
        </button>
        <button
          class="toolbar-button"
          type="button"
          onClick={async () => {
            // Closing flushes any pending edits and refreshes the hierarchy,
            // matching the behaviour of the in-dock Close button.
            if (sopEditorOpen()) {
              setSopEditorOpen(false);
              try {
                const fresh = await api.getAllActors();
                setActors(fresh);
              } catch (e) {
                console.warn('actor refresh after SOP close failed:', e);
              }
              if (viewportRef) viewportRef.render();
            } else {
              setSopEditorOpen(true);
            }
          }}
        >
          SOP Graph
        </button>
        <button
          class="toolbar-button"
          type="button"
          onClick={() => setExportVideoOpen(true)}
        >
          Export Video…
        </button>
        <Show when={isCooking()}>
          <div class="toolbar-cook-status" role="status" aria-live="polite">
            <span class="toolbar-cook-spinner" aria-hidden="true" />
            <span>Cooking…</span>
          </div>
        </Show>
      </div>

      <MaterialGraphEditor
        open={materialEditorOpen}
        onClose={async () => {
          setMaterialEditorOpen(false);
          // Force a fresh frame: the engine cleared the accumulator on
          // graph-set, but the viewport doesn't know to re-tick on its own.
          if (viewportRef) viewportRef.render();
        }}
      />

      <ExportVideoDialog
        open={exportVideoOpen}
        onClose={() => {
          setExportVideoOpen(false);
          if (viewportRef) viewportRef.render();
        }}
      />

      <div
        class="main-content"
        ref={(el) => {
          // Drive the layout sizes via CSS custom properties. createEffect on
          // a ref keeps inline `style={…}` out of the JSX (the lint config
          // blocks dynamic inline styles).
          createEffect(() => {
            el.style.setProperty('--left-panel-w', `${leftPanelW()}px`);
            el.style.setProperty('--right-panel-w', `${rightPanelW()}px`);
            el.style.setProperty('--sop-dock-w', `${sopDockW()}px`);
            el.style.setProperty('--browser-h', `${browserH()}px`);
          });
        }}
      >
        <div class="left-panel panel">
          <h3>Scene Hierarchy</h3>
          <SceneHierarchy
            actors={actors}
            selectedActorId={selectedActorId}
            onActorSelect={setSelectedActorId}
            onActorVisibilityChange={(id, visible) =>
              setActors((prev) =>
                prev.map((a) => (a.id === id ? { ...a, visible } : a)),
              )
            }
            onActorDelete={async (id) => {
              const a = actors().find((x) => x.id === id);
              if (!a || a.sop_node_uid == null) return;
              if (!removeNodeAnywhere(a.sop_node_uid)) return;
              if (selectedActorId() === id) setSelectedActorId(null);
              await flushSopGraph();
              try {
                setActors(await api.getAllActors());
              } catch (e) {
                console.warn('actor refresh after delete failed:', e);
              }
              if (viewportRef) viewportRef.render();
            }}
            onActorReorder={async (sourceId, targetId, mode) => {
              const src = actors().find((a) => a.id === sourceId);
              const tgt = actors().find((a) => a.id === targetId);
              if (!src || !tgt || src.sop_node_uid == null || tgt.sop_node_uid == null) return;
              if (!moveNodeAnywhere(src.sop_node_uid, tgt.sop_node_uid, mode)) return;
              await flushSopGraph();
              try {
                setActors(await api.getAllActors());
              } catch (e) {
                console.warn('actor refresh after reorder failed:', e);
              }
              if (viewportRef) viewportRef.render();
            }}
            canDropInside={(targetId) => {
              const a = actors().find((x) => x.id === targetId);
              return !!(a && a.sop_node_uid != null && nodeIsSubnet(a.sop_node_uid));
            }}
            isLoading={() => false}
          />
        </div>

        <Splitter
          orientation="vertical"
          onDrag={(dx) => setLeftPanelW((w) => clamp(w + dx, 150, 600))}
        />

        <div class="center-area">
          <div class="viewport-container">
            <Viewport
              ref={(ref) => (viewportRef = ref)}
              cameraPosition={cameraPosition}
              onCameraPositionChange={setCameraPosition}
            />
          </div>
          <Splitter
            orientation="horizontal"
            onDrag={(dy) => setBrowserH((h) => clamp(h - dy, 80, 600))}
          />
          <div class="resources-wrapper">
            <ResourcesBrowser
              assets={getAssets()}
              currentAssetPath={currentScene}
              onAssetSelect={(asset) => setCurrentScene(asset.path)}
              onAssetLoad={handleLoadAsset}
              onAssetRemove={removeAsset}
            />
          </div>
        </div>

        <Show when={sopEditorOpen() || isVopEditorOpen()}>
          <Splitter
            orientation="vertical"
            onDrag={(dx) => setSopDockW((w) => clamp(w - dx, 380, 1400))}
          />
          <SopGraphEditor
            onClose={async () => {
              setSopEditorOpen(false);
              // Refresh the actor list so the hierarchy reflects whatever
              // the last SOP cook emitted before the dock closed.
              try {
                const fresh = await api.getAllActors();
                setActors(fresh);
              } catch (e) {
                console.warn('actor refresh after SOP edit failed:', e);
              }
              if (viewportRef) viewportRef.render();
            }}
          />
        </Show>

        <Splitter
          orientation="vertical"
          onDrag={(dx) => setRightPanelW((w) => clamp(w - dx, 200, 600))}
        />

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

      <Splitter
        orientation="horizontal"
        onDrag={(dy) =>
          setDopesheetH((h) =>
            clamp(h - dy, DOPESHEET_MIN_HEIGHT, DOPESHEET_MAX_HEIGHT),
          )
        }
      />
      <div
        class="dopesheet-wrapper"
        ref={(el) => {
          // Reactive write of the dopesheet height to a CSS variable, same
          // pattern as the playhead position in Playbar.tsx. Avoids inline
          // `style={...}` while still tracking the signal.
          createEffect(() => el.style.setProperty('--dopesheet-h', `${dopesheetH()}px`));
        }}
      >
        <Dopesheet />
      </div>

      <Playbar />
    </div>
  );
};

export default App;
