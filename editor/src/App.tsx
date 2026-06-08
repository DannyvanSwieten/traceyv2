import { Component, Show, createEffect, createSignal, onMount, onCleanup } from 'solid-js';
import * as api from './lib/api';
import { Viewport, ViewportHandle, CameraPosition } from './components/viewport/Viewport';
import {
  SceneHierarchy,
  Actor,
} from './components/scene-hierarchy/SceneHierarchy';
import { ResourcesBrowser } from './components/resources-browser/ResourcesBrowser';
import { RasterizerToolbar } from './components/rasterizer-toolbar/RasterizerToolbar';
import { RenderPanel } from './components/render-panel/RenderPanel';
import { CameraControls } from './components/camera-controls/CameraControls';
import { ActorProperties, Transform } from './components/actor-properties/ActorProperties';
import { MaterialGraphEditor } from './components/material-graph/MaterialGraphEditor';
import { SopGraphEditor } from './components/sop-graph/SopGraphEditor';
import { DopGraphPanel } from './components/dop-graph/DopGraphPanel';
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
import { isMaterialEditorOpen, setMaterialEditorOpen } from './stores/materials';
import { CommandPalette } from './components/command-palette/CommandPalette';
import { toggleCommandPalette, registerCommands } from './lib/command_palette';
import { GrabOverlay } from './components/viewport-grab/GrabOverlay';
import {
  startGrab,
  updateGrab,
  setAxis,
  setOrigin as setGrabOrigin,
  commitGrab,
  cancelGrab,
  grabState,
  isGrabActive,
  type Axis,
} from './lib/viewport_grab';
import { PieMenu } from './components/pie-menu/PieMenu';
import {
  openPieMenu,
  commitPieMenu,
  dismissPieMenu,
  updatePieMenuCursor,
  setPieMenuWedges,
  isPieMenuOpen,
} from './lib/pie_menu';
import { WorkspaceTabs } from './components/workspaces/WorkspaceTabs';
import { WorkspaceBar } from './components/workspaces/WorkspaceBar';
import {
  WORKSPACES,
  WORKSPACE_LABELS,
  activeWorkspace,
  setActiveWorkspaceInternal,
  type WorkspaceName,
} from './lib/workspaces';
import { autoKeyEnabled } from './lib/auto_key';
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
  // Drive the rasterizer translate-gizmo from selection + the selected
  // actor's transform. Hidden when no actor is selected. Axis length is a
  // fixed world value for now — when we wire camera matrices client-side
  // we can scale it by camera distance for screen-stable visual size.
  createEffect(() => {
    const id = selectedActorId();
    if (id === null) {
      api.setGizmoVisible(false).catch(() => {});
      return;
    }
    const actor = actors().find((a) => a.id === id);
    if (!actor) {
      api.setGizmoVisible(false).catch(() => {});
      return;
    }
    const p = actor.transform.position;
    api.setGizmoAnchor(p.x, p.y, p.z, 1.0).catch(() => {});
    api.setGizmoVisible(true).catch(() => {});
  });
  const [cameraPosition, setCameraPosition] = createSignal<CameraPosition>({
    x: 0,
    y: 0,
    z: 3,
  });
  // Material dock visibility lives in the materials store so the
  // actor-inspector's "New Material" flow can open it without prop
  // drilling. App just reads the signal for layout decisions.
  const materialEditorOpen = isMaterialEditorOpen;
  // SOP dock defaults to open — it's the primary editing surface, so
  // there's no reason to hide it on first launch.
  const [sopEditorOpen, setSopEditorOpen] = createSignal(true);
  // DOP graph editor shares the same dock slot as SOP / Material.
  const [dopEditorOpen, setDopEditorOpen] = createSignal(false);
  const [exportVideoOpen, setExportVideoOpen] = createSignal(false);
  // Mirrors the native EditorServer::m_pt_preview_enabled. Seeded from
  // the native side on mount so a future-default change (or a project
  // file storing the preference) flows through; the toolbar button is
  // the only writer beyond that.
  const [ptPreviewEnabled, setPtPreviewEnabled] = createSignal(false);
  // Frame-lock toggle. Mirrors TimelineState::frame_locked on the native
  // side: off = async / wall-clock playback (default), on = advance one
  // frame per cook completion. Seeded on mount so the toolbar matches
  // engine state.
  const [frameLocked, setFrameLocked] = createSignal(false);
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

  // Latest pointer position in client coords, updated globally on
  // pointermove. The Blender-modal grab uses this both as the grab origin
  // (so it doesn't snap to wherever the cursor was at app start) and as
  // the per-frame delta source.
  const lastViewportPointer = { x: 0, y: 0 };
  // Whether the next broadcast after grab-activation should be treated as
  // the origin. The Metal-view pointer coords don't agree with browser
  // clientX/Y so we re-seed the grab's startX/Y from the first sample.
  let needGrabOrigin = false;

  // Render-workspace bar signals. Mirrors the engine's max-samples /
  // max-bounces values so the sliders show the right starting value
  // when the user enters Render mode; setters round-trip through the
  // IPC so the engine adopts the change immediately.
  const [maxSamples, setMaxSamplesSignal] = createSignal(1024);
  const [maxBounces, setMaxBouncesSignal] = createSignal(8);
  // [w, h]. [0, 0] = match viewport pixel size; otherwise the PT renders
  // at this fixed resolution and the viewport blit scales to fit.
  const [renderResolution, setRenderResolutionSignal] =
    createSignal<[number, number]>([0, 0]);
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

  // Key the selected actor's translate/scale/rotate at the playhead. Used
  // both by the K hotkey and the WorkspaceBar's "Set Key" button — and by
  // the Auto-Key toggle which calls this after every transform commit.
  const keySelectedActorPose = () => {
    const id = selectedActorId();
    if (id === null) return;
    const actor = actors().find((a) => a.id === id);
    if (!actor || actor.sop_node_uid == null) return;
    const uid = actor.sop_node_uid;
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
  };

  // Wrappers that flip the native grab-active flag alongside the JS state.
  // Mandatory because pointer events over the Metal viewport never reach
  // the WebView — the engine has to broadcast pointer state back to us.
  const startGrabSession = async (
    kind: 'translate' | 'rotate' | 'scale',
    actorId: number,
    transform: api.Transform,
    pointerX: number,
    pointerY: number,
  ) => {
    // Flip the native gate FIRST so the engine stops orbiting the camera
    // even if startGrab (which awaits a getCamera roundtrip) takes a few
    // frames. Without this leading call, dragging during the window
    // between "G pressed" and "grab fully armed" still pans / orbits.
    try {
      await api.setViewportGrabActive(true);
    } catch (e) {
      console.error('setViewportGrabActive(true) failed:', e);
      return;
    }
    try {
      await startGrab(kind, actorId, transform, pointerX, pointerY);
    } catch (e) {
      console.error('startGrab failed:', e);
      api.setViewportGrabActive(false).catch(() => {});
      return;
    }
  };
  const endGrabSession = (mode: 'commit' | 'cancel') => {
    if (mode === 'commit') {
      commitGrab();
      if (autoKeyEnabled()) keySelectedActorPose();
    } else {
      cancelGrab().catch((err) => console.error('cancelGrab failed:', err));
    }
    api.setViewportGrabActive(false).catch(() => {});
  };

  const applyWorkspace = (name: WorkspaceName) => {
    const w = WORKSPACES[name];
    setSopEditorOpen(w.sopOpen);
    setMaterialEditorOpen(w.materialOpen);
    setDopEditorOpen(w.dopOpen);
    setDopesheetH(w.dopesheetH);
    setActiveWorkspaceInternal(name);

    // Render workspace: promote the path tracer from the small PiP inset
    // to the entire viewport so the user gets a dedicated final-look view.
    // Auto-enables PT preview on entry; flips back to PiP layout on exit
    // (but does NOT disable preview, since the user may have turned it on
    // explicitly via the existing toggle).
    if (name === 'render') {
      api.setPtPreview(true).catch((e) => console.error('setPtPreview:', e));
      api.setPtFullscreen(true).catch((e) => console.error('setPtFullscreen:', e));
    } else {
      api.setPtFullscreen(false).catch((e) => console.error('setPtFullscreen:', e));
    }
  };

  onMount(() => {
    // Global pointermove tracker drives the modal grab. We listen at the
    // window level (not just the viewport) because Blender-style modal
    // transforms continue even when the cursor leaves the placeholder
    // div — until commit or cancel.
    const onPointerMove = (e: PointerEvent) => {
      lastViewportPointer.x = e.clientX;
      lastViewportPointer.y = e.clientY;
      if (isGrabActive()) {
        const next = updateGrab(e.clientX, e.clientY);
        const g = grabState();
        if (next && g) {
          api.setActorTransform(g.actorId, next).catch(() => {});
        }
      }
      if (isPieMenuOpen()) {
        updatePieMenuCursor(e.clientX, e.clientY);
      }
    };
    const onPointerDown = (e: PointerEvent) => {
      if (!isGrabActive()) return;
      // Left-click commits, right-click cancels — matches Blender. Only
      // covers events that DO reach the WebView (i.e. on dock/panel area);
      // events over the Metal viewport are handled by the native pointer
      // broadcast below.
      if (e.button === 0) {
        e.preventDefault();
        endGrabSession('commit');
      } else if (e.button === 2) {
        e.preventDefault();
        endGrabSession('cancel');
      }
    };
    // Native pointer-state broadcast: fires per render_tick while the
    // grab is active. Drives updateGrab so the actor follows the cursor.
    // Click-to-commit is intentionally NOT wired here — too easy to
    // misfire when the user click-drags (their camera-orbit instinct):
    // the first button-down would close the grab before they ever see
    // the actor move. Use Enter to commit, Esc to cancel.
    const unlistenViewportPointer = api.listen('viewport_pointer', (msg) => {
      if (!isGrabActive()) return;
      const x = msg.x as number;
      const y = msg.y as number;
      if (needGrabOrigin) {
        setGrabOrigin(x, y);
        needGrabOrigin = false;
        return;
      }
      const next = updateGrab(x, y);
      const g = grabState();
      if (next && g) api.setActorTransform(g.actorId, next).catch(() => {});
    });
    onCleanup(unlistenViewportPointer);
    window.addEventListener('pointermove', onPointerMove);
    window.addEventListener('pointerdown', onPointerDown, { capture: true });
    onCleanup(() => {
      window.removeEventListener('pointermove', onPointerMove);
      window.removeEventListener('pointerdown', onPointerDown, { capture: true } as EventListenerOptions);
    });

    // Global command-palette registrations. Each canvas adds its own
    // node-creation commands on its mount; here we register the app-wide
    // file/edit/view actions so a single Cmd+K lookup finds all of them.
    const unregisterCommands = registerCommands([
      { id: 'app.openCommandPalette', label: 'Show All Commands…',
        group: 'Help', hint: '⌘K',
        run: () => toggleCommandPalette() },
      { id: 'file.openScene', label: 'Open Scene…',
        group: 'File', hint: '⌘O',
        keywords: 'load',
        run: handleOpenScene },
      { id: 'file.saveScene', label: 'Save Scene',
        group: 'File', hint: '⌘S',
        run: handleSaveScene },
      { id: 'file.saveSceneAs', label: 'Save Scene As…',
        group: 'File', hint: '⇧⌘S',
        run: handleSaveSceneAs },
      { id: 'file.import', label: 'Import…',
        group: 'File',
        keywords: 'gltf glb obj fbx',
        run: handleImport },
      { id: 'file.exportVideo', label: 'Export Video…',
        group: 'File', hint: '⌘E',
        keywords: 'render mp4',
        run: () => { setExportVideoOpen(true); } },
      { id: 'edit.undo', label: 'Undo',
        group: 'Edit', hint: '⌘Z',
        run: () => undo().then((ok) => {
          if (ok) api.getAllActors().then(setActors).catch(() => {});
        }) },
      { id: 'edit.redo', label: 'Redo',
        group: 'Edit', hint: '⇧⌘Z',
        run: () => redo().then((ok) => {
          if (ok) api.getAllActors().then(setActors).catch(() => {});
        }) },
      { id: 'view.toggleMaterialEditor', label: 'Toggle Material Graph',
        group: 'View',
        run: () => {
          if (materialEditorOpen()) {
            setMaterialEditorOpen(false);
          } else {
            setSopEditorOpen(false);
            setDopEditorOpen(false);
            setMaterialEditorOpen(true);
          }
        } },
      { id: 'view.toggleSopEditor', label: 'Toggle SOP Graph',
        group: 'View',
        run: () => {
          if (sopEditorOpen()) {
            setSopEditorOpen(false);
          } else {
            setMaterialEditorOpen(false);
            setDopEditorOpen(false);
            setSopEditorOpen(true);
          }
        } },
      { id: 'view.toggleDopEditor', label: 'Toggle DOP Graph',
        group: 'View',
        run: () => {
          if (dopEditorOpen()) {
            setDopEditorOpen(false);
          } else {
            setMaterialEditorOpen(false);
            setSopEditorOpen(false);
            setDopEditorOpen(true);
          }
        } },
      { id: 'playback.togglePlay', label: 'Play / Pause',
        group: 'Playback', hint: 'Space',
        run: () => togglePlayPause() },
      ...(Object.keys(WORKSPACES) as WorkspaceName[]).map((n) => ({
        id: `workspace.${n}`,
        label: `Workspace: ${WORKSPACE_LABELS[n]}`,
        group: 'Workspace',
        run: () => applyWorkspace(n),
      })),
    ]);
    onCleanup(unregisterCommands);

    // Pie-menu wedge set. Eight wedges arranged radially — chosen to cover
    // the operations users reach for most often while their eyes are on
    // the viewport. Q opens the menu at the cursor; releasing Q over a
    // wedge commits it.
    const grabActiveActor = (kind: 'translate' | 'rotate' | 'scale') => {
      const sel = selectedActorId();
      const a = sel === null ? null : actors().find((x) => x.id === sel);
      if (!a) return;
      startGrab(kind, a.id, a.transform,
                lastViewportPointer.x, lastViewportPointer.y).catch(() => {});
    };
    setPieMenuWedges([
      { label: 'Translate', run: () => grabActiveActor('translate') },
      { label: 'Rotate',    run: () => grabActiveActor('rotate') },
      { label: 'Scale',     run: () => grabActiveActor('scale') },
      { label: 'Toggle Points', run: async () => {
        try {
          const cur = await api.getShowPoints();
          await api.setShowPoints(!cur);
          viewportRef?.render();
        } catch (e) { console.error('toggle points:', e); }
      } },
      { label: 'Toggle Edges', run: async () => {
        try {
          const cur = await api.getShowEdges();
          await api.setShowEdges(!cur);
          viewportRef?.render();
        } catch (e) { console.error('toggle edges:', e); }
      } },
      { label: 'Toggle Ground', run: async () => {
        try {
          const cur = await api.getShowGround();
          await api.setShowGround(!cur);
          viewportRef?.render();
        } catch (e) { console.error('toggle ground:', e); }
      } },
      { label: 'Persp View', run: () => { api.setCameraView('persp').catch(() => {}); } },
      { label: 'Commands…', run: toggleCommandPalette },
    ]);
    onCleanup(() => dismissPieMenu());

    // Seed the render-workspace sliders with what the engine actually has.
    api.getMaxSamples().then(setMaxSamplesSignal).catch(() => {});
    api.getMaxBounces().then(setMaxBouncesSignal).catch(() => {});
    api.getPtRenderResolution()
       .then((r) => setRenderResolutionSignal([r.width, r.height]))
       .catch(() => {});

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
    //
    // Coalesce bursts: a particle sim ticking at ~60Hz fires 60
    // scene_changed events per second, but the hierarchy only ever
    // changes when the user adds/removes a SOP node (not when particles
    // move). rAF-debounce so all events within one animation frame
    // resolve to a single fetch + setActors. The trailing fetch still
    // lands within ~16ms of the last event, so the UI stays in sync;
    // we just stop hammering the IPC bridge 60×/sec with redundant
    // round-trips that re-fetch the same actor list.
    let scenePendingRaf: number | null = null;
    unlistenSceneChanged = api.listen('scene_changed', () => {
      if (scenePendingRaf !== null) return;
      scenePendingRaf = requestAnimationFrame(() => {
        scenePendingRaf = null;
        api.getAllActors()
          .then(setActors)
          .catch((e) => console.warn('actor refresh after scene_changed failed:', e));
      });
    });
    // Engine-side cook of the default SOP graph completes during native
    // startup — typically before this JS bundle has had a chance to
    // attach the `scene_changed` listener above, so the initial actor
    // emission is missed. Pull the current list once at mount so the
    // hierarchy panel (which now opens by default along with the SOP
    // dock) renders the seeded actors instead of staying empty until the
    // user pokes the graph.
    api.getAllActors()
      .then(setActors)
      .catch((e) => console.warn('initial actor fetch failed:', e));

    // Seed the PT-preview toggle from the native default so the toolbar
    // button reflects engine state even before the user touches it.
    api.getPtPreview()
      .then(setPtPreviewEnabled)
      .catch((e) => console.warn('initial pt_preview fetch failed:', e));

    api.timelineGetFrameLocked()
      .then(setFrameLocked)
      .catch((e) => console.warn('initial frame_locked fetch failed:', e));

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

      // Command palette: Cmd+K / Ctrl+K, also Cmd+P / Ctrl+P (VS Code
      // convention). Toggle so a second press dismisses it without the
      // user reaching for Escape. Wins over text-edit blocks because we
      // already returned early above for editable focus targets.
      if ((e.metaKey || e.ctrlKey) && !e.altKey && !e.shiftKey) {
        const k = e.key.toLowerCase();
        if (k === 'k' || k === 'p') {
          e.preventDefault();
          toggleCommandPalette();
          return;
        }
      }

      // Hold-Q pie menu: open on keydown at the current cursor, commit
      // wedge on keyup. e.repeat gate prevents auto-repeat from spamming
      // openPieMenu() while the key is held down.
      if (!e.metaKey && !e.ctrlKey && !e.altKey) {
        const k = e.key.toLowerCase();
        if (k === 'q' && !e.repeat) {
          if (!isPieMenuOpen()) {
            e.preventDefault();
            openPieMenu(lastViewportPointer.x, lastViewportPointer.y);
            return;
          }
        }
      }

      // Blender-style modal transform (G/R/S + X/Y/Z + Enter/Esc). Only
      // fires when an actor is selected and no modifiers are held.
      if (!e.metaKey && !e.ctrlKey && !e.altKey) {
        const k = e.key.toLowerCase();
        if (isGrabActive()) {
          if (k === 'x' || k === 'y' || k === 'z') {
            e.preventDefault();
            setAxis(k as Axis);
            return;
          }
          if (e.key === 'Escape') {
            e.preventDefault();
            endGrabSession('cancel');
            return;
          }
          if (e.key === 'Enter') {
            e.preventDefault();
            endGrabSession('commit');
            return;
          }
        } else if (k === 'g' || k === 'r' || k === 's') {
          const sel = selectedActorId();
          if (sel !== null) {
            const actor = actors().find((a) => a.id === sel);
            if (actor) {
              e.preventDefault();
              const kind = k === 'g' ? 'translate' : k === 'r' ? 'rotate' : 'scale';
              const px = lastViewportPointer.x;
              const py = lastViewportPointer.y;
              // The first native pointer broadcast becomes the origin —
              // see needGrabOrigin in the viewport_pointer listener.
              needGrabOrigin = true;
              startGrabSession(kind, actor.id, actor.transform, px, py)
                .catch((err) => console.error('startGrabSession failed:', err));
              return;
            }
          }
        }
      }

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
        case 'K':
          e.preventDefault();
          keySelectedActorPose();
          break;
      }
    };
    window.addEventListener('keydown', onAppKeyDown);
    // Q keyup commits the pie menu's currently-hovered wedge. Listening
    // on window (not on the keydown handler's scope) so a press-and-flick
    // that ends with focus elsewhere still fires the action.
    const onAppKeyUp = (e: KeyboardEvent) => {
      if (e.key.toLowerCase() === 'q' && isPieMenuOpen()) {
        e.preventDefault();
        commitPieMenu();
      }
    };
    window.addEventListener('keyup', onAppKeyUp);
    unlistenKeydown = () => {
      window.removeEventListener('keydown', onAppKeyDown);
      window.removeEventListener('keyup', onAppKeyUp);
    };
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
        {/* Workspace tabs replace the per-editor toggle buttons (Material /
            SOP / DOP). Picking a workspace sets the dock visibility; the
            Cmd+K command palette still has individual "Toggle SOP Graph"
            entries for when the user wants to deviate from a preset. */}
        <WorkspaceTabs onApply={applyWorkspace} />
        <button
          class="toolbar-button"
          classList={{ 'toolbar-button--active': frameLocked() }}
          type="button"
          title={
            frameLocked()
              ? 'Every frame mode: playback waits for each cook to finish and advances by 1/fps. Slower than realtime on heavy scenes but no frame is skipped.'
              : 'Async mode: playhead advances by wall-clock, slow cooks drop frames. UI never blocks.'
          }
          onClick={async () => {
            const next = !frameLocked();
            setFrameLocked(next);
            try {
              await api.timelineSetFrameLocked(next);
            } catch (e) {
              console.warn('set_frame_locked failed:', e);
              setFrameLocked(!next);
            }
          }}
        >
          Every Frame
        </button>
        <button
          class="toolbar-button"
          classList={{ 'toolbar-button--active': ptPreviewEnabled() }}
          type="button"
          title="Toggle the path-traced inset preview. Off → live viewport runs raster-only and skips BVH builds."
          onClick={async () => {
            const next = !ptPreviewEnabled();
            // Optimistic flip + native push. The native handler does a
            // synchronous compile_scene on OFF→ON, which can be a
            // visible pause on heavy scenes — the user already
            // clicked, so the wait is implicit consent.
            setPtPreviewEnabled(next);
            try {
              await api.setPtPreview(next);
              if (viewportRef) viewportRef.render();
            } catch (e) {
              console.warn('set_pt_preview failed:', e);
              setPtPreviewEnabled(!next);
            }
          }}
        >
          PT Preview
        </button>
        <button
          class="toolbar-button"
          type="button"
          onClick={() => setExportVideoOpen(true)}
        >
          Export Video…
        </button>
      </div>

      {/* Viewport-overlay sub-toolbar: rasterizer display toggles
          (points, edges, ground). Lives below the main toolbar so the
          controls are reachable in every workspace, but separate from
          the main toolbar's app-level chrome (dock toggles, video
          export, etc.). */}
      <RasterizerToolbar onSettingsChange={() => viewportRef?.render()} />

      {/* Workspace-specific quick-actions strip. Renders only for the
          Animation workspace (Set Key / Auto-Key shortcuts) now —
          render settings live in the dock slot via RenderPanel, not
          here. */}
      <WorkspaceBar
        animation={{ onSetKey: keySelectedActorPose }}
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
            onLightAdd={async (type) => {
              // Create a manual light via the new IPC, then refresh the
              // actor list and select the new row. The native side has
              // already queued a recompile + scene_changed broadcast, but
              // we still need to repopulate the local store because the
              // broadcast handler isn't always synchronous.
              try {
                const newId = await api.createLight(type);
                setActors(await api.getAllActors());
                setSelectedActorId(newId);
                if (viewportRef) viewportRef.render();
              } catch (e) {
                console.warn('[add light] failed:', e);
              }
            }}
            onActorVisibilityChange={(id, visible) =>
              setActors((prev) =>
                prev.map((a) => (a.id === id ? { ...a, visible } : a)),
              )
            }
            onActorDelete={async (id) => {
              const a = actors().find((x) => x.id === id);
              if (!a) {
                console.warn('[delete] actor not found in store:', id);
                return;
              }
              if (a.sop_node_uid == null) {
                console.warn('[delete] actor has no sop_node_uid — it was not produced by the cook (manually created or stale state); nothing to remove from the SOP graph', a);
                return;
              }
              // The SOP graph store can drift from the backend after some
              // edits (load_scene, undo, third-party broadcasts), and the
              // delete looks like a silent no-op. Refresh the store first
              // when the uid isn't in our local copy, then retry.
              let removed = removeNodeAnywhere(a.sop_node_uid);
              if (!removed) {
                console.info('[delete] node uid not in local SOP graph store; re-fetching from engine and retrying', a.sop_node_uid);
                await loadSopGraphFromEngine();
                removed = removeNodeAnywhere(a.sop_node_uid);
              }
              if (!removed) {
                console.warn('[delete] node uid still not found after re-fetch; backend state may be inconsistent', a.sop_node_uid);
                return;
              }
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
            {/* Cook badge — overlaid on the viewport so its
                appearance/disappearance never reflows the toolbar.
                Bottom-left corner, away from the PT inset preview in
                the top-right. */}
            <Show when={isCooking()}>
              <div class="viewport-cook-status" role="status" aria-live="polite">
                <span class="viewport-cook-spinner" aria-hidden="true" />
                <span>Cooking…</span>
              </div>
            </Show>
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

        {/* Single dock slot, shared between the SOP/VOP and Material
            editors AND the Render workspace's settings panel. They're
            mutually exclusive — opening one closes the other so the
            splitter handle doesn't have to know about multiple docks.
            The width state (sopDockW) belongs to the slot, not the
            editor, so the size persists across switches.

            When the active workspace is "render" the slot takes over
            unconditionally with the RenderPanel: that's the workspace's
            primary surface (viewport on the left, render settings on
            the right) so we don't want the previous SOP/Material/DOP
            open-state leaking through. */}
        <Show
          when={
            activeWorkspace() === 'render' ||
            sopEditorOpen() ||
            isVopEditorOpen() ||
            materialEditorOpen() ||
            dopEditorOpen()
          }
        >
          <Splitter
            orientation="vertical"
            onDrag={(dx) => setSopDockW((w) => clamp(w - dx, 380, 1400))}
          />
          {/* Dock body priority when in Render workspace: RenderPanel
              wins. Otherwise the same Material > DOP > SOP precedence
              as before. */}
          <Show
            when={activeWorkspace() === 'render'}
            fallback={
              <Show
                when={materialEditorOpen()}
                fallback={
                  <Show
                    when={dopEditorOpen() && !isVopEditorOpen()}
                    fallback={
                      <SopGraphEditor
                        onClose={async () => {
                          setSopEditorOpen(false);
                          try {
                            const fresh = await api.getAllActors();
                            setActors(fresh);
                          } catch (e) {
                            console.warn('actor refresh after SOP edit failed:', e);
                          }
                          if (viewportRef) viewportRef.render();
                        }}
                      />
                    }
                  >
                    <div class="sop-graph-dock">
                      <DopGraphPanel />
                    </div>
                  </Show>
                }
              >
                <MaterialGraphEditor
                  open={materialEditorOpen}
                  onClose={async () => {
                    setMaterialEditorOpen(false);
                    if (viewportRef) viewportRef.render();
                  }}
                />
              </Show>
            }
          >
            <RenderPanel
              maxSamples={maxSamples}
              setMaxSamples={(n) => {
                setMaxSamplesSignal(n);
                api.setMaxSamples(n).catch(() => {});
              }}
              maxBounces={maxBounces}
              setMaxBounces={(n) => {
                setMaxBouncesSignal(n);
                api.setMaxBounces(n).catch(() => {});
              }}
              resolution={renderResolution}
              setResolution={(w, h) => {
                setRenderResolutionSignal([w, h]);
                api.setPtRenderResolution(w, h).catch(() => {});
              }}
              onResetRender={() => {
                api.resetPtAccumulator().catch(() => {});
              }}
            />
          </Show>
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
          {/* Path-tracer settings (max samples, max bounces, resolution)
              live in the Render workspace's WorkspaceBar so the user
              sees them only when the PT is the active focus, not in
              every workspace's inspector. The rasterizer's display
              toggles (points/edges/ground) moved to a dedicated
              sub-toolbar below the main toolbar — always visible
              because they're viewport-display preferences, not
              workspace-specific. */}
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

      {/* Top-level overlay — mounted unconditionally and gated internally
          on isCommandPaletteOpen(). Anywhere in the app, Cmd+K toggles it. */}
      <CommandPalette />
      <GrabOverlay />
      <PieMenu />
    </div>
  );
};

export default App;
