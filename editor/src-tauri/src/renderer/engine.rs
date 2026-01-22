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

        let hdr_pixels = tracer
            .readback()
            .map_err(|e| format!("Readback failed: {}", e))?;

        let width = tracer.width();
        let height = tracer.height();

        // Convert HDR to LDR for display
        let hdr_output = self.config.hdr_output;
        let pixels = if hdr_output {
            Self::hdr_to_ldr_static(&hdr_pixels, sample_count, width, height)
        } else {
            hdr_pixels
        };

        Ok(RenderResult {
            pixels,
            width,
            height,
            sample_count,
            render_time_ms: render_time,
        })
    }

    /// Convert HDR float pixels to LDR u8 pixels with tonemapping
    fn hdr_to_ldr_static(hdr_data: &[u8], sample_count: u32, width: u32, height: u32) -> Vec<u8> {
        let num_pixels = (width * height) as usize;
        let mut ldr = vec![0u8; num_pixels * 4];

        for i in 0..num_pixels {
            // Read HDR pixel as f32
            let offset = i * 16; // 4 floats * 4 bytes
            let r = f32::from_le_bytes([hdr_data[offset], hdr_data[offset + 1], hdr_data[offset + 2], hdr_data[offset + 3]]);
            let g = f32::from_le_bytes([hdr_data[offset + 4], hdr_data[offset + 5], hdr_data[offset + 6], hdr_data[offset + 7]]);
            let b = f32::from_le_bytes([hdr_data[offset + 8], hdr_data[offset + 9], hdr_data[offset + 10], hdr_data[offset + 11]]);
            let a = f32::from_le_bytes([hdr_data[offset + 12], hdr_data[offset + 13], hdr_data[offset + 14], hdr_data[offset + 15]]);

            // Divide by sample count to get average
            let scale = 1.0 / sample_count as f32;
            let r_avg = r * scale;
            let g_avg = g * scale;
            let b_avg = b * scale;

            // Simple clamp to [0, 1] and convert to u8
            let out_offset = i * 4;
            ldr[out_offset] = (r_avg.clamp(0.0, 1.0) * 255.0) as u8;
            ldr[out_offset + 1] = (g_avg.clamp(0.0, 1.0) * 255.0) as u8;
            ldr[out_offset + 2] = (b_avg.clamp(0.0, 1.0) * 255.0) as u8;
            ldr[out_offset + 3] = (a.clamp(0.0, 1.0) * 255.0) as u8;
        }

        ldr
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
