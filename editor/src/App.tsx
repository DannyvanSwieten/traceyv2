import { Component, Show, createEffect, createSignal, onMount, onCleanup } from 'solid-js';
import * as api from './lib/api';
import { Viewport, ViewportHandle, CameraPosition } from './components/viewport/Viewport';
import { SceneHierarchy } from './components/scene-hierarchy/SceneHierarchy';
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
import { CurveEditor } from './components/curve-editor/CurveEditor';
import { animPanelMode } from './lib/anim_panel_mode';
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
  setParamAnywhere,
  undo,
} from './stores/sops';
import {
  actors,
  setActors,
  selectedActorId,
  setSelectedActorId,
  refreshActors,
  attachSceneChangedListener,
  keySelectedActorPose,
} from './stores/actors';
import {
  ptPreviewEnabled,
  frameLocked,
  maxSamples,
  maxBounces,
  renderResolution,
  ptBackend,
  initRenderSettings,
  setPtPreview,
  setFrameLocked,
  setMaxSamples,
  setMaxBounces,
  setRenderResolution,
  setPtBackend,
} from './stores/render_settings';
import { buildSubnetsFromGltf } from './lib/gltf_import';
import { buildSubnetsFromUsd } from './lib/usd_import';
import { fetchCatalog as fetchSopCatalog } from './lib/sop_graph';
import { isVopEditorOpen } from './stores/vops';
import { isMaterialEditorOpen, setMaterialEditorOpen } from './stores/materials';
import { CommandPalette } from './components/command-palette/CommandPalette';
import { ToastHost } from './components/toast/ToastHost';
import { showToast } from './lib/toasts';
import { toggleCommandPalette, registerCommands } from './lib/command_palette';
import { GrabOverlay } from './components/viewport-grab/GrabOverlay';
import { installAppInput } from './lib/app_input';
import { PieMenu } from './components/pie-menu/PieMenu';
import { WorkspaceTabs } from './components/workspaces/WorkspaceTabs';
import { WorkspaceBar } from './components/workspaces/WorkspaceBar';
import {
  WORKSPACES,
  WORKSPACE_LABELS,
  activeWorkspace,
  setActiveWorkspaceInternal,
  type WorkspaceName,
} from './lib/workspaces';
import { togglePlayPause, setRange as setTimelineRange } from './stores/timeline';
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
  const [renderingStill, setRenderingStill] = createSignal(false);
  // Non-null while an asset import is in flight → shows the loading overlay.
  const [loadingState, setLoadingState] = createSignal<
    { name: string; stage: string; done: number; total: number } | null
  >(null);
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
  let unlistenExportGltf: (() => void) | undefined;
  let unlistenExportGlb: (() => void) | undefined;
  let unlistenExportObj: (() => void) | undefined;

  // Import is just "remember this file" — adds an entry to the asset browser
  // and stops. Pulling geometry into the scene is a separate action the user
  // triggers from the asset row (handleLoadAsset). This split lets the user
  // line up several files first, then decide which to actually load (and
  // re-load the same asset multiple times for repeated instances).
  const handleImport = async () => {
    try {
      const selected = await api.openFileDialog('Import 3D Asset', [
        { description: '3D Assets', extensions: ['gltf', 'glb', 'usd', 'usda', 'usdc', 'usdz'] },
        { description: 'glTF', extensions: ['gltf', 'glb'] },
        { description: 'USD', extensions: ['usd', 'usda', 'usdc', 'usdz'] },
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
    const name = asset.path.split(/[/\\]/).pop() ?? asset.path;
    const ext = asset.path.split('.').pop()?.toLowerCase() ?? '';
    const isUsd = ext === 'usd' || ext === 'usda' || ext === 'usdc' || ext === 'usdz';
    // Generous: a very large asset can take minutes to cook + build BLASes.
    const LOAD_TIMEOUT_MS = 600000;

    // Resolve when any of `events` fires (or on timeout → false). Used to wait
    // on the async cook + the async USD-extras worker without blocking.
    const waitForAny = (events: string[], timeoutMs: number) =>
      new Promise<boolean>((resolve) => {
        let done = false;
        const offs: Array<() => void> = [];
        const finish = (ok: boolean) => {
          if (done) return;
          done = true;
          clearTimeout(timer);
          offs.forEach((o) => o());
          resolve(ok);
        };
        const timer = setTimeout(() => finish(false), timeoutMs);
        for (const e of events) offs.push(api.listen(e, () => finish(true)));
      });

    // Live progress → overlay. Stays subscribed for the whole load.
    const offProgress = api.listen('usd_import_progress', (m) => {
      setLoadingState((s) =>
        s ? {
          ...s,
          stage: typeof m.stage === 'string' ? m.stage : s.stage,
          done: typeof m.done === 'number' ? m.done : 0,
          total: typeof m.total === 'number' ? m.total : 0,
        } : s);
    });

    setLoadingState({ name, stage: 'Reading…', done: 0, total: 0 });
    // The native Metal viewport is an NSView ON TOP of the WebView, so it would
    // cover the loading overlay. Hide it for the duration of the load.
    api.setViewportVisible(false).catch(() => {});
    try {
      const beforeCount = actors().length;

      // Build the procedural subnet tree (peek + construct). glTF + USD share
      // the builder; only the import SOP kind differs.
      let subnets;
      let usdTimeline: { fps: number; frameStart: number; frameEnd: number } | undefined;
      if (isUsd) {
        const res = await buildSubnetsFromUsd(asset.path);
        subnets = res.subnets;
        usdTimeline = res.timeline;
      } else {
        subnets = await buildSubnetsFromGltf(asset.path);
      }
      for (const s of subnets) addNode(s);

      if (usdTimeline) {
        try {
          await setTimelineRange(usdTimeline.fps, usdTimeline.frameStart, usdTimeline.frameEnd);
        } catch (e) {
          console.warn('USD timeline range set failed:', e);
        }
      }

      // Geometry cook (async on the engine's worker). Only wait when we
      // actually pushed geometry subnets — an all-instanced USD has none.
      let cookedOk = true;
      if (subnets.length > 0) {
        setLoadingState((s) => (s ? { ...s, stage: 'Building geometry…' } : s));
        await flushSopGraph();
        if (viewportRef) viewportRef.render();
        cookedOk = await waitForAny(['scene_changed'], LOAD_TIMEOUT_MS);
        if (!cookedOk) {
          console.error('Load timed out — no scene_changed for', asset.path);
          showToast('Load timed out — the engine did not finish cooking in time.', {
            kind: 'error', detail: asset.path,
          });
          return;
        }
      }

      // USD lights / camera / instanced geometry — imported on a native worker
      // thread (no main-thread block), so the overlay stays live + animated.
      if (isUsd) {
        setLoadingState((s) => (s ? { ...s, stage: 'Importing lights & instances…' } : s));
        api.importUsdStage(asset.path).catch((e) =>
          console.warn('USD extras import dispatch failed:', e));
        await waitForAny(['usd_import_done', 'usd_import_error'], LOAD_TIMEOUT_MS);
      }

      await refreshActors('refresh after load');
      if (actors().length <= beforeCount) {
        console.warn('Load completed but actor count did not grow:', asset.path);
        showToast('Loaded the file but no actors appeared — check the editor console.', {
          kind: 'error', detail: asset.path,
        });
      }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      console.error('Asset import failed for', asset.path, ':', msg);
      const assetId = asset.id;
      showToast(`Failed to load asset: ${msg}`, {
        kind: 'error',
        detail: asset.path,
        action: assetId
          ? { label: 'Remove from browser', run: () => removeAsset(assetId) }
          : undefined,
      });
    } finally {
      offProgress();
      setLoadingState(null);
      api.setViewportVisible(true).catch(() => {});
      if (viewportRef) viewportRef.render();
    }
  };

  const handleTransformChange = async (actorId: number, transform: Transform) => {
    // Optimistic local update so the inspector doesn't lag the edit.
    setActors((prev) =>
      prev.map((actor) =>
        actor.id === actorId ? { ...actor, transform } : actor
      )
    );

    // Recompile scene and re-render. On failure, re-pull the actor list so
    // the UI snaps back to the engine's truth instead of showing a
    // transform the engine never applied.
    try {
      await api.compileScene();
      if (viewportRef) {
        viewportRef.render();
      }
    } catch (error) {
      console.error('Failed to recompile scene after transform change:', error);
      await refreshActors('rollback after failed transform change');
    }
  };

  let unlistenOpenScene: (() => void) | undefined;
  let unlistenSaveScene: (() => void) | undefined;
  let unlistenSaveSceneAs: (() => void) | undefined;
  let unlistenSceneChanged: (() => void) | undefined;
  let uninstallAppInput: (() => void) | undefined;

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
      await refreshActors('refresh after open scene');
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

  // Export the cooked scene geometry to glTF/GLB/OBJ. Prompts for a path with
  // the matching extension, then hands the live scene to the native exporter.
  const handleExportGeometry = async (format: api.GeometryFormat) => {
    const ext = format;
    try {
      const selected = await api.saveFileDialog(
        `Export Geometry (${format.toUpperCase()})`,
        `scene.${ext}`,
        [{ description: `${format.toUpperCase()} geometry`, extensions: [ext] }],
      );
      if (!selected) return;
      await api.exportScene(selected, format);
      showToast(`Exported ${format.toUpperCase()}`, { kind: 'success', detail: selected });
    } catch (e) {
      console.error('Export geometry failed:', e);
      showToast('Export failed', { kind: 'error', detail: String(e) });
    }
  };

  // Render a single still frame to PNG/EXR at the Render panel's Output
  // resolution + Samples. Prompts for a path (format inferred from extension),
  // kicks off the native render_still worker, and toasts on done/error.
  const handleRenderStill = async () => {
    if (renderingStill()) return;
    let selected: string | null = null;
    try {
      selected = await api.saveFileDialog('Render Still', 'render.png', [
        { description: 'PNG image', extensions: ['png'] },
        { description: 'OpenEXR (linear + AOVs)', extensions: ['exr'] },
      ]);
    } catch (e) {
      console.error('Render still: save dialog failed', e);
      return;
    }
    if (!selected) return;
    const path = selected;
    const ext = path.split('.').pop()?.toLowerCase();
    const format: 'png' | 'exr' = ext === 'exr' ? 'exr' : 'png';
    const [w, h] = renderResolution();

    setRenderingStill(true);
    let offDone = () => {};
    let offErr = () => {};
    const cleanup = () => { offDone(); offErr(); setRenderingStill(false); };
    offDone = api.listen('render_still_done', (msg) => {
      cleanup();
      showToast(msg.cancelled === true ? 'Render cancelled' : 'Rendered still', {
        kind: msg.cancelled === true ? 'info' : 'success',
        detail: path,
      });
    });
    offErr = api.listen('render_still_error', (msg) => {
      cleanup();
      showToast('Render failed', {
        kind: 'error',
        detail: typeof msg.message === 'string' ? msg.message : 'unknown error',
      });
    });

    try {
      await api.renderStill({
        path,
        width: w,
        height: h,
        samples: maxSamples(),
        max_bounces: maxBounces(),
        format,
        denoise: format === 'exr',
      });
    } catch (e) {
      cleanup();
      showToast('Render failed', { kind: 'error', detail: String(e) });
    }
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
    // Global input (pointer tracking, modal grab, pie menu, keyboard
    // shortcuts) lives in lib/app_input.ts.
    uninstallAppInput = installAppInput({
      renderViewport: () => viewportRef?.render(),
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
      { id: 'file.exportGltf', label: 'Export Geometry: glTF…',
        group: 'File',
        keywords: 'export geometry gltf mesh interchange blender',
        run: () => handleExportGeometry('gltf') },
      { id: 'file.exportGlb', label: 'Export Geometry: GLB…',
        group: 'File',
        keywords: 'export geometry glb mesh binary interchange blender unreal',
        run: () => handleExportGeometry('glb') },
      { id: 'file.exportObj', label: 'Export Geometry: OBJ…',
        group: 'File',
        keywords: 'export geometry obj wavefront mesh mtl',
        run: () => handleExportGeometry('obj') },
      { id: 'edit.undo', label: 'Undo',
        group: 'Edit', hint: '⌘Z',
        run: () => undo().then((ok) => {
          if (ok) void refreshActors('refresh after undo');
        }) },
      { id: 'edit.redo', label: 'Redo',
        group: 'Edit', hint: '⇧⌘Z',
        run: () => redo().then((ok) => {
          if (ok) void refreshActors('refresh after redo');
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

    // Seed the render-workspace mirrors (PT preview, frame lock, samples,
    // bounces, resolution) with what the engine actually has.
    initRenderSettings();

    unlistenImport = api.listen('menu-import', () => handleImport());
    unlistenExport = api.listen('menu-export', () => {
      // File → Export… (Cmd+E) opens the video export dialog.
      setExportVideoOpen(true);
    });
    unlistenExportGltf = api.listen('menu-export-gltf', () => handleExportGeometry('gltf'));
    unlistenExportGlb = api.listen('menu-export-glb', () => handleExportGeometry('glb'));
    unlistenExportObj = api.listen('menu-export-obj', () => handleExportGeometry('obj'));
    unlistenOpenScene = api.listen('menu-open-scene', () => handleOpenScene());
    unlistenSaveScene = api.listen('menu-save-scene', () => handleSaveScene());
    unlistenSaveSceneAs = api.listen('menu-save-scene-as', () => handleSaveSceneAs());

    // Keep the scene hierarchy live across cooks (rAF-coalesced
    // scene_changed → refreshActors, see stores/actors.ts).
    unlistenSceneChanged = attachSceneChangedListener();
    // Engine-side cook of the default SOP graph completes during native
    // startup — typically before this JS bundle has had a chance to
    // attach the `scene_changed` listener above, so the initial actor
    // emission is missed. Pull the current list once at mount so the
    // hierarchy panel renders the seeded actors instead of staying empty
    // until the user pokes the graph.
    void refreshActors('initial fetch');

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
  });

  onCleanup(() => {
    unlistenImport?.();
    unlistenExport?.();
    unlistenExportGltf?.();
    unlistenExportGlb?.();
    unlistenExportObj?.();
    unlistenOpenScene?.();
    unlistenSaveScene?.();
    unlistenSaveSceneAs?.();
    unlistenSceneChanged?.();
    uninstallAppInput?.();
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
          onClick={() => void setFrameLocked(!frameLocked())}
        >
          Every Frame
        </button>
        <button
          class="toolbar-button"
          classList={{ 'toolbar-button--active': ptPreviewEnabled() }}
          type="button"
          title="Toggle the path-traced inset preview. Off → live viewport runs raster-only and skips BVH builds."
          onClick={async () => {
            const ok = await setPtPreview(!ptPreviewEnabled());
            if (ok && viewportRef) viewportRef.render();
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

      {/* Asset-load progress overlay. Shown while an import is in flight; the
          heavy native work runs on worker threads so this stays animated. */}
      <Show when={loadingState()}>
        {(ls) => (
          <div class="load-overlay">
            <div class="load-card">
              <div class="load-title">Loading {ls().name}</div>
              <div class="load-stage">{ls().stage}</div>
              <div class="load-bar"><div class="load-bar-fill" /></div>
              <Show when={ls().total > 0}>
                <div class="load-count">{ls().done} / {ls().total}</div>
              </Show>
            </div>
          </div>
        )}
      </Show>

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
                await refreshActors('refresh after add light');
                setSelectedActorId(newId);
                if (viewportRef) viewportRef.render();
              } catch (e) {
                console.warn('[add light] failed:', e);
              }
            }}
            onActorRename={async (id, name) => {
              const a = actors().find((x) => x.id === id);
              if (!a) return;
              if (a.sop_node_uid == null) {
                // Manually-created actors (lights via create_light) aren't
                // backed by a SOP node; there's no rename IPC for them yet.
                showToast('This actor has no SOP node — renaming is not supported for it yet.', {
                  kind: 'info',
                });
                return;
              }
              // The emit node's `name` string param is the actor name. Like
              // delete, the local SOP store can drift from the backend, so
              // refresh-and-retry when the uid isn't found.
              let ok = setParamAnywhere(a.sop_node_uid, 'name', { type: 'string', value: name });
              if (!ok) {
                await loadSopGraphFromEngine();
                ok = setParamAnywhere(a.sop_node_uid, 'name', { type: 'string', value: name });
              }
              if (!ok) {
                console.warn('[rename] SOP node not found for actor', a);
                return;
              }
              // Optimistic local rename so the row updates before the cook.
              setActors((prev) =>
                prev.map((x) => (x.id === id ? { ...x, name } : x)),
              );
              await flushSopGraph();
              await refreshActors('refresh after rename');
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
                // Manually-created actor (e.g. a light from "+ Add Light") —
                // not produced by the cook, so there's no SOP node to remove.
                // Delete it straight from the scene instead.
                try {
                  await api.deleteActor(id);
                  if (selectedActorId() === id) setSelectedActorId(null);
                  await refreshActors('refresh after delete actor');
                  if (viewportRef) viewportRef.render();
                } catch (e) {
                  console.warn('[delete] deleteActor failed:', e);
                }
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
              await refreshActors('refresh after delete');
              if (viewportRef) viewportRef.render();
            }}
            onActorReorder={async (sourceId, targetId, mode) => {
              const src = actors().find((a) => a.id === sourceId);
              const tgt = actors().find((a) => a.id === targetId);
              if (!src || !tgt || src.sop_node_uid == null || tgt.sop_node_uid == null) return;
              if (!moveNodeAnywhere(src.sop_node_uid, tgt.sop_node_uid, mode)) return;
              await flushSopGraph();
              await refreshActors('refresh after reorder');
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
                          await refreshActors('refresh after SOP edit');
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
              setMaxSamples={setMaxSamples}
              maxBounces={maxBounces}
              setMaxBounces={setMaxBounces}
              resolution={renderResolution}
              setResolution={setRenderResolution}
              ptBackend={ptBackend}
              setPtBackend={setPtBackend}
              onResetRender={() => {
                api.resetPtAccumulator().catch(() => {});
              }}
              onRenderStill={handleRenderStill}
              rendering={renderingStill}
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
          {/* Camera controls only when nothing is selected — when an actor is
              selected the panel shows that actor's properties instead. */}
          <Show when={selectedActorId() === null}>
            <CameraControls
              position={cameraPosition}
              onPositionChange={setCameraPosition}
            />
          </Show>
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
        <Show when={animPanelMode() === 'curves'} fallback={<Dopesheet />}>
          <CurveEditor />
        </Show>
      </div>

      <Playbar />

      {/* Top-level overlay — mounted unconditionally and gated internally
          on isCommandPaletteOpen(). Anywhere in the app, Cmd+K toggles it. */}
      <CommandPalette />
      <GrabOverlay />
      <PieMenu />
      <ToastHost />
    </div>
  );
};

export default App;
