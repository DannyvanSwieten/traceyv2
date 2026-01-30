//! Raw C FFI bindings to tracey_c_api
//!
//! These are direct, unsafe bindings to the C API.
//! Use the safe wrappers in `types.rs` instead.

use std::os::raw::{c_char, c_double, c_float, c_int, c_uint};

// ============================================================================
// Opaque Handle Types
// ============================================================================

#[repr(C)]
pub struct TraceyDevice {
    _private: [u8; 0],
}

#[repr(C)]
pub struct TraceyScene {
    _private: [u8; 0],
}

#[repr(C)]
pub struct TraceyCompiledScene {
    _private: [u8; 0],
}

#[repr(C)]
pub struct TraceyPathTracer {
    _private: [u8; 0],
}

#[repr(C)]
pub struct TraceyRasterizer {
    _private: [u8; 0],
}

#[repr(C)]
pub struct TraceyPresenter {
    _private: [u8; 0],
}

#[repr(C)]
pub struct TraceyNodeGraph {
    _private: [u8; 0],
}

#[repr(C)]
pub struct TraceyNode {
    _private: [u8; 0],
}

#[repr(C)]
pub struct TraceyParameter {
    _private: [u8; 0],
}

// ============================================================================
// Math Types
// ============================================================================

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyVec2 {
    pub x: c_float,
    pub y: c_float,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyVec3 {
    pub x: c_float,
    pub y: c_float,
    pub z: c_float,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyVec4 {
    pub x: c_float,
    pub y: c_float,
    pub z: c_float,
    pub w: c_float,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyQuat {
    pub w: c_float,
    pub x: c_float,
    pub y: c_float,
    pub z: c_float,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyMat4 {
    pub m: [c_float; 16],
}

// ============================================================================
// Scene Types
// ============================================================================

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyTransform {
    pub position: TraceyVec3,
    pub rotation: TraceyQuat,
    pub scale: TraceyVec3,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyCamera {
    pub position: TraceyVec3,
    pub rotation: TraceyQuat,
    pub fov: c_float,
    pub near_plane: c_float,
    pub far_plane: c_float,
    pub aspect_ratio: c_float,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum TraceyMaterialPropertyType {
    Float = 0,
    Vec2 = 1,
    Vec3 = 2,
    Vec4 = 3,
    Int = 4,
    Texture = 5,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum TraceyNodeType {
    Actor = 0,
    PrimitiveCube = 1,
    PrimitiveSphere = 2,
    PrimitiveTorus = 3,
    PrimitivePlane = 4,
    PrimitiveCylinder = 5,
    PrimitiveCone = 6,
    GeometryTransform = 7,
    GeometryMerge = 8,
    Material = 9,
    Transform = 10,
    Camera = 11,
    MathFloat = 12,
    MathVector = 13,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum TraceyParameterType {
    Float = 0,
    Vec2 = 1,
    Vec3 = 2,
    Vec4 = 3,
    Int = 4,
    Bool = 5,
    String = 6,
    Color = 7,
    Texture = 8,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union TraceyMaterialPropertyValue {
    pub float_value: c_float,
    pub vec2_value: TraceyVec2,
    pub vec3_value: TraceyVec3,
    pub vec4_value: TraceyVec4,
    pub int_value: i32,
    pub texture_value: *const c_char,
}

#[repr(C)]
pub struct TraceyMaterialProperty {
    pub name: *const c_char,
    pub prop_type: TraceyMaterialPropertyType,
    pub value: TraceyMaterialPropertyValue,
}

#[repr(C)]
pub struct TraceyMaterialInstance {
    pub shader_id: *const c_char,
    pub properties: *const TraceyMaterialProperty,
    pub property_count: c_uint,
}

#[repr(C)]
pub struct TraceySceneInstance {
    pub object_ref: *const c_char,
    pub material: TraceyMaterialInstance,
    pub has_local_transform: bool,
    pub local_transform: TraceyTransform,
}

// ============================================================================
// Device Types
// ============================================================================

#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum TraceyDeviceType {
    Cpu = 0,
    Gpu = 1,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum TraceyDeviceBackend {
    None = 0,
    Compute = 1,
    Rtx = 2,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum TraceyImageFormat {
    R8G8B8A8Unorm = 0,
    R8G8B8A8Srgb = 1,
    R32G32B32A32Sfloat = 2,
    R32Sfloat = 3,
}

// ============================================================================
// Path Tracer Configuration
// ============================================================================

#[repr(C)]
pub struct TraceyPathTracerConfig {
    pub width: c_uint,
    pub height: c_uint,
    pub ray_gen_shader_path: *const c_char,
    pub hit_shader_path: *const c_char,
    pub miss_shader_path: *const c_char,
    pub resolve_shader_path: *const c_char,
    pub hdr_output: bool,
    pub samples_per_frame: c_uint,
    pub max_bounces: c_uint,
}

// ============================================================================
// Rasterizer Configuration
// ============================================================================

#[repr(C)]
pub struct TraceyRasterizerConfig {
    pub width: c_uint,
    pub height: c_uint,
    pub vertex_shader_path: *const c_char,
    pub fragment_shader_path: *const c_char,
    pub use_depth_buffer: bool,
    pub depth_test_enable: bool,
    pub cull_back_faces: bool,
    pub alpha_blending: bool,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyPresenterConfig {
    pub width: c_uint,
    pub height: c_uint,
    pub enable_hdr: bool,
    pub desired_image_count: c_uint,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyViewportBounds {
    pub x: i32,
    pub y: i32,
    pub width: c_uint,
    pub height: c_uint,
}

// ============================================================================
// Result Codes
// ============================================================================

#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum TraceyResult {
    Success = 0,
    ErrorInvalidParameter = -1,
    ErrorOutOfMemory = -2,
    ErrorDeviceCreationFailed = -3,
    ErrorShaderCompilationFailed = -4,
    ErrorSceneCompilationFailed = -5,
    ErrorFileNotFound = -6,
    ErrorRenderingFailed = -7,
    ErrorNullPointer = -8,
    ErrorNotFound = -9,
    ErrorUnknown = -999,
}

// ============================================================================
// Scene Query Types
// ============================================================================

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyActorInfo {
    pub uid: u64,
    pub name: *const c_char,
    pub transform: TraceyTransform,
    pub child_count: c_uint,
    pub instance_count: c_uint,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyInstanceInfo {
    pub object_ref: *const c_char,
    pub shader_id: *const c_char,
    pub has_local_transform: bool,
    pub local_transform: TraceyTransform,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyMeshInfo {
    pub name: *const c_char,
    pub vertex_count: c_uint,
    pub triangle_count: c_uint,
    pub has_indices: bool,
    pub has_normals: bool,
    pub has_uvs: bool,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct TraceyTextureInfo {
    pub id: *const c_char,
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub mime_type: *const c_char,
}

// ============================================================================
// Extern C Functions
// ============================================================================

extern "C" {
    // Device Management
    pub fn tracey_create_device(
        device_type: TraceyDeviceType,
        backend: TraceyDeviceBackend,
    ) -> *mut TraceyDevice;
    pub fn tracey_destroy_device(device: *mut TraceyDevice);
    pub fn tracey_device_wait_idle(device: *mut TraceyDevice);

    // Scene Management
    pub fn tracey_scene_create() -> *mut TraceyScene;
    pub fn tracey_scene_destroy(scene: *mut TraceyScene);
    pub fn tracey_scene_clear(scene: *mut TraceyScene);
    pub fn tracey_scene_create_actor(scene: *mut TraceyScene, name: *const c_char) -> u64;
    pub fn tracey_scene_remove_actor(scene: *mut TraceyScene, actor_uid: u64) -> TraceyResult;
    pub fn tracey_scene_get_actor_transform(
        scene: *mut TraceyScene,
        actor_uid: u64,
        out_transform: *mut TraceyTransform,
    ) -> TraceyResult;
    pub fn tracey_scene_set_actor_transform(
        scene: *mut TraceyScene,
        actor_uid: u64,
        transform: *const TraceyTransform,
    ) -> TraceyResult;
    pub fn tracey_scene_set_actor_name(
        scene: *mut TraceyScene,
        actor_uid: u64,
        name: *const c_char,
    ) -> TraceyResult;
    pub fn tracey_scene_add_instance(
        scene: *mut TraceyScene,
        actor_uid: u64,
        instance: *const TraceySceneInstance,
    ) -> TraceyResult;
    pub fn tracey_scene_set_camera(
        scene: *mut TraceyScene,
        camera: *const TraceyCamera,
    ) -> TraceyResult;
    pub fn tracey_scene_get_camera(
        scene: *mut TraceyScene,
        out_camera: *mut TraceyCamera,
    ) -> TraceyResult;
    pub fn tracey_scene_load_gltf(scene: *mut TraceyScene, file_path: *const c_char)
        -> TraceyResult;
    pub fn tracey_scene_load_gltf_with_project(
        scene: *mut TraceyScene,
        file_path: *const c_char,
        project_root: *const c_char,
    ) -> TraceyResult;
    pub fn tracey_scene_add_gltf_with_project(
        scene: *mut TraceyScene,
        file_path: *const c_char,
        project_root: *const c_char,
    ) -> TraceyResult;
    pub fn tracey_scene_get_actor_count(scene: *mut TraceyScene) -> c_uint;
    pub fn tracey_scene_get_actor_uids(
        scene: *mut TraceyScene,
        out_uids: *mut u64,
        max_count: c_uint,
    ) -> c_uint;

    // Scene Query Functions
    pub fn tracey_scene_get_actor_name(
        scene: *mut TraceyScene,
        actor_uid: u64,
    ) -> *const c_char;
    pub fn tracey_scene_get_actor_children(
        scene: *mut TraceyScene,
        actor_uid: u64,
        out_uids: *mut u64,
        max_count: c_uint,
    ) -> c_uint;
    pub fn tracey_scene_get_actor_instance_count(
        scene: *mut TraceyScene,
        actor_uid: u64,
    ) -> c_uint;
    pub fn tracey_scene_get_actor_instance(
        scene: *mut TraceyScene,
        actor_uid: u64,
        instance_index: c_uint,
        out_info: *mut TraceyInstanceInfo,
    ) -> TraceyResult;

    // Material editing functions
    pub fn tracey_scene_get_instance_material_property_count(
        scene: *mut TraceyScene,
        actor_uid: u64,
        instance_index: c_uint,
    ) -> c_uint;
    pub fn tracey_scene_get_instance_material_property_by_name(
        scene: *mut TraceyScene,
        actor_uid: u64,
        instance_index: c_uint,
        property_name: *const c_char,
        out_property: *mut TraceyMaterialProperty,
    ) -> TraceyResult;
    pub fn tracey_scene_set_instance_material_float(
        scene: *mut TraceyScene,
        actor_uid: u64,
        instance_index: c_uint,
        property_name: *const c_char,
        value: c_float,
    ) -> TraceyResult;
    pub fn tracey_scene_set_instance_material_vec3(
        scene: *mut TraceyScene,
        actor_uid: u64,
        instance_index: c_uint,
        property_name: *const c_char,
        value: TraceyVec3,
    ) -> TraceyResult;
    pub fn tracey_scene_set_instance_material_vec4(
        scene: *mut TraceyScene,
        actor_uid: u64,
        instance_index: c_uint,
        property_name: *const c_char,
        value: TraceyVec4,
    ) -> TraceyResult;
    pub fn tracey_scene_set_instance_material_texture(
        scene: *mut TraceyScene,
        actor_uid: u64,
        instance_index: c_uint,
        property_name: *const c_char,
        texture_path: *const c_char,
    ) -> TraceyResult;
    pub fn tracey_scene_get_instance_material_shader_id(
        scene: *mut TraceyScene,
        actor_uid: u64,
        instance_index: c_uint,
    ) -> *const c_char;

    pub fn tracey_scene_get_mesh_count(scene: *mut TraceyScene) -> c_uint;
    pub fn tracey_scene_get_mesh_names(
        scene: *mut TraceyScene,
        out_names: *mut *const c_char,
        max_count: c_uint,
    ) -> c_uint;
    pub fn tracey_scene_get_mesh_info(
        scene: *mut TraceyScene,
        name: *const c_char,
        out_info: *mut TraceyMeshInfo,
    ) -> TraceyResult;
    pub fn tracey_scene_get_texture_count(scene: *mut TraceyScene) -> c_uint;
    pub fn tracey_scene_get_texture_ids(
        scene: *mut TraceyScene,
        out_ids: *mut *const c_char,
        max_count: c_uint,
    ) -> c_uint;
    pub fn tracey_scene_get_texture_info(
        scene: *mut TraceyScene,
        id: *const c_char,
        out_info: *mut TraceyTextureInfo,
    ) -> TraceyResult;

    // Primitive Creation Functions
    pub fn tracey_scene_add_cube(
        scene: *mut TraceyScene,
        name: *const c_char,
        size: c_float,
    ) -> u64;
    pub fn tracey_scene_add_sphere(
        scene: *mut TraceyScene,
        name: *const c_char,
        radius: c_float,
        segments: c_uint,
        rings: c_uint,
    ) -> u64;
    pub fn tracey_scene_add_torus(
        scene: *mut TraceyScene,
        name: *const c_char,
        major_radius: c_float,
        minor_radius: c_float,
        major_segments: c_uint,
        minor_segments: c_uint,
    ) -> u64;
    pub fn tracey_scene_add_plane(
        scene: *mut TraceyScene,
        name: *const c_char,
        width: c_float,
        depth: c_float,
    ) -> u64;
    pub fn tracey_scene_add_cylinder(
        scene: *mut TraceyScene,
        name: *const c_char,
        radius: c_float,
        height: c_float,
        segments: c_uint,
    ) -> u64;
    pub fn tracey_scene_add_cone(
        scene: *mut TraceyScene,
        name: *const c_char,
        radius: c_float,
        height: c_float,
        segments: c_uint,
    ) -> u64;

    // Primitive Creation with Pre-assigned Actor ID
    pub fn tracey_scene_add_cube_with_id(
        scene: *mut TraceyScene,
        actor_uid: u64,
        name: *const c_char,
        size: c_float,
    ) -> TraceyResult;
    pub fn tracey_scene_add_sphere_with_id(
        scene: *mut TraceyScene,
        actor_uid: u64,
        name: *const c_char,
        radius: c_float,
        segments: c_uint,
        rings: c_uint,
    ) -> TraceyResult;
    pub fn tracey_scene_add_torus_with_id(
        scene: *mut TraceyScene,
        actor_uid: u64,
        name: *const c_char,
        major_radius: c_float,
        minor_radius: c_float,
        major_segments: c_uint,
        minor_segments: c_uint,
    ) -> TraceyResult;
    pub fn tracey_scene_add_plane_with_id(
        scene: *mut TraceyScene,
        actor_uid: u64,
        name: *const c_char,
        width: c_float,
        depth: c_float,
    ) -> TraceyResult;
    pub fn tracey_scene_add_cylinder_with_id(
        scene: *mut TraceyScene,
        actor_uid: u64,
        name: *const c_char,
        radius: c_float,
        height: c_float,
        segments: c_uint,
    ) -> TraceyResult;
    pub fn tracey_scene_add_cone_with_id(
        scene: *mut TraceyScene,
        actor_uid: u64,
        name: *const c_char,
        radius: c_float,
        height: c_float,
        segments: c_uint,
    ) -> TraceyResult;

    // Scene Compilation
    pub fn tracey_compile_scene(
        device: *mut TraceyDevice,
        scene: *mut TraceyScene,
    ) -> *mut TraceyCompiledScene;
    pub fn tracey_destroy_compiled_scene(compiled_scene: *mut TraceyCompiledScene);
    pub fn tracey_update_scene_transforms(
        device: *mut TraceyDevice,
        scene: *mut TraceyScene,
        compiled_scene: *mut TraceyCompiledScene,
    ) -> c_int;
    pub fn tracey_update_scene_materials(
        device: *mut TraceyDevice,
        scene: *mut TraceyScene,
        compiled_scene: *mut TraceyCompiledScene,
    ) -> c_int;

    // Path Tracer
    pub fn tracey_path_tracer_create(
        device: *mut TraceyDevice,
        config: *const TraceyPathTracerConfig,
    ) -> *mut TraceyPathTracer;
    pub fn tracey_path_tracer_destroy(path_tracer: *mut TraceyPathTracer);
    pub fn tracey_path_tracer_render(
        path_tracer: *mut TraceyPathTracer,
        compiled_scene: *mut TraceyCompiledScene,
        camera: *const TraceyCamera,
        clear_accumulation: bool,
    ) -> c_double;
    pub fn tracey_path_tracer_readback(
        path_tracer: *mut TraceyPathTracer,
        out_buffer: *mut u8,
        buffer_size: usize,
    ) -> usize;
    pub fn tracey_path_tracer_get_resolution(
        path_tracer: *mut TraceyPathTracer,
        out_width: *mut c_uint,
        out_height: *mut c_uint,
    ) -> TraceyResult;
    pub fn tracey_path_tracer_get_sample_count(path_tracer: *mut TraceyPathTracer) -> c_uint;
    pub fn tracey_path_tracer_get_samples_per_frame(path_tracer: *mut TraceyPathTracer) -> c_uint;
    pub fn tracey_path_tracer_set_samples_per_frame(
        path_tracer: *mut TraceyPathTracer,
        samples: c_uint,
    ) -> TraceyResult;
    pub fn tracey_path_tracer_get_max_bounces(path_tracer: *mut TraceyPathTracer) -> c_uint;
    pub fn tracey_path_tracer_set_max_bounces(
        path_tracer: *mut TraceyPathTracer,
        bounces: c_uint,
    ) -> TraceyResult;

    // Rasterizer
    pub fn tracey_rasterizer_create(
        device: *mut TraceyDevice,
        config: *const TraceyRasterizerConfig,
    ) -> *mut TraceyRasterizer;
    pub fn tracey_rasterizer_destroy(rasterizer: *mut TraceyRasterizer);
    pub fn tracey_rasterizer_render(
        rasterizer: *mut TraceyRasterizer,
        compiled_scene: *mut TraceyCompiledScene,
        camera: *const TraceyCamera,
    ) -> c_double;
    pub fn tracey_rasterizer_readback(
        rasterizer: *mut TraceyRasterizer,
        out_buffer: *mut u8,
        buffer_size: usize,
    ) -> usize;
    pub fn tracey_rasterizer_get_resolution(
        rasterizer: *mut TraceyRasterizer,
        out_width: *mut c_uint,
        out_height: *mut c_uint,
    ) -> TraceyResult;

    // Presenter
    pub fn tracey_presenter_create(
        device: *mut TraceyDevice,
        native_window_handle: *mut std::ffi::c_void,
        native_display_handle: *mut std::ffi::c_void,
        config: *const TraceyPresenterConfig,
    ) -> *mut TraceyPresenter;
    pub fn tracey_presenter_destroy(presenter: *mut TraceyPresenter);
    pub fn tracey_presenter_present_pathtracer(
        presenter: *mut TraceyPresenter,
        path_tracer: *mut TraceyPathTracer,
    ) -> TraceyResult;
    pub fn tracey_presenter_present_rasterizer(
        presenter: *mut TraceyPresenter,
        rasterizer: *mut TraceyRasterizer,
    ) -> TraceyResult;
    pub fn tracey_presenter_present_pathtracer_to_region(
        presenter: *mut TraceyPresenter,
        path_tracer: *mut TraceyPathTracer,
        bounds: *const TraceyViewportBounds,
    ) -> TraceyResult;
    pub fn tracey_presenter_present_rasterizer_to_region(
        presenter: *mut TraceyPresenter,
        rasterizer: *mut TraceyRasterizer,
        bounds: *const TraceyViewportBounds,
    ) -> TraceyResult;
    pub fn tracey_presenter_resize(
        presenter: *mut TraceyPresenter,
        new_width: c_uint,
        new_height: c_uint,
    ) -> TraceyResult;
    pub fn tracey_presenter_wait_idle(presenter: *mut TraceyPresenter);

    // Procedural Node System (Phase 1)
    pub fn tracey_scene_get_node_graph(scene: *mut TraceyScene) -> *mut TraceyNodeGraph;
    pub fn tracey_node_graph_create_node(
        graph: *mut TraceyNodeGraph,
        node_type: TraceyNodeType,
        name: *const c_char,
    ) -> u64;
    pub fn tracey_node_graph_get_node(graph: *mut TraceyNodeGraph, node_uid: u64)
        -> *mut TraceyNode;
    pub fn tracey_node_graph_remove_node(
        graph: *mut TraceyNodeGraph,
        node_uid: u64,
    ) -> TraceyResult;
    pub fn tracey_node_get_parameter(
        node: *mut TraceyNode,
        param_name: *const c_char,
    ) -> *mut TraceyParameter;
    pub fn tracey_parameter_set_float(param: *mut TraceyParameter, value: c_float)
        -> TraceyResult;
    pub fn tracey_parameter_get_float(
        param: *mut TraceyParameter,
        out_value: *mut c_float,
    ) -> TraceyResult;
    pub fn tracey_parameter_set_vec3(
        param: *mut TraceyParameter,
        value: *const TraceyVec3,
    ) -> TraceyResult;
    pub fn tracey_parameter_get_vec3(
        param: *mut TraceyParameter,
        out_value: *mut TraceyVec3,
    ) -> TraceyResult;
    pub fn tracey_parameter_set_int(param: *mut TraceyParameter, value: c_int) -> TraceyResult;
    pub fn tracey_parameter_get_int(
        param: *mut TraceyParameter,
        out_value: *mut c_int,
    ) -> TraceyResult;
    pub fn tracey_parameter_set_bool(param: *mut TraceyParameter, value: bool) -> TraceyResult;
    pub fn tracey_parameter_get_bool(
        param: *mut TraceyParameter,
        out_value: *mut bool,
    ) -> TraceyResult;

    // Error Handling
    pub fn tracey_get_last_error() -> *const c_char;
    pub fn tracey_clear_last_error();

    // Utility Functions
    pub fn tracey_get_version() -> *const c_char;
    pub fn tracey_transform_identity() -> TraceyTransform;
    pub fn tracey_camera_default() -> TraceyCamera;
}
