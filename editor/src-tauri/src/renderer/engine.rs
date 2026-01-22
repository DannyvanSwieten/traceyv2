//! Rendering engine that orchestrates path tracing

use crate::ffi::{
    Camera, CompiledScene, Device, DeviceBackend, DeviceType, PathTracer, PathTracerConfig,
    Scene as CppScene,
};
use crate::scene::SceneState;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

pub struct RenderEngine {
    device: Arc<Device>,
    path_tracer: Option<PathTracer>,
    compiled_scene: Option<CompiledScene>,
    cpp_scene: Mutex<CppScene>,
    config: RenderConfig,
}

#[derive(Debug, Clone)]
pub struct RenderConfig {
    pub width: u32,
    pub height: u32,
    pub shader_dir: PathBuf,
    pub hdr_output: bool,
}

impl RenderEngine {
    pub fn new(config: RenderConfig) -> Result<Self, String> {
        let device = Device::new(DeviceType::Gpu, DeviceBackend::Compute)
            .map_err(|e| format!("Failed to create device: {}", e))?;

        let cpp_scene = CppScene::new().map_err(|e| format!("Failed to create scene: {}", e))?;

        Ok(Self {
            device: Arc::new(device),
            path_tracer: None,
            compiled_scene: None,
            cpp_scene: Mutex::new(cpp_scene),
            config,
        })
    }

    /// Initialize path tracer with shader paths
    pub fn initialize_path_tracer(&mut self) -> Result<(), String> {
        let ray_gen = self.config.shader_dir.join("ray_gen.isf");
        let hit = self.config.shader_dir.join("diffuse_hit.isf");
        let miss = self.config.shader_dir.join("sky_miss.isf");
        let resolve = self.config.shader_dir.join("resolve.isf");

        let pt_config = PathTracerConfig {
            width: self.config.width,
            height: self.config.height,
            ray_gen_shader: ray_gen.to_string_lossy().to_string(),
            hit_shader: hit.to_string_lossy().to_string(),
            miss_shader: miss.to_string_lossy().to_string(),
            resolve_shader: Some(resolve.to_string_lossy().to_string()),
            hdr_output: self.config.hdr_output,
        };

        let path_tracer = PathTracer::new(&self.device, &pt_config)
            .map_err(|e| format!("Failed to create path tracer: {}", e))?;

        self.path_tracer = Some(path_tracer);
        Ok(())
    }

    /// Compile the scene for rendering
    pub fn compile_scene(&mut self, scene: &SceneState) -> Result<(), String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;

        // Sync Rust scene to C++ scene
        scene.sync_to_cpp(&mut cpp_scene)?;

        // Compile the C++ scene
        let compiled = CompiledScene::compile(&self.device, &cpp_scene)
            .map_err(|e| format!("Failed to compile scene: {}", e))?;

        self.compiled_scene = Some(compiled);
        Ok(())
    }

    /// Render a frame
    pub fn render_frame(
        &mut self,
        camera: &Camera,
        clear_accumulation: bool,
    ) -> Result<RenderResult, String> {
        let tracer = self
            .path_tracer
            .as_mut()
            .ok_or("Path tracer not initialized")?;

        let compiled = self
            .compiled_scene
            .as_ref()
            .ok_or("Scene not compiled")?;

        let render_time = tracer
            .render(compiled, camera, clear_accumulation)
            .map_err(|e| format!("Render failed: {}", e))?;

        let sample_count = tracer.get_sample_count();

        let pixels = tracer
            .readback()
            .map_err(|e| format!("Readback failed: {}", e))?;

        Ok(RenderResult {
            pixels,
            width: tracer.width(),
            height: tracer.height(),
            sample_count,
            render_time_ms: render_time,
        })
    }

    /// Load a GLTF file into the scene
    pub fn load_gltf(&mut self, scene: &mut SceneState, path: &str) -> Result<(), String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;

        scene.load_gltf(&mut cpp_scene, path)?;
        Ok(())
    }

    pub fn get_resolution(&self) -> (u32, u32) {
        (self.config.width, self.config.height)
    }

    pub fn set_resolution(&mut self, width: u32, height: u32) {
        self.config.width = width;
        self.config.height = height;
        // Path tracer needs to be recreated with new resolution
        self.path_tracer = None;
    }
}

pub struct RenderResult {
    pub pixels: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub sample_count: u32,
    pub render_time_ms: f64,
}
