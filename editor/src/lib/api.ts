// Typed wrappers around the native bridge transport. Every host-side command
// has a wrapper here — components import these instead of calling invoke()
// directly. This is the single seam where the JSON wire format is bound to TS.

import { WebViewTransport, Transport } from './transport';

export interface Vec2 {
  x: number;
  y: number;
}

export interface Vec3 {
  x: number;
  y: number;
  z: number;
}

export interface Quat {
  w: number;
  x: number;
  y: number;
  z: number;
}

export interface Transform {
  position: Vec3;
  rotation: Quat;
  scale: Vec3;
}

export interface Camera {
  position: Vec3;
  rotation: Quat;
  fov: number;
  near_plane: number;
  far_plane: number;
  aspect_ratio: number;
  // Thin-lens depth of field (R4). aperture=0 → pinhole (no DOF). Optional so
  // scenes/cameras saved before R4 still parse.
  aperture?: number;
  focal_distance?: number;
  // Motion-blur shutter as a fraction of the frame interval (0 = off). Applied
  // on sequence/EXR export — moving objects blur over [t, t+shutter/fps].
  shutter?: number;
}

export interface Actor {
  id: number;
  name: string;
  transform: Transform;
  children: number[];
  // True when the actor has a library graph attached (vs. the passthrough
  // default). Backend doesn't ship the actual JSON — the frontend only needs
  // to know "assigned vs not" + the library entry list to render the picker.
  material_assigned?: boolean;
  // uid of the object_output SOP node that emitted this actor (null when the
  // actor wasn't produced by the cook). Used to target keyframe edits at the
  // matching SOP parameter without an extra round-trip.
  sop_node_uid?: number | null;
  // Display flag toggled via the hierarchy's eye icon. Hidden actors are
  // skipped by the SceneCompiler, so they appear in neither the path tracer
  // nor the rasterizer overlay. Defaults to true server-side.
  visible?: boolean;
  // Scene-level light component. Present either because the actor was
  // emitted by a `light` SOP terminal OR because the user created it via
  // the hierarchy's "+ Add Light" menu. The hierarchy panel swaps the
  // actor's icon for 💡 and the inspector renders the type-conditional
  // light rows instead of the geometry / material section.
  light?: {
    // Matches tracey::LightType on the engine side.
    //   0 = Point, 1 = Distant ("Sun"), 2 = Dome, 3 = Area.
    type: number;
    color: Vec3;
    intensity: number;
    // Dome procedural-sky gradient. Always present in the payload — the
    // inspector only renders these rows when type === Dome.
    sky_color: Vec3;
    horizon_color: Vec3;
    ground_color: Vec3;
    // Optional HDRI override for Dome. Empty string = procedural gradient.
    hdri_path: string;
    // Area-light rectangle extent (XY in the actor's local plane).
    size: Vec2;
    // Point-light soft radius for 1/(d² + r²) attenuation. 0 = hard point.
    radius: number;
  } | null;
}

export interface InstanceInfo {
  object_ref: string;
  shader_id: string;
  has_local_transform: boolean;
  local_transform: Transform | null;
}

export interface MeshInfo {
  name: string;
  vertex_count: number;
  triangle_count: number;
  has_indices: boolean;
  has_normals: boolean;
  has_uvs: boolean;
}

export interface TextureInfo {
  id: string;
  width: number;
  height: number;
  channels: number;
  mime_type: string;
}

export interface RenderResult {
  width: number;
  height: number;
  sample_count: number;
  render_time_ms: number;
}

export interface FileFilter {
  description: string;
  extensions: string[];
}

interface Envelope<T> {
  ok: boolean;
  data?: T;
  error?: string;
}

const transport: Transport = new WebViewTransport();

// Exported so feature modules (sop_graph.ts) can reuse the same envelope-
// unwrapping behaviour without re-implementing it. Most call sites should
// prefer the typed wrappers further down this file; `send` is the escape
// hatch for commands not yet wrapped.
export async function send<T>(cmd: string, args: Record<string, unknown> = {}): Promise<T> {
  const body = JSON.stringify({ cmd, ...args });
  const responseJson = await transport.send(body);
  const env: Envelope<T> = JSON.parse(responseJson);
  if (!env.ok) throw new Error(env.error ?? `Command failed: ${cmd}`);
  return env.data as T;
}

// ─── Broadcast events (menu, etc.) ──────────────────────────────────────────

type EventListener = (payload: Record<string, unknown>) => void;
const listeners = new Map<string, Set<EventListener>>();

transport.onBroadcast((json: string) => {
  try {
    const msg = JSON.parse(json);
    const event = msg.event as string | undefined;
    if (!event) return;
    const set = listeners.get(event);
    if (!set) return;
    for (const fn of set) fn(msg);
  } catch (e) {
    console.error('Bad broadcast:', e, json);
  }
});

export function listen(event: string, fn: EventListener): () => void {
  let set = listeners.get(event);
  if (!set) {
    set = new Set();
    listeners.set(event, set);
  }
  set.add(fn);
  return () => set!.delete(fn);
}

// ─── Scene management ──────────────────────────────────────────────────────

export const createActor = (name: string) => send<number>('create_actor', { name });
export const getAllActors = () => send<Actor[]>('get_all_actors');
export const getActor = (actorId: number) =>
  send<Actor | null>('get_actor', { actor_id: actorId });
export const setActorTransform = (actorId: number, transform: Transform) =>
  send<boolean>('set_actor_transform', { actor_id: actorId, transform });

// Toggle an actor's visibility in the live scene. The flag is also remembered
// against the source SOP node so it survives a re-cook.
export const setActorVisible = (actorId: number, visible: boolean) =>
  send<boolean>('set_actor_visible', { actor_id: actorId, visible });

// Rotation edits go through this sibling IPC (rather than set_actor_transform)
// because the SOP-side storage is per-axis euler-degrees. The server converts
// to quaternion for the live actor and writes the euler back to the source
// node's `rotate_euler_deg` param so the edit survives the next cook.
export const setActorRotationEuler = (actorId: number, eulerDeg: Vec3) =>
  send<boolean>('set_actor_rotation_euler', { actor_id: actorId, euler_deg: eulerDeg });
export const setCamera = (camera: Camera) => send<null>('set_camera', { camera });
export const getCamera = () => send<Camera>('get_camera');
// Push the active selection to the server so the orbital viewport camera can
// pivot around it. Pass null to clear.
export const selectActor = (actorId: number | null) =>
  send<null>('select_actor', { actor_id: actorId });

// Scene-level light lifecycle. Lights are first-class entities authored
// directly in the editor (no SOP node involved) — they live alongside the
// camera in the scene tree and round-trip through save/load via the
// existing actor JSON.
export type LightKind = 'dome' | 'sun' | 'point' | 'area';
export const createLight = (type: LightKind, name?: string) =>
  send<number>('create_light', name ? { type, name } : { type });

// Remove a manually-created actor (lights from createLight) directly from the
// scene. SOP-emitted actors are removed via SOP-node deletion instead.
export const deleteActor = (actorId: number) =>
  send<boolean>('delete_actor', { actor_id: actorId });

// Patch handler: send only the fields you changed. The native side reads
// each key with a default, so missing keys leave their stored value alone.
// Triggers a re-compile so both rasterizer + path tracer see the new
// light parameters on the next render.
export interface LightParamPatch {
  type?: number;
  color?: Vec3;
  intensity?: number;
  sky_color?: Vec3;
  horizon_color?: Vec3;
  ground_color?: Vec3;
  hdri_path?: string;
  size?: Vec2;
  radius?: number;
}
export const setLightParams = (actorId: number, patch: LightParamPatch) =>
  send<boolean>('set_light_params', { actor_id: actorId, ...patch });

// Import the non-geometry half of a USD stage — its UsdLux lights (as native
// light actors, full type/color/intensity/radius/size/hdri fidelity) and its
// camera (framing the native orbital view). The meshes come in separately via
// the procedural usd_import subnet path. Returns how many lights landed and
// whether the camera was applied.
export const importUsdStage = (
  path: string,
  opts?: { lights?: boolean; camera?: boolean; instances?: boolean }
) =>
  send<{ lights: number; camera: boolean; instances: number }>('import_usd_stage', {
    path,
    lights: opts?.lights ?? true,
    camera: opts?.camera ?? true,
    instances: opts?.instances ?? true,
  });

// Tell the native side that a JS modal grab (G/R/S) is active. While
// true the engine suppresses camera orbit/pan/dolly and instead
// broadcasts `viewport_pointer` events so the JS grab can drive the
// transform. The WebView never sees pointer events over the Metal layer,
// so without this the grab silently does nothing.
export const setViewportGrabActive = (active: boolean) =>
  send<null>('set_viewport_grab_active', { value: active });

// Translate-gizmo overlay. Visibility + anchor go through separate IPCs
// so the editor can hide it during a tab-switch without re-sending the
// position. `length` controls the axis-line length in world units;
// frontend picks a value based on camera distance for screen-stable size.
export const setGizmoVisible = (visible: boolean) =>
  send<null>('set_gizmo_visible', { value: visible });
export const setGizmoAnchor = (x: number, y: number, z: number, length: number) =>
  send<null>('set_gizmo_anchor', { x, y, z, length });

// Snap the orbital camera to a named preset. The server keeps the current
// pivot + zoom distance so pressing Top while focused on an actor reframes
// that actor from above (rather than teleporting to world origin).
export type CameraView =
  | 'top'
  | 'bottom'
  | 'front'
  | 'back'
  | 'left'
  | 'right'
  | 'persp';
export const setCameraView = (view: CameraView) =>
  send<null>('set_camera_view', { view });

// ─── Scene resource queries ────────────────────────────────────────────────

export const getActorInstances = (actorId: number) =>
  send<InstanceInfo[]>('get_actor_instances', { actor_id: actorId });
export const getMeshNames = () => send<string[]>('get_mesh_names');
export const getMeshInfo = (name: string) => send<MeshInfo>('get_mesh_info', { name });
export const getAllMeshes = () => send<MeshInfo[]>('get_all_meshes');
export const getTextureIds = () => send<string[]>('get_texture_ids');
export const getTextureInfo = (id: string) => send<TextureInfo>('get_texture_info', { id });
export const getAllTextures = () => send<TextureInfo[]>('get_all_textures');

// ─── Rendering ─────────────────────────────────────────────────────────────

export const renderFrame = (camera: Camera, clearAccumulation: boolean) =>
  send<RenderResult>('render_frame', { camera, clear_accumulation: clearAccumulation });

// Returns RGBA pixels for the most-recent render as a Uint8Array.
export async function getRenderPixels(): Promise<Uint8Array> {
  const b64 = await send<string>('get_render_pixels');
  return decodeBase64(b64);
}

export const compileScene = () => send<null>('compile_scene');
export const getViewportResolution = () =>
  send<[number, number]>('get_viewport_resolution');
export const setViewportResolution = (width: number, height: number) =>
  send<null>('set_viewport_resolution', { width, height });
// Path-tracer accumulation cap. The editor renders one sample per tick and
// stops once `max_samples` is reached, leaving the converged image on screen.
// Camera / scene / settings changes reset the accumulator automatically.
export const getMaxSamples = () => send<number>('get_max_samples');
export const setMaxSamples = (samples: number) =>
  send<null>('set_max_samples', { samples });
// Current accumulated sample count; useful for showing progress in the UI.
export const getCurrentSamples = () => send<number>('get_current_samples');
export const getMaxBounces = () => send<number>('get_max_bounces');
export const setMaxBounces = (bounces: number) =>
  send<null>('set_max_bounces', { bounces });
// Path-tracer backend: 'auto' | 'metal' (GPU) | 'cpu'. Switching recreates the
// path tracer and restarts accumulation.
export type PtBackend = 'auto' | 'metal' | 'cpu';
export const getPtBackend = () => send<string>('get_pt_backend');
export const setPtBackend = (backend: PtBackend) =>
  send<null>('set_pt_backend', { backend });

// Toggle the rasterizer's antialiased point-sprite overlay (drawn on top of
// the triangle pass in the main view). PiP path-tracer view is unaffected.
export const getShowPoints = () => send<boolean>('get_show_points');
export const setShowPoints = (value: boolean) =>
  send<null>('set_show_points', { value });

// Toggle the rasterizer's wireframe overlay (triangle edges drawn over the
// filled triangles using POLYGON_MODE_LINE). PiP path-tracer view is unaffected.
export const getShowEdges = () => send<boolean>('get_show_edges');
export const setShowEdges = (value: boolean) =>
  send<null>('set_show_edges', { value });

// Toggle the rasterizer's reference ground-grid overlay (anti-aliased grid
// on the y=0 plane, alpha-blended over the scene). PiP path-tracer view is
// unaffected — the ground is a viewport reference, not real geometry.
export const getShowGround = () => send<boolean>('get_show_ground');
export const setShowGround = (value: boolean) =>
  send<null>('set_show_ground', { value });

// Rasterizer viewport background. Returned/passed as [r,g,b,a] in linear
// [0,1]. The set command accepts either 3 or 4 components — 3-element
// payloads default alpha to 1.0 so the frontend's color picker can stay
// RGB without owning the alpha channel.
export const getBackgroundColor = () =>
  send<[number, number, number, number]>('get_background_color');
export const setBackgroundColor = (
  rgba: [number, number, number] | [number, number, number, number],
) => send<null>('set_background_color', { value: rgba });

// Toggle the path-traced inset preview. Off by default (the live viewport
// runs raster-only, which also lets the engine skip BLAS/TLAS construction
// during compile_scene). Turning it on triggers a synchronous engine
// recompile so the BVH is ready by the next render_tick.
export const getPtPreview = () => send<boolean>('get_pt_preview');
export const setPtPreview = (value: boolean) =>
  send<null>('set_pt_preview', { value });

// When true, the path tracer takes the entire viewport instead of the
// top-right PiP inset. The Render workspace flips this on entry; other
// workspaces leave it off so the PiP composite returns. Requires
// setPtPreview(true) to have any visible effect — the PT pass still has
// to actually run.
export const getPtFullscreen = () => send<boolean>('get_pt_fullscreen');
export const setPtFullscreen = (value: boolean) =>
  send<null>('set_pt_fullscreen', { value });

// Clear the path tracer's accumulator on the next tick. Equivalent to
// camera-moved invalidation but explicit — for "Reset Render" buttons.
export const resetPtAccumulator = () =>
  send<null>('reset_pt_accumulator');

// Optional fixed render resolution for the path tracer in fullscreen
// (Render workspace) mode. Width=0, Height=0 returns to "match viewport".
export interface RenderResolution { width: number; height: number; }
export const getPtRenderResolution = () =>
  send<RenderResolution>('get_pt_render_resolution');
export const setPtRenderResolution = (width: number, height: number) =>
  send<null>('set_pt_render_resolution', { width, height });

// ─── Material graphs ───────────────────────────────────────────────────────

// Returns the active material graph as a JSON string. The schema lives in
// editor/src/lib/material_graph.ts (and is mirrored on the C++ side).
export const getMaterialGraph = () => send<string>('get_material_graph');

// Replace the active material graph. `graphJson` must be a JSON string
// matching the ShaderGraph schema. Triggers recompile + reupload + accumulator
// invalidation server-side.
export const setMaterialGraph = (graphJson: string) =>
  send<null>('set_material_graph', { graph: graphJson });

// Mutate one parameter slot in the active material's parameter pool. Animation
// path: no recompile, just an SSBO write.
export const setMaterialParameter = (
  programId: number,
  paramIdx: number,
  value: [number, number, number, number]
) =>
  send<null>('set_material_parameter', {
    program_id: programId,
    param_idx: paramIdx,
    value,
  });

// ─── Material library (project + global persistent graphs) ─────────────────
// Each material lives as one .json per name in either the open project's
// `materials/` subfolder ("project" scope — moves with the project file)
// or the user-wide global library ("global" scope — palette shared across
// projects). Names are sanitized server-side (alphanumerics, ' ', '_', '-').
// When loading by name, project entries shadow global ones of the same name.

export type MaterialScope = 'project' | 'global';

export interface MaterialLibraryEntry {
  name: string;
  scope: MaterialScope;
}

export const listMaterialLibrary = () =>
  send<MaterialLibraryEntry[]>('list_material_library');

// `scope` defaults to "project" when a project is open, otherwise "global".
// Passing an explicit scope is required when you want to write a project-
// scoped material before save_scene has run (no project dir set yet
// → server returns an error).
export const saveMaterialGraphAs = (
  name: string,
  graphJson: string,
  scope?: MaterialScope,
) =>
  send<null>(
    'save_material_graph_as',
    scope ? { name, graph: graphJson, scope } : { name, graph: graphJson },
  );

// Without an explicit scope the server resolves project-first, falling back
// to global — matches the cook-side `resolve_material_path` precedence.
export const loadMaterialGraphFromLibrary = (
  name: string,
  scope?: MaterialScope,
) =>
  send<string>(
    'load_material_graph_from_library',
    scope ? { name, scope } : { name },
  );

export const deleteMaterialGraphFromLibrary = (
  name: string,
  scope?: MaterialScope,
) =>
  send<null>(
    'delete_material_graph_from_library',
    scope ? { name, scope } : { name },
  );

// Project folder. `get_project_dir` returns "" when no project is open
// (legacy single-file flow before the first save_scene / load_scene).
// `set_project_dir` with an empty path clears the binding.
export const getProjectDir = () => send<string>('get_project_dir');
export const setProjectDir = (path: string) =>
  send<string>('set_project_dir', { path });

// Consolidate all external references (gltf_import paths + globally-
// scoped materials) into the project folder so it's portable to another
// machine. Returns a summary of what was copied + any warnings (missing
// sources, copy failures, etc.). Requires a project folder to be open.
export interface ConsolidateResult {
  project_dir: string;
  copied_assets: Array<{ from: string; to: string }>;
  copied_materials: string[];
  warnings: string[];
}
export const consolidateProject = () =>
  send<ConsolidateResult>('consolidate_project');

// Assign a library graph to an actor. Empty `libraryName` clears the
// assignment back to the default passthrough. Triggers a scene recompile
// server-side so the new MaterialProgramBuffer takes effect on next render.
export const setActorMaterial = (actorId: number, libraryName: string) =>
  send<null>('set_actor_material', { actor_id: actorId, library_name: libraryName });

// ─── IO ────────────────────────────────────────────────────────────────────

export const saveScene = (path: string) => send<null>('save_scene', { path });
export const loadScene = (path: string) => send<null>('load_scene', { path });
export const exportImage = (path: string, format: string) =>
  send<null>('export_image', { path, format });

// Export the live cooked scene geometry to a standard interchange format.
// 'gltf'/'glb' round-trip through the importer (meshes + per-instance
// transforms + PBR material factors incl. transmission/ior/emission); 'obj'
// writes baked world-space triangles + a sidecar .mtl.
export type GeometryFormat = 'gltf' | 'glb' | 'obj';
export const exportScene = (path: string, format: GeometryFormat) =>
  send<null>('export_scene', { path, format });

// ─── Viewport surface (native overlay) ─────────────────────────────────────

export const setViewportRect = (x: number, y: number, width: number, height: number) =>
  send<null>('set_viewport_rect', { x, y, width, height });
export const setViewportVisible = (visible: boolean) =>
  send<null>('set_viewport_visible', { visible });

// ─── Native dialogs ────────────────────────────────────────────────────────

export const openFileDialog = (title: string, filters?: FileFilter[]) =>
  send<string | null>('open_file_dialog', { title, filters: filters ?? [] });
export const saveFileDialog = (
  title: string,
  defaultName: string,
  filters?: FileFilter[]
) =>
  send<string | null>('save_file_dialog', {
    title,
    default_name: defaultName,
    filters: filters ?? [],
  });
export const openFolderDialog = (title: string) =>
  send<string | null>('open_folder_dialog', { title });

// ─── glTF peek (structural import helper) ──────────────────────────────────
// Returns the node hierarchy of a glTF file in the shape the importer needs
// to build a recursive subnet tree: one entry per glTF node with local TRS
// (rotation pre-converted to ZYX euler-degrees so it lands in the SOP
// `rotate_euler_deg` param with no client-side conversion), the
// SceneObject name to feed into `gltf_import`'s `mesh_name`, and recursive
// children. Reads structural metadata only — no buffers, accessors or
// images — so even large files peek quickly.

// One animated world-transform sample (USD import). `t` is in the stage's
// time-code units; the importer converts to seconds via the peek's fps.
export interface TrsSample {
  t: number;
  translate: [number, number, number];
  rotate_euler_deg: [number, number, number];
  scale: [number, number, number];
}

export interface GltfHierarchyNode {
  name: string;
  translate: [number, number, number];
  rotate_euler_deg: [number, number, number];
  scale: [number, number, number];
  // SceneObject names this node's mesh expands into, one per primitive of
  // the referenced mesh. Empty when the node is a transform-only container
  // (no mesh). Multi-primitive meshes return multiple names so the
  // importer can create one `gltf_import` per primitive instead of
  // dropping all but the first.
  mesh_names: string[];
  children: GltfHierarchyNode[];
  // USD only: present + non-empty when the prim's world transform is
  // animated. The importer bakes these into keyframe channels on the
  // subnet's TRS params. Absent for static prims + all glTF nodes.
  trs_samples?: TrsSample[];
}

export interface GltfPeekResult {
  path: string;
  roots: GltfHierarchyNode[];
  // USD only: stage time metadata → the editor timeline.
  animated?: boolean;
  time_codes_per_second?: number;
  start_time_code?: number;
  end_time_code?: number;
}

export const peekGltf = (path: string) =>
  send<GltfPeekResult>('peek_gltf', { path });

// ─── USD peek (structural import helper) ───────────────────────────────────
// Same hierarchy shape as the glTF peek — the importer treats them
// identically (a node tree with local/world TRS + SceneObject keys). USD's
// first slice is flat: one node per mesh prim, carrying the prim's world TRS
// and its Sdf path as the `mesh_names` entry the `usd_import` SOP looks up.
export type HierarchyNode = GltfHierarchyNode;
export type PeekResult = GltfPeekResult;

export const peekUsd = (path: string) =>
  send<PeekResult>('peek_usd', { path });

// ─── MaterialX import ──────────────────────────────────────────────────────
// Read a .mtlx and map its standard_surface materials onto the engine BSDF.
// `params` are keyed EXACTLY by the object_output SOP's override-param names,
// so the inspector writes them straight onto the selected Object Output node
// (reusing the inline-override path — preserves clearcoat/sheen/subsurface/
// anisotropy). Errors (no MaterialX support / no surface material) reject.

export interface MaterialXImportParams {
  override_material: boolean;
  base_color: [number, number, number];
  metallic: number;
  roughness: number;
  transmission: number;
  ior: number;
  emission: [number, number, number];
  emission_strength: number;
  opacity: number;
  clearcoat: number;
  clearcoat_roughness: number;
  sheen: number;
  subsurface: number;
  subsurface_color: [number, number, number];
  anisotropy: number;
}

export interface MaterialXMaterial {
  name: string;
  params: MaterialXImportParams;
}

export const readMaterialXMaterials = (path: string) =>
  send<MaterialXMaterial[]>('read_materialx_material', { path });

// ─── Video export ──────────────────────────────────────────────────────────
// Drives an offline render at the timeline frame range. The native worker
// broadcasts `video_export_progress`, `video_export_done`, and
// `video_export_error` events while running; subscribe via listen().

export type VideoCodec = 'h264' | 'prores';

export interface VideoExportRequest {
  path: string;
  frame_start: number;
  frame_end: number;
  fps: number;
  samples_per_frame: number;
  // Max ray bounces per sample. 0 leaves the engine's current setting alone.
  max_bounces: number;
  // Output resolution. 0 means "use the current path-tracer viewport size".
  width: number;
  height: number;
  codec: VideoCodec;
  // 'video' (default) → AVFoundation movie via codec. 'exr' → a multi-layer
  // OpenEXR sequence (linear beauty + albedo/normal/depth/position/emission/id
  // AOVs), one file per frame named "<path-stem>.NNNN.exr".
  format?: 'video' | 'exr';
  // EXR only: run the OIDN denoiser (albedo + normal guided) on the beauty
  // before writing. Ignored if the engine wasn't built with OIDN.
  denoise?: boolean;
}

export const exportVideoStart = (req: VideoExportRequest) =>
  send<null>('export_video_start', req as unknown as Record<string, unknown>);
export const exportVideoCancel = () => send<null>('export_video_cancel');

// ─── Still render ────────────────────────────────────────────────────────────
// Render a SINGLE offline frame at an arbitrary resolution → one image file.
// Renders the current scene + camera as-is (no timeline seek). The native
// worker broadcasts `render_still_done` ({path, cancelled}) and
// `render_still_error` ({message}); subscribe via listen(). Shares the export
// worker, so it's mutually exclusive with a sequence export.
export interface RenderStillRequest {
  path: string;
  // Output resolution. 0 → use the current path-tracer viewport size.
  width: number;
  height: number;
  samples: number;
  // Max ray bounces. 0 leaves the engine's current setting alone.
  max_bounces?: number;
  // 'png' → LDR (tonemapped) PNG. 'exr' → linear multi-layer EXR (beauty + AOVs).
  format?: 'png' | 'exr';
  // EXR only: OIDN-denoise the beauty before writing. Ignored without OIDN.
  denoise?: boolean;
}

export const renderStill = (req: RenderStillRequest) =>
  send<null>('render_still', req as unknown as Record<string, unknown>);

// ─── Timeline / playback ───────────────────────────────────────────────────
// The playhead is owned by the native side (advances in render_tick), so the
// frontend only sends transport commands and listens for `timeline_tick`
// broadcasts via listen('timeline_tick', ...).

export type LoopMode = 'once' | 'loop' | 'pingpong';
export type Interp = 'step' | 'linear' | 'bezier';

export interface TimelineState {
  fps: number;
  frame_start: number;
  frame_end: number;
  current_time: number;  // seconds
  playing: boolean;
  loop: LoopMode;
}

export const timelineGet = () => send<TimelineState>('timeline_get');

export const timelineSetRange = (fps: number, frameStart: number, frameEnd: number) =>
  send<null>('timeline_set_range', {
    fps,
    frame_start: frameStart,
    frame_end: frameEnd,
  });

// Either `time` (seconds) or `frame` (1-based) is accepted; prefer `frame` from
// the playbar UI so the snap-to-frame conversion happens server-side.
export const timelineSetPlayhead = (input: { time?: number; frame?: number }) =>
  send<null>('timeline_set_playhead', input);

export const timelinePlay  = () => send<null>('timeline_play');
export const timelinePause = () => send<null>('timeline_pause');
export const timelineSetLoop = (mode: LoopMode) =>
  send<null>('timeline_set_loop', { mode });

// Frame-lock toggle. When false (default), playback runs in async mode:
// the playhead advances by wall-clock dt every render_tick and the cook
// worker keeps up best-effort, dropping frames when it falls behind. When
// true, the playhead advances by exactly 1/fps after each cook completion
// so every frame is shown — playback speed becomes cook throughput.
// Either way the UI thread runs at vsync; nothing blocks the render path.
export const timelineGetFrameLocked = () =>
  send<boolean>('timeline_get_frame_locked');
export const timelineSetFrameLocked = (value: boolean) =>
  send<null>('timeline_set_frame_locked', { value });

// ─── Keyframe edits ────────────────────────────────────────────────────────
// Keys live on SOP node parameters. `component` is 0 for scalar params and
// 0..2 for vec3 (per-component, Houdini-style). `time` is seconds.

export const paramSetKeyframe = (args: {
  nodeUid: number;
  paramName: string;
  component?: number;
  time: number;
  value: number;
  interp?: Interp;
  inTangent?: number;
  outTangent?: number;
}) =>
  send<null>('param_set_keyframe', {
    node_uid: args.nodeUid,
    param_name: args.paramName,
    component: args.component ?? 0,
    time: args.time,
    value: args.value,
    interp: args.interp ?? 'linear',
    in_tangent: args.inTangent ?? 0,
    out_tangent: args.outTangent ?? 0,
  });

// Retime a key while preserving value + tangents + interpolation. Returns
// false if no key existed at `fromTime`.
export const paramMoveKeyframe = (args: {
  nodeUid: number;
  paramName: string;
  component?: number;
  fromTime: number;
  toTime: number;
}) =>
  send<boolean>('param_move_keyframe', {
    node_uid: args.nodeUid,
    param_name: args.paramName,
    component: args.component ?? 0,
    from_time: args.fromTime,
    to_time: args.toTime,
  });

export const paramDeleteKeyframe = (args: {
  nodeUid: number;
  paramName: string;
  component?: number;
  time: number;
}) =>
  send<boolean>('param_delete_keyframe', {
    node_uid: args.nodeUid,
    param_name: args.paramName,
    component: args.component ?? 0,
    time: args.time,
  });

// Set a channel's pre/post extrapolation (what the curve does outside the
// keyed range). Either side may be omitted to leave it unchanged. Returns
// false when the channel doesn't exist (no keys on that component).
export type Extrap = 'hold' | 'cycle' | 'linear';
export const paramSetChannelExtrap = (args: {
  nodeUid: number;
  paramName: string;
  component?: number;
  pre?: Extrap;
  post?: Extrap;
}) =>
  send<boolean>('param_set_channel_extrap', {
    node_uid: args.nodeUid,
    param_name: args.paramName,
    component: args.component ?? 0,
    ...(args.pre ? { pre: args.pre } : {}),
    ...(args.post ? { post: args.post } : {}),
  });

// component < 0 (default) clears all components.
export const paramClearChannel = (args: {
  nodeUid: number;
  paramName: string;
  component?: number;
}) =>
  send<null>('param_clear_channel', {
    node_uid: args.nodeUid,
    param_name: args.paramName,
    component: args.component ?? -1,
  });

// ─── Helpers ───────────────────────────────────────────────────────────────

function decodeBase64(b64: string): Uint8Array {
  const binary = atob(b64);
  const len = binary.length;
  const out = new Uint8Array(len);
  for (let i = 0; i < len; i++) out[i] = binary.charCodeAt(i);
  return out;
}
