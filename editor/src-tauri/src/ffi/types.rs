//! Safe Rust wrappers around the C API
//!
//! These types provide RAII, error handling, and Rust-friendly APIs.

use super::raw::*;
use serde::{Deserialize, Serialize};
use std::ffi::{CStr, CString};
use std::fmt;
use std::ptr;

// ============================================================================
// Error Handling
// ============================================================================

#[derive(Debug, Clone)]
pub struct TraceyError(pub String);

impl fmt::Display for TraceyError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "Tracey error: {}", self.0)
    }
}

impl std::error::Error for TraceyError {}

pub type Result<T> = std::result::Result<T, TraceyError>;

fn get_last_error() -> String {
    unsafe {
        let err_ptr = tracey_get_last_error();
        if err_ptr.is_null() {
            return "Unknown error".to_string();
        }
        CStr::from_ptr(err_ptr)
            .to_string_lossy()
            .into_owned()
    }
}

fn check_result(result: TraceyResult) -> Result<()> {
    match result {
        TraceyResult::Success => Ok(()),
        _ => Err(TraceyError(get_last_error())),
    }
}

// ============================================================================
// Math Types
// ============================================================================

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct Vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Vec3 {
    pub fn new(x: f32, y: f32, z: f32) -> Self {
        Self { x, y, z }
    }

    pub fn zero() -> Self {
        Self::new(0.0, 0.0, 0.0)
    }

    pub fn one() -> Self {
        Self::new(1.0, 1.0, 1.0)
    }
}

impl From<Vec3> for TraceyVec3 {
    fn from(v: Vec3) -> Self {
        TraceyVec3 { x: v.x, y: v.y, z: v.z }
    }
}

impl From<TraceyVec3> for Vec3 {
    fn from(v: TraceyVec3) -> Self {
        Vec3 { x: v.x, y: v.y, z: v.z }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct Quat {
    pub w: f32,
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Quat {
    pub fn new(w: f32, x: f32, y: f32, z: f32) -> Self {
        Self { w, x, y, z }
    }

    pub fn identity() -> Self {
        Self::new(1.0, 0.0, 0.0, 0.0)
    }
}

impl From<Quat> for TraceyQuat {
    fn from(q: Quat) -> Self {
        TraceyQuat { w: q.w, x: q.x, y: q.y, z: q.z }
    }
}

impl From<TraceyQuat> for Quat {
    fn from(q: TraceyQuat) -> Self {
        Quat { w: q.w, x: q.x, y: q.y, z: q.z }
    }
}

// ============================================================================
// Transform
// ============================================================================

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Transform {
    pub position: Vec3,
    pub rotation: Quat,
    pub scale: Vec3,
}

impl Transform {
    pub fn identity() -> Self {
        unsafe { tracey_transform_identity().into() }
    }

    pub fn new(position: Vec3, rotation: Quat, scale: Vec3) -> Self {
        Self { position, rotation, scale }
    }
}

impl From<Transform> for TraceyTransform {
    fn from(t: Transform) -> Self {
        TraceyTransform {
            position: t.position.into(),
            rotation: t.rotation.into(),
            scale: t.scale.into(),
        }
    }
}

impl From<TraceyTransform> for Transform {
    fn from(t: TraceyTransform) -> Self {
        Transform {
            position: t.position.into(),
            rotation: t.rotation.into(),
            scale: t.scale.into(),
        }
    }
}

// ============================================================================
// Camera
// ============================================================================

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Camera {
    pub position: Vec3,
    pub rotation: Quat,
    pub fov: f32,
    pub near_plane: f32,
    pub far_plane: f32,
    pub aspect_ratio: f32,
}

impl Camera {
    pub fn default() -> Self {
        unsafe { tracey_camera_default().into() }
    }

    pub fn new(position: Vec3, rotation: Quat, fov: f32, aspect_ratio: f32) -> Self {
        Self {
            position,
            rotation,
            fov,
            near_plane: 0.01,
            far_plane: 1000.0,
            aspect_ratio,
        }
    }
}

impl From<Camera> for TraceyCamera {
    fn from(c: Camera) -> Self {
        TraceyCamera {
            position: c.position.into(),
            rotation: c.rotation.into(),
            fov: c.fov,
            near_plane: c.near_plane,
            far_plane: c.far_plane,
            aspect_ratio: c.aspect_ratio,
        }
    }
}

impl From<TraceyCamera> for Camera {
    fn from(c: TraceyCamera) -> Self {
        Camera {
            position: c.position.into(),
            rotation: c.rotation.into(),
            fov: c.fov,
            near_plane: c.near_plane,
            far_plane: c.far_plane,
            aspect_ratio: c.aspect_ratio,
        }
    }
}

// ============================================================================
// Device
// ============================================================================

pub struct Device {
    ptr: *mut TraceyDevice,
}

unsafe impl Send for Device {}
unsafe impl Sync for Device {}

impl Device {
    pub fn new(device_type: DeviceType, backend: DeviceBackend) -> Result<Self> {
        unsafe {
            let ptr = tracey_create_device(device_type.into(), backend.into());
            if ptr.is_null() {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(Self { ptr })
            }
        }
    }

    pub fn as_ptr(&self) -> *mut TraceyDevice {
        self.ptr
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        unsafe {
            tracey_destroy_device(self.ptr);
        }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub enum DeviceType {
    Cpu,
    Gpu,
}

impl From<DeviceType> for TraceyDeviceType {
    fn from(t: DeviceType) -> Self {
        match t {
            DeviceType::Cpu => TraceyDeviceType::Cpu,
            DeviceType::Gpu => TraceyDeviceType::Gpu,
        }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub enum DeviceBackend {
    None,
    Compute,
    Rtx,
}

impl From<DeviceBackend> for TraceyDeviceBackend {
    fn from(b: DeviceBackend) -> Self {
        match b {
            DeviceBackend::None => TraceyDeviceBackend::None,
            DeviceBackend::Compute => TraceyDeviceBackend::Compute,
            DeviceBackend::Rtx => TraceyDeviceBackend::Rtx,
        }
    }
}

// ============================================================================
// Scene
// ============================================================================

pub struct Scene {
    ptr: *mut TraceyScene,
}

unsafe impl Send for Scene {}
unsafe impl Sync for Scene {}

impl Scene {
    pub fn new() -> Result<Self> {
        unsafe {
            let ptr = tracey_scene_create();
            if ptr.is_null() {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(Self { ptr })
            }
        }
    }

    pub fn create_actor(&mut self, name: &str) -> Result<u64> {
        let c_name = CString::new(name).map_err(|_| TraceyError("Invalid name".to_string()))?;
        unsafe {
            let uid = tracey_scene_create_actor(self.ptr, c_name.as_ptr());
            if uid == u64::MAX {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(uid)
            }
        }
    }

    pub fn set_actor_transform(&mut self, actor_uid: u64, transform: &Transform) -> Result<()> {
        unsafe {
            let t: TraceyTransform = transform.clone().into();
            check_result(tracey_scene_set_actor_transform(self.ptr, actor_uid, &t))
        }
    }

    pub fn get_actor_transform(&self, actor_uid: u64) -> Result<Transform> {
        unsafe {
            let mut t = TraceyTransform {
                position: TraceyVec3 { x: 0.0, y: 0.0, z: 0.0 },
                rotation: TraceyQuat { w: 1.0, x: 0.0, y: 0.0, z: 0.0 },
                scale: TraceyVec3 { x: 1.0, y: 1.0, z: 1.0 },
            };
            check_result(tracey_scene_get_actor_transform(self.ptr, actor_uid, &mut t))?;
            Ok(t.into())
        }
    }

    pub fn set_actor_name(&mut self, actor_uid: u64, name: &str) -> Result<()> {
        let c_name = CString::new(name).map_err(|_| TraceyError("Invalid name".to_string()))?;
        unsafe {
            check_result(tracey_scene_set_actor_name(self.ptr, actor_uid, c_name.as_ptr()))
        }
    }

    pub fn set_camera(&mut self, camera: &Camera) -> Result<()> {
        unsafe {
            let c: TraceyCamera = camera.clone().into();
            check_result(tracey_scene_set_camera(self.ptr, &c))
        }
    }

    pub fn get_camera(&self) -> Result<Camera> {
        unsafe {
            let mut c = tracey_camera_default();
            check_result(tracey_scene_get_camera(self.ptr, &mut c))?;
            Ok(c.into())
        }
    }

    pub fn load_gltf(&mut self, path: &str) -> Result<()> {
        let c_path = CString::new(path).map_err(|_| TraceyError("Invalid path".to_string()))?;
        unsafe {
            check_result(tracey_scene_load_gltf(self.ptr, c_path.as_ptr()))
        }
    }

    pub fn get_actor_count(&self) -> u32 {
        unsafe { tracey_scene_get_actor_count(self.ptr) }
    }

    pub fn get_actor_uids(&self) -> Vec<u64> {
        let count = self.get_actor_count() as usize;
        if count == 0 {
            return Vec::new();
        }

        let mut uids = vec![0u64; count];
        unsafe {
            tracey_scene_get_actor_uids(self.ptr, uids.as_mut_ptr(), count as u32);
        }
        uids
    }

    pub fn as_ptr(&self) -> *mut TraceyScene {
        self.ptr
    }
}

impl Drop for Scene {
    fn drop(&mut self) {
        unsafe {
            tracey_scene_destroy(self.ptr);
        }
    }
}

// ============================================================================
// CompiledScene
// ============================================================================

pub struct CompiledScene {
    ptr: *mut TraceyCompiledScene,
}

unsafe impl Send for CompiledScene {}
unsafe impl Sync for CompiledScene {}

impl CompiledScene {
    pub fn compile(device: &Device, scene: &Scene) -> Result<Self> {
        unsafe {
            let ptr = tracey_compile_scene(device.as_ptr(), scene.as_ptr());
            if ptr.is_null() {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(Self { ptr })
            }
        }
    }

    pub fn as_ptr(&self) -> *mut TraceyCompiledScene {
        self.ptr
    }
}

impl Drop for CompiledScene {
    fn drop(&mut self) {
        unsafe {
            tracey_destroy_compiled_scene(self.ptr);
        }
    }
}

// ============================================================================
// PathTracer
// ============================================================================

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PathTracerConfig {
    pub width: u32,
    pub height: u32,
    pub ray_gen_shader: String,
    pub hit_shader: String,
    pub miss_shader: String,
    pub resolve_shader: Option<String>,
    pub hdr_output: bool,
}

pub struct PathTracer {
    ptr: *mut TraceyPathTracer,
    width: u32,
    height: u32,
    hdr: bool,
}

unsafe impl Send for PathTracer {}
unsafe impl Sync for PathTracer {}

impl PathTracer {
    pub fn new(device: &Device, config: &PathTracerConfig) -> Result<Self> {
        let c_ray_gen = CString::new(config.ray_gen_shader.as_str())
            .map_err(|_| TraceyError("Invalid shader path".to_string()))?;
        let c_hit = CString::new(config.hit_shader.as_str())
            .map_err(|_| TraceyError("Invalid shader path".to_string()))?;
        let c_miss = CString::new(config.miss_shader.as_str())
            .map_err(|_| TraceyError("Invalid shader path".to_string()))?;

        let c_resolve = config.resolve_shader.as_ref().and_then(|s| CString::new(s.as_str()).ok());

        let c_config = TraceyPathTracerConfig {
            width: config.width,
            height: config.height,
            ray_gen_shader_path: c_ray_gen.as_ptr(),
            hit_shader_path: c_hit.as_ptr(),
            miss_shader_path: c_miss.as_ptr(),
            resolve_shader_path: c_resolve.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
            hdr_output: config.hdr_output,
        };

        unsafe {
            let ptr = tracey_path_tracer_create(device.as_ptr(), &c_config);
            if ptr.is_null() {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(Self {
                    ptr,
                    width: config.width,
                    height: config.height,
                    hdr: config.hdr_output,
                })
            }
        }
    }

    pub fn render(
        &mut self,
        compiled_scene: &CompiledScene,
        camera: &Camera,
        clear_accumulation: bool,
    ) -> Result<f64> {
        unsafe {
            let c: TraceyCamera = camera.clone().into();
            let time = tracey_path_tracer_render(
                self.ptr,
                compiled_scene.as_ptr(),
                &c,
                clear_accumulation,
            );
            if time < 0.0 {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(time)
            }
        }
    }

    pub fn readback(&self) -> Result<Vec<u8>> {
        let bytes_per_pixel = if self.hdr { 16 } else { 4 };
        let buffer_size = (self.width * self.height) as usize * bytes_per_pixel;
        let mut buffer = vec![0u8; buffer_size];

        unsafe {
            let bytes_read = tracey_path_tracer_readback(
                self.ptr as *mut _,
                buffer.as_mut_ptr(),
                buffer_size,
            );
            if bytes_read == 0 {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(buffer)
            }
        }
    }

    pub fn get_sample_count(&self) -> u32 {
        unsafe { tracey_path_tracer_get_sample_count(self.ptr as *mut _) }
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }
}

impl Drop for PathTracer {
    fn drop(&mut self) {
        unsafe {
            tracey_path_tracer_destroy(self.ptr);
        }
    }
}
