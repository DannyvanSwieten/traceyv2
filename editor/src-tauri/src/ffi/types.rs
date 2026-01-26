//! Safe Rust wrappers around the C API
//!
//! These types provide RAII, error handling, and Rust-friendly APIs.

use super::raw::*;
use serde::{Deserialize, Serialize};
use std::ffi::{CStr, CString};
use std::fmt;
use std::os::raw::c_char;
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

    /// Multiply two quaternions
    pub fn multiply(&self, other: &Quat) -> Quat {
        Quat {
            w: self.w * other.w - self.x * other.x - self.y * other.y - self.z * other.z,
            x: self.w * other.x + self.x * other.w + self.y * other.z - self.z * other.y,
            y: self.w * other.y - self.x * other.z + self.y * other.w + self.z * other.x,
            z: self.w * other.z + self.x * other.y - self.y * other.x + self.z * other.w,
        }
    }

    /// Rotate a vector by this quaternion
    pub fn rotate_vec3(&self, v: &Vec3) -> Vec3 {
        // Using the formula: v' = q * v * q^(-1)
        // Optimized version using: v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
        let qvec = Vec3::new(self.x, self.y, self.z);
        let uv = Self::cross(&qvec, v);
        let uuv = Self::cross(&qvec, &uv);

        Vec3::new(
            v.x + ((uv.x * self.w) + uuv.x) * 2.0,
            v.y + ((uv.y * self.w) + uuv.y) * 2.0,
            v.z + ((uv.z * self.w) + uuv.z) * 2.0,
        )
    }

    fn cross(a: &Vec3, b: &Vec3) -> Vec3 {
        Vec3::new(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x,
        )
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

    /// Multiply two transforms: parent * child = world
    /// This applies the parent transform to the child transform
    pub fn multiply(&self, child: &Transform) -> Transform {
        // Scale child position by parent scale
        let scaled_child_pos = Vec3::new(
            child.position.x * self.scale.x,
            child.position.y * self.scale.y,
            child.position.z * self.scale.z,
        );

        // Rotate child position by parent rotation
        let rotated_child_pos = self.rotation.rotate_vec3(&scaled_child_pos);

        // Add parent position
        let world_position = Vec3::new(
            self.position.x + rotated_child_pos.x,
            self.position.y + rotated_child_pos.y,
            self.position.z + rotated_child_pos.z,
        );

        // Multiply rotations (parent * child)
        let world_rotation = self.rotation.multiply(&child.rotation);

        // Multiply scales component-wise
        let world_scale = Vec3::new(
            self.scale.x * child.scale.x,
            self.scale.y * child.scale.y,
            self.scale.z * child.scale.z,
        );

        Transform {
            position: world_position,
            rotation: world_rotation,
            scale: world_scale,
        }
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
        self.load_gltf_with_project(path, None)
    }

    pub fn load_gltf_with_project(&mut self, path: &str, project_root: Option<&str>) -> Result<()> {
        let c_path = CString::new(path).map_err(|_| TraceyError("Invalid path".to_string()))?;

        if let Some(root) = project_root {
            let c_root = CString::new(root).map_err(|_| TraceyError("Invalid project root".to_string()))?;
            unsafe {
                check_result(tracey_scene_load_gltf_with_project(
                    self.ptr,
                    c_path.as_ptr(),
                    c_root.as_ptr(),
                ))
            }
        } else {
            unsafe {
                check_result(tracey_scene_load_gltf_with_project(
                    self.ptr,
                    c_path.as_ptr(),
                    std::ptr::null(),
                ))
            }
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

    pub fn get_actor_name(&self, actor_uid: u64) -> Option<String> {
        unsafe {
            let name_ptr = tracey_scene_get_actor_name(self.ptr, actor_uid);
            if name_ptr.is_null() {
                None
            } else {
                Some(CStr::from_ptr(name_ptr).to_string_lossy().into_owned())
            }
        }
    }

    pub fn get_actor_children(&self, actor_uid: u64) -> Vec<u64> {
        unsafe {
            // First get the count by passing 0 as max_count
            let instance_count = tracey_scene_get_actor_instance_count(self.ptr, actor_uid);
            // Allocate a reasonable buffer for children
            let mut children = vec![0u64; 256];
            let count = tracey_scene_get_actor_children(
                self.ptr,
                actor_uid,
                children.as_mut_ptr(),
                256,
            );
            children.truncate(count as usize);
            children
        }
    }

    pub fn get_actor_instance_count(&self, actor_uid: u64) -> u32 {
        unsafe { tracey_scene_get_actor_instance_count(self.ptr, actor_uid) }
    }

    pub fn get_actor_instance(&self, actor_uid: u64, instance_index: u32) -> Result<InstanceInfo> {
        unsafe {
            let mut info = TraceyInstanceInfo {
                object_ref: ptr::null(),
                shader_id: ptr::null(),
                has_local_transform: false,
                local_transform: tracey_transform_identity(),
            };
            check_result(tracey_scene_get_actor_instance(
                self.ptr,
                actor_uid,
                instance_index,
                &mut info,
            ))?;
            Ok(InstanceInfo::from(info))
        }
    }

    pub fn get_mesh_count(&self) -> u32 {
        unsafe { tracey_scene_get_mesh_count(self.ptr) }
    }

    pub fn get_mesh_names(&self) -> Vec<String> {
        let count = self.get_mesh_count() as usize;
        if count == 0 {
            return Vec::new();
        }

        let mut name_ptrs = vec![ptr::null::<std::os::raw::c_char>(); count];
        unsafe {
            tracey_scene_get_mesh_names(self.ptr, name_ptrs.as_mut_ptr(), count as u32);
            name_ptrs
                .iter()
                .filter(|p| !p.is_null())
                .map(|&p| CStr::from_ptr(p).to_string_lossy().into_owned())
                .collect()
        }
    }

    pub fn get_mesh_info(&self, name: &str) -> Result<MeshInfo> {
        let c_name = CString::new(name).map_err(|_| TraceyError("Invalid name".to_string()))?;
        unsafe {
            let mut info = TraceyMeshInfo {
                name: ptr::null(),
                vertex_count: 0,
                triangle_count: 0,
                has_indices: false,
                has_normals: false,
                has_uvs: false,
            };
            check_result(tracey_scene_get_mesh_info(self.ptr, c_name.as_ptr(), &mut info))?;
            Ok(MeshInfo::from(info))
        }
    }

    pub fn get_texture_count(&self) -> u32 {
        unsafe { tracey_scene_get_texture_count(self.ptr) }
    }

    pub fn get_texture_ids(&self) -> Vec<String> {
        let count = self.get_texture_count() as usize;
        if count == 0 {
            return Vec::new();
        }

        let mut id_ptrs = vec![ptr::null::<std::os::raw::c_char>(); count];
        unsafe {
            tracey_scene_get_texture_ids(self.ptr, id_ptrs.as_mut_ptr(), count as u32);
            id_ptrs
                .iter()
                .filter(|p| !p.is_null())
                .map(|&p| CStr::from_ptr(p).to_string_lossy().into_owned())
                .collect()
        }
    }

    pub fn get_texture_info(&self, id: &str) -> Result<TextureInfo> {
        let c_id = CString::new(id).map_err(|_| TraceyError("Invalid id".to_string()))?;
        unsafe {
            let mut info = TraceyTextureInfo {
                id: ptr::null(),
                width: 0,
                height: 0,
                channels: 0,
                mime_type: ptr::null(),
            };
            check_result(tracey_scene_get_texture_info(self.ptr, c_id.as_ptr(), &mut info))?;
            Ok(TextureInfo::from(info))
        }
    }

    // Primitive creation methods
    pub fn add_cube(&mut self, name: &str, size: f32) -> Result<u64> {
        let c_name = CString::new(name).map_err(|_| TraceyError("Invalid name".to_string()))?;
        unsafe {
            let uid = tracey_scene_add_cube(self.ptr, c_name.as_ptr(), size);
            if uid == u64::MAX {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(uid)
            }
        }
    }

    pub fn add_sphere(&mut self, name: &str, radius: f32, segments: u32, rings: u32) -> Result<u64> {
        let c_name = CString::new(name).map_err(|_| TraceyError("Invalid name".to_string()))?;
        unsafe {
            let uid = tracey_scene_add_sphere(self.ptr, c_name.as_ptr(), radius, segments, rings);
            if uid == u64::MAX {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(uid)
            }
        }
    }

    pub fn add_torus(
        &mut self,
        name: &str,
        major_radius: f32,
        minor_radius: f32,
        major_segments: u32,
        minor_segments: u32,
    ) -> Result<u64> {
        let c_name = CString::new(name).map_err(|_| TraceyError("Invalid name".to_string()))?;
        unsafe {
            let uid = tracey_scene_add_torus(
                self.ptr,
                c_name.as_ptr(),
                major_radius,
                minor_radius,
                major_segments,
                minor_segments,
            );
            if uid == u64::MAX {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(uid)
            }
        }
    }

    pub fn add_plane(&mut self, name: &str, width: f32, depth: f32) -> Result<u64> {
        let c_name = CString::new(name).map_err(|_| TraceyError("Invalid name".to_string()))?;
        unsafe {
            let uid = tracey_scene_add_plane(self.ptr, c_name.as_ptr(), width, depth);
            if uid == u64::MAX {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(uid)
            }
        }
    }

    pub fn add_cylinder(&mut self, name: &str, radius: f32, height: f32, segments: u32) -> Result<u64> {
        let c_name = CString::new(name).map_err(|_| TraceyError("Invalid name".to_string()))?;
        unsafe {
            let uid = tracey_scene_add_cylinder(self.ptr, c_name.as_ptr(), radius, height, segments);
            if uid == u64::MAX {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(uid)
            }
        }
    }

    pub fn add_cone(&mut self, name: &str, radius: f32, height: f32, segments: u32) -> Result<u64> {
        let c_name = CString::new(name).map_err(|_| TraceyError("Invalid name".to_string()))?;
        unsafe {
            let uid = tracey_scene_add_cone(self.ptr, c_name.as_ptr(), radius, height, segments);
            if uid == u64::MAX {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(uid)
            }
        }
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

    pub fn update_transforms(&mut self, device: &Device, scene: &Scene) -> Result<()> {
        unsafe {
            let result = tracey_update_scene_transforms(device.as_ptr(), scene.as_ptr(), self.ptr);
            if result < 0 {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(())
            }
        }
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
    pub samples_per_frame: u32,
    pub max_bounces: u32,
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
            samples_per_frame: config.samples_per_frame,
            max_bounces: config.max_bounces,
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

    pub fn get_samples_per_frame(&self) -> u32 {
        unsafe { tracey_path_tracer_get_samples_per_frame(self.ptr as *mut _) }
    }

    pub fn set_samples_per_frame(&mut self, samples: u32) -> Result<()> {
        unsafe {
            check_result(tracey_path_tracer_set_samples_per_frame(self.ptr, samples))
        }
    }

    pub fn get_max_bounces(&self) -> u32 {
        unsafe { tracey_path_tracer_get_max_bounces(self.ptr as *mut _) }
    }

    pub fn set_max_bounces(&mut self, bounces: u32) -> Result<()> {
        unsafe {
            check_result(tracey_path_tracer_set_max_bounces(self.ptr, bounces))
        }
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    pub fn as_ptr(&self) -> *mut TraceyPathTracer {
        self.ptr
    }
}

impl Drop for PathTracer {
    fn drop(&mut self) {
        unsafe {
            tracey_path_tracer_destroy(self.ptr);
        }
    }
}

// ============================================================================
// Rasterizer
// ============================================================================

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RasterizerConfig {
    pub width: u32,
    pub height: u32,
    pub vertex_shader: String,
    pub fragment_shader: String,
    pub use_depth_buffer: bool,
    pub depth_test_enable: bool,
    pub cull_back_faces: bool,
    pub alpha_blending: bool,
}

pub struct Rasterizer {
    ptr: *mut TraceyRasterizer,
    width: u32,
    height: u32,
}

unsafe impl Send for Rasterizer {}
unsafe impl Sync for Rasterizer {}

impl Rasterizer {
    pub fn new(device: &Device, config: &RasterizerConfig) -> Result<Self> {
        let c_vertex = CString::new(config.vertex_shader.as_str())
            .map_err(|_| TraceyError("Invalid shader path".to_string()))?;
        let c_fragment = CString::new(config.fragment_shader.as_str())
            .map_err(|_| TraceyError("Invalid shader path".to_string()))?;

        let c_config = TraceyRasterizerConfig {
            width: config.width,
            height: config.height,
            vertex_shader_path: c_vertex.as_ptr(),
            fragment_shader_path: c_fragment.as_ptr(),
            use_depth_buffer: config.use_depth_buffer,
            depth_test_enable: config.depth_test_enable,
            cull_back_faces: config.cull_back_faces,
            alpha_blending: config.alpha_blending,
        };

        unsafe {
            let ptr = tracey_rasterizer_create(device.as_ptr(), &c_config);
            if ptr.is_null() {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(Self {
                    ptr,
                    width: config.width,
                    height: config.height,
                })
            }
        }
    }

    pub fn render(
        &mut self,
        compiled_scene: &CompiledScene,
        camera: &Camera,
    ) -> Result<f64> {
        unsafe {
            let c: TraceyCamera = camera.clone().into();
            let time = tracey_rasterizer_render(
                self.ptr,
                compiled_scene.as_ptr(),
                &c,
            );
            if time < 0.0 {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(time)
            }
        }
    }

    pub fn readback(&self) -> Result<Vec<u8>> {
        let pixel_size = 4; // R8G8B8A8
        let buffer_size = (self.width * self.height * pixel_size) as usize;
        let mut buffer = vec![0u8; buffer_size];

        unsafe {
            let bytes_written = tracey_rasterizer_readback(
                self.ptr,
                buffer.as_mut_ptr(),
                buffer_size,
            );
            if bytes_written == 0 {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(buffer)
            }
        }
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    pub fn as_ptr(&self) -> *mut TraceyRasterizer {
        self.ptr
    }
}

impl Drop for Rasterizer {
    fn drop(&mut self) {
        unsafe {
            tracey_rasterizer_destroy(self.ptr);
        }
    }
}

// ============================================================================
// Scene Query Types
// ============================================================================

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct InstanceInfo {
    pub object_ref: String,
    pub shader_id: String,
    pub has_local_transform: bool,
    pub local_transform: Option<Transform>,
}

impl From<TraceyInstanceInfo> for InstanceInfo {
    fn from(info: TraceyInstanceInfo) -> Self {
        unsafe {
            let object_ref = if info.object_ref.is_null() {
                String::new()
            } else {
                CStr::from_ptr(info.object_ref).to_string_lossy().into_owned()
            };
            let shader_id = if info.shader_id.is_null() {
                String::new()
            } else {
                CStr::from_ptr(info.shader_id).to_string_lossy().into_owned()
            };
            let local_transform = if info.has_local_transform {
                Some(info.local_transform.into())
            } else {
                None
            };

            InstanceInfo {
                object_ref,
                shader_id,
                has_local_transform: info.has_local_transform,
                local_transform,
            }
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MeshInfo {
    pub name: String,
    pub vertex_count: u32,
    pub triangle_count: u32,
    pub has_indices: bool,
    pub has_normals: bool,
    pub has_uvs: bool,
}

impl From<TraceyMeshInfo> for MeshInfo {
    fn from(info: TraceyMeshInfo) -> Self {
        unsafe {
            let name = if info.name.is_null() {
                String::new()
            } else {
                CStr::from_ptr(info.name).to_string_lossy().into_owned()
            };

            MeshInfo {
                name,
                vertex_count: info.vertex_count,
                triangle_count: info.triangle_count,
                has_indices: info.has_indices,
                has_normals: info.has_normals,
                has_uvs: info.has_uvs,
            }
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TextureInfo {
    pub id: String,
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub mime_type: String,
}

impl From<TraceyTextureInfo> for TextureInfo {
    fn from(info: TraceyTextureInfo) -> Self {
        unsafe {
            let id = if info.id.is_null() {
                String::new()
            } else {
                CStr::from_ptr(info.id).to_string_lossy().into_owned()
            };
            let mime_type = if info.mime_type.is_null() {
                String::new()
            } else {
                CStr::from_ptr(info.mime_type).to_string_lossy().into_owned()
            };

            TextureInfo {
                id,
                width: info.width,
                height: info.height,
                channels: info.channels,
                mime_type,
            }
        }
    }
}

// ============================================================================
// Presenter
// ============================================================================

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct PresenterConfig {
    pub width: u32,
    pub height: u32,
    pub enable_hdr: bool,
    pub desired_image_count: u32,
}

impl Default for PresenterConfig {
    fn default() -> Self {
        Self {
            width: 1280,
            height: 720,
            enable_hdr: false,
            desired_image_count: 3, // Triple buffering
        }
    }
}

impl From<PresenterConfig> for TraceyPresenterConfig {
    fn from(config: PresenterConfig) -> Self {
        TraceyPresenterConfig {
            width: config.width,
            height: config.height,
            enable_hdr: config.enable_hdr,
            desired_image_count: config.desired_image_count,
        }
    }
}

/// Viewport bounds for region-based presentation
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct ViewportBounds {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl From<ViewportBounds> for TraceyViewportBounds {
    fn from(bounds: ViewportBounds) -> Self {
        TraceyViewportBounds {
            x: bounds.x,
            y: bounds.y,
            width: bounds.width,
            height: bounds.height,
        }
    }
}

impl From<TraceyViewportBounds> for ViewportBounds {
    fn from(bounds: TraceyViewportBounds) -> Self {
        ViewportBounds {
            x: bounds.x,
            y: bounds.y,
            width: bounds.width,
            height: bounds.height,
        }
    }
}

/// Safe wrapper for native window presenter
/// Handles direct GPU-to-screen presentation via Vulkan swapchain
pub struct Presenter {
    ptr: *mut TraceyPresenter,
}

unsafe impl Send for Presenter {}
unsafe impl Sync for Presenter {}

impl Presenter {
    /// Create a new presenter for a native window
    ///
    /// # Arguments
    /// * `device` - GPU device (must be Vulkan-based)
    /// * `native_window_handle` - Platform-specific window handle (NSView*, HWND, etc.)
    /// * `native_display_handle` - Platform-specific display handle (or null for macOS)
    /// * `config` - Presenter configuration
    pub fn new(
        device: &Device,
        native_window_handle: *mut std::ffi::c_void,
        native_display_handle: *mut std::ffi::c_void,
        config: &PresenterConfig,
    ) -> Result<Self> {
        unsafe {
            let c_config: TraceyPresenterConfig = (*config).into();
            let ptr = tracey_presenter_create(
                device.as_ptr(),
                native_window_handle,
                native_display_handle,
                &c_config as *const _,
            );

            if ptr.is_null() {
                Err(TraceyError(get_last_error()))
            } else {
                Ok(Self { ptr })
            }
        }
    }

    /// Present PathTracer output to window
    pub fn present_pathtracer(&self, pathtracer: &PathTracer) -> Result<()> {
        unsafe {
            let result = tracey_presenter_present_pathtracer(self.ptr, pathtracer.as_ptr());
            check_result(result)
        }
    }

    /// Present Rasterizer output to window
    pub fn present_rasterizer(&self, rasterizer: &Rasterizer) -> Result<()> {
        unsafe {
            let result = tracey_presenter_present_rasterizer(self.ptr, rasterizer.as_ptr());
            check_result(result)
        }
    }

    /// Present PathTracer output to a specific region of the window
    /// Used for viewport-based rendering with full-window swapchain
    pub fn present_pathtracer_to_region(
        &self,
        pathtracer: &PathTracer,
        bounds: &ViewportBounds,
    ) -> Result<()> {
        unsafe {
            let c_bounds: TraceyViewportBounds = (*bounds).into();
            let result = tracey_presenter_present_pathtracer_to_region(
                self.ptr,
                pathtracer.as_ptr(),
                &c_bounds as *const _,
            );
            check_result(result)
        }
    }

    /// Present Rasterizer output to a specific region of the window
    /// Used for viewport-based rendering with full-window swapchain
    pub fn present_rasterizer_to_region(
        &self,
        rasterizer: &Rasterizer,
        bounds: &ViewportBounds,
    ) -> Result<()> {
        unsafe {
            let c_bounds: TraceyViewportBounds = (*bounds).into();
            let result = tracey_presenter_present_rasterizer_to_region(
                self.ptr,
                rasterizer.as_ptr(),
                &c_bounds as *const _,
            );
            check_result(result)
        }
    }

    /// Resize the presenter's swapchain
    pub fn resize(&self, width: u32, height: u32) -> Result<()> {
        unsafe {
            let result = tracey_presenter_resize(self.ptr, width, height);
            check_result(result)
        }
    }

    /// Wait for all presentation operations to complete
    pub fn wait_idle(&self) {
        unsafe {
            tracey_presenter_wait_idle(self.ptr);
        }
    }

    pub fn as_ptr(&self) -> *mut TraceyPresenter {
        self.ptr
    }
}

impl Drop for Presenter {
    fn drop(&mut self) {
        unsafe {
            tracey_presenter_destroy(self.ptr);
        }
    }
}
