//! Raw C FFI bindings to tracey_c_api
//!
//! These are direct, unsafe bindings to the C API.
//! Use the safe wrappers in `types.rs` instead.

use std::os::raw::{c_char, c_double, c_float, c_uint};

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
    ErrorUnknown = -999,
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

    // Scene Management
    pub fn tracey_scene_create() -> *mut TraceyScene;
    pub fn tracey_scene_destroy(scene: *mut TraceyScene);
    pub fn tracey_scene_create_actor(scene: *mut TraceyScene, name: *const c_char) -> u64;
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
    pub fn tracey_scene_get_actor_count(scene: *mut TraceyScene) -> c_uint;
    pub fn tracey_scene_get_actor_uids(
        scene: *mut TraceyScene,
        out_uids: *mut u64,
        max_count: c_uint,
    ) -> c_uint;

    // Scene Compilation
    pub fn tracey_compile_scene(
        device: *mut TraceyDevice,
        scene: *mut TraceyScene,
    ) -> *mut TraceyCompiledScene;
    pub fn tracey_destroy_compiled_scene(compiled_scene: *mut TraceyCompiledScene);

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

    // Error Handling
    pub fn tracey_get_last_error() -> *const c_char;
    pub fn tracey_clear_last_error();

    // Utility Functions
    pub fn tracey_get_version() -> *const c_char;
    pub fn tracey_transform_identity() -> TraceyTransform;
    pub fn tracey_camera_default() -> TraceyCamera;
}
