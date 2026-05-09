// Typed wrappers around the native bridge transport. Every host-side command
// has a wrapper here — components import these instead of calling invoke()
// directly. This is the single seam where the JSON wire format is bound to TS.

import { WebViewTransport, Transport } from './transport';

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

export type PrimitiveParams =
  | { type: 'cube'; size?: number }
  | { type: 'sphere'; radius?: number; segments?: number; rings?: number }
  | {
      type: 'torus';
      major_radius?: number;
      minor_radius?: number;
      major_segments?: number;
      minor_segments?: number;
    }
  | { type: 'plane'; width?: number; depth?: number }
  | { type: 'cylinder'; radius?: number; height?: number; segments?: number }
  | { type: 'cone'; radius?: number; height?: number; segments?: number };

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

async function send<T>(cmd: string, args: Record<string, unknown> = {}): Promise<T> {
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
export const deleteActor = (actorId: number) =>
  send<boolean>('delete_actor', { actor_id: actorId });
export const getAllActors = () => send<Actor[]>('get_all_actors');
export const getActor = (actorId: number) =>
  send<Actor | null>('get_actor', { actor_id: actorId });
export const setActorTransform = (actorId: number, transform: Transform) =>
  send<boolean>('set_actor_transform', { actor_id: actorId, transform });
export const setActorName = (actorId: number, name: string) =>
  send<boolean>('set_actor_name', { actor_id: actorId, name });
export const setCamera = (camera: Camera) => send<null>('set_camera', { camera });
export const getCamera = () => send<Camera>('get_camera');
export const addChild = (parentId: number, childId: number) =>
  send<boolean>('add_child', { parent_id: parentId, child_id: childId });
export const removeChild = (parentId: number, childId: number) =>
  send<boolean>('remove_child', { parent_id: parentId, child_id: childId });

// ─── Scene resource queries ────────────────────────────────────────────────

export const getActorInstances = (actorId: number) =>
  send<InstanceInfo[]>('get_actor_instances', { actor_id: actorId });
export const getMeshNames = () => send<string[]>('get_mesh_names');
export const getMeshInfo = (name: string) => send<MeshInfo>('get_mesh_info', { name });
export const getAllMeshes = () => send<MeshInfo[]>('get_all_meshes');
export const getTextureIds = () => send<string[]>('get_texture_ids');
export const getTextureInfo = (id: string) => send<TextureInfo>('get_texture_info', { id });
export const getAllTextures = () => send<TextureInfo[]>('get_all_textures');

// ─── Primitive creation ────────────────────────────────────────────────────

export const addPrimitive = (name: string, params: PrimitiveParams) =>
  send<Actor>('add_primitive', { name, params });

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
export const getSamplesPerFrame = () => send<number>('get_samples_per_frame');
export const setSamplesPerFrame = (samples: number) =>
  send<null>('set_samples_per_frame', { samples });
export const getMaxBounces = () => send<number>('get_max_bounces');
export const setMaxBounces = (bounces: number) =>
  send<null>('set_max_bounces', { bounces });

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

// ─── Material library (per-user persistent graphs) ─────────────────────────
// Graphs are stored as one .json per name in a platform-specific user data
// directory. Names are sanitized server-side (alphanumerics, ' ', '_', '-').

export const listMaterialLibrary = () =>
  send<string[]>('list_material_library');

export const saveMaterialGraphAs = (name: string, graphJson: string) =>
  send<null>('save_material_graph_as', { name, graph: graphJson });

export const loadMaterialGraphFromLibrary = (name: string) =>
  send<string>('load_material_graph_from_library', { name });

export const deleteMaterialGraphFromLibrary = (name: string) =>
  send<null>('delete_material_graph_from_library', { name });

// Assign a library graph to an actor. Empty `libraryName` clears the
// assignment back to the default passthrough. Triggers a scene recompile
// server-side so the new MaterialProgramBuffer takes effect on next render.
export const setActorMaterial = (actorId: number, libraryName: string) =>
  send<null>('set_actor_material', { actor_id: actorId, library_name: libraryName });

// ─── IO ────────────────────────────────────────────────────────────────────

export const saveScene = (path: string) => send<null>('save_scene', { path });
export const loadScene = (path: string) => send<null>('load_scene', { path });
export const importGltf = (path: string) => send<null>('import_gltf', { path });
export const exportImage = (path: string, format: string) =>
  send<null>('export_image', { path, format });

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

// ─── Helpers ───────────────────────────────────────────────────────────────

function decodeBase64(b64: string): Uint8Array {
  const binary = atob(b64);
  const len = binary.length;
  const out = new Uint8Array(len);
  for (let i = 0; i < len; i++) out[i] = binary.charCodeAt(i);
  return out;
}
