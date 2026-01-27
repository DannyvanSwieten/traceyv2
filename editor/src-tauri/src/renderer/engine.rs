//! Rendering engine that orchestrates rendering (path tracing and rasterization)

use crate::ffi::{
    Camera, CompiledScene, Device, DeviceBackend, DeviceType, InstanceInfo, MeshInfo, PathTracer,
    PathTracerConfig, Rasterizer, RasterizerConfig, Scene as CppScene, TextureInfo,
};
use crate::scene::SceneState;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

/// Rendering mode selection
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RenderMode {
    /// Real-time rasterization for interactive preview
    Rasterizer,
    /// Path tracing for high-quality final rendering
    PathTracer,
}

pub struct RenderEngine {
    device: Arc<Device>,
    path_tracer: Option<PathTracer>,
    rasterizer: Option<Rasterizer>,
    render_mode: RenderMode,
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
    pub samples_per_frame: u32,
    pub max_bounces: u32,
}

impl RenderEngine {
    pub fn new(config: RenderConfig) -> Result<Self, String> {
        let device = Device::new(DeviceType::Gpu, DeviceBackend::Compute)
            .map_err(|e| format!("Failed to create device: {}", e))?;

        let cpp_scene = CppScene::new().map_err(|e| format!("Failed to create scene: {}", e))?;

        Ok(Self {
            device: Arc::new(device),
            path_tracer: None,
            rasterizer: None,
            render_mode: RenderMode::PathTracer, // Default to path tracer
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
            samples_per_frame: self.config.samples_per_frame,
            max_bounces: self.config.max_bounces,
        };

        let path_tracer = PathTracer::new(&self.device, &pt_config)
            .map_err(|e| format!("Failed to create path tracer: {}", e))?;

        self.path_tracer = Some(path_tracer);
        Ok(())
    }

    /// Initialize rasterizer with shader paths
    /// Uses simple shaders for MoltenVK compatibility (no bindless textures)
    pub fn initialize_rasterizer(&mut self) -> Result<(), String> {
        // Use position-only shaders (compatible with our vertex buffer layout)
        // TODO: Replace with PBR shaders once normals/UVs are added to vertex buffers
        let vertex_shader = self.config.shader_dir.join("rasterizer/position_only.vert.spv");
        let fragment_shader = self.config.shader_dir.join("rasterizer/position_only.frag.spv");

        let rasterizer_config = RasterizerConfig {
            width: self.config.width,
            height: self.config.height,
            vertex_shader: vertex_shader.to_string_lossy().to_string(),
            fragment_shader: fragment_shader.to_string_lossy().to_string(),
            use_depth_buffer: false,
            depth_test_enable: false,
            cull_back_faces: true,
            alpha_blending: false,
        };

        let rasterizer = Rasterizer::new(&self.device, &rasterizer_config)
            .map_err(|e| format!("Failed to create rasterizer: {}", e))?;

        self.rasterizer = Some(rasterizer);
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

    /// Compile the existing C++ scene without syncing from Rust
    /// Use this after loading GLTF directly into the C++ scene
    pub fn compile_scene_no_sync(&mut self) -> Result<(), String> {
        let cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;

        // Compile the C++ scene as-is (don't clear/sync from Rust)
        let compiled = CompiledScene::compile(&self.device, &cpp_scene)
            .map_err(|e| format!("Failed to compile scene: {}", e))?;

        self.compiled_scene = Some(compiled);
        Ok(())
    }

    /// Update only transforms in the compiled scene (fast update for animations)
    /// This rebuilds the TLAS but keeps geometry, materials, and textures
    pub fn update_transforms(&mut self, scene: &SceneState) -> Result<(), String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;

        // Sync transforms to C++ scene
        scene.sync_to_cpp(&mut cpp_scene)?;

        // Update transforms in the existing compiled scene
        let compiled = self.compiled_scene.as_mut()
            .ok_or("Scene not compiled - call compile_scene first".to_string())?;

        compiled.update_transforms(&self.device, &cpp_scene)
            .map_err(|e| format!("Failed to update transforms: {}", e))?;

        Ok(())
    }

    /// Render a frame using the active renderer (path tracer or rasterizer)
    pub fn render_frame(
        &mut self,
        camera: &Camera,
        clear_accumulation: bool,
    ) -> Result<RenderResult, String> {
        // Check scene is compiled before borrowing
        if self.compiled_scene.is_none() {
            return Err("Scene not compiled".to_string());
        }

        match self.render_mode {
            RenderMode::PathTracer => {
                println!("Rendering with path tracer");
                self.render_with_path_tracer(camera, clear_accumulation)
            }
            RenderMode::Rasterizer => {
                println!("Rendering with rasterizer");
                self.render_with_rasterizer(camera)
            }
        }
    }

    /// Render using path tracer
    fn render_with_path_tracer(
        &mut self,
        camera: &Camera,
        clear_accumulation: bool,
    ) -> Result<RenderResult, String> {
        let compiled = self.compiled_scene.as_ref().ok_or("Scene not compiled")?;
        let tracer = self
            .path_tracer
            .as_mut()
            .ok_or("Path tracer not initialized")?;

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

    /// Render using rasterizer (realtime preview)
    fn render_with_rasterizer(
        &mut self,
        camera: &Camera,
    ) -> Result<RenderResult, String> {
        let compiled = self.compiled_scene.as_ref().ok_or("Scene not compiled")?;

        let rasterizer = self
            .rasterizer
            .as_mut()
            .ok_or("Rasterizer not initialized")?;

        let render_time = rasterizer
            .render(compiled, camera)
            .map_err(|e| format!("Rasterizer render failed: {}", e))?;

        let pixels = rasterizer
            .readback()
            .map_err(|e| format!("Rasterizer readback failed: {}", e))?;

        let width = rasterizer.width();
        let height = rasterizer.height();

        Ok(RenderResult {
            pixels,
            width,
            height,
            sample_count: 1, // Rasterizer doesn't accumulate samples
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
            let r = f32::from_le_bytes([
                hdr_data[offset],
                hdr_data[offset + 1],
                hdr_data[offset + 2],
                hdr_data[offset + 3],
            ]);
            let g = f32::from_le_bytes([
                hdr_data[offset + 4],
                hdr_data[offset + 5],
                hdr_data[offset + 6],
                hdr_data[offset + 7],
            ]);
            let b = f32::from_le_bytes([
                hdr_data[offset + 8],
                hdr_data[offset + 9],
                hdr_data[offset + 10],
                hdr_data[offset + 11],
            ]);
            let a = f32::from_le_bytes([
                hdr_data[offset + 12],
                hdr_data[offset + 13],
                hdr_data[offset + 14],
                hdr_data[offset + 15],
            ]);
        }

        ldr
    }

    /// Load a GLTF file into the scene
    pub fn load_gltf(&mut self, scene: &mut SceneState, path: &str) -> Result<(), String> {
        self.load_gltf_with_project(scene, path, None)
    }

    /// Load a GLTF file into the scene with optional project root
    pub fn load_gltf_with_project(
        &mut self,
        scene: &mut SceneState,
        path: &str,
        project_root: Option<&str>,
    ) -> Result<(), String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;

        scene.load_gltf_with_project(&mut cpp_scene, path, project_root)?;
        Ok(())
    }

    /// Add a GLTF file to the scene without clearing existing actors
    pub fn add_gltf_with_project(
        &mut self,
        scene: &mut SceneState,
        path: &str,
        project_root: Option<&str>,
    ) -> Result<(), String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;

        scene.add_gltf_with_project(&mut cpp_scene, path, project_root)?;
        Ok(())
    }

    pub fn get_resolution(&self) -> (u32, u32) {
        (self.config.width, self.config.height)
    }

    pub fn set_resolution(&mut self, width: u32, height: u32) {
        self.config.width = width;
        self.config.height = height;
        // Both renderers need to be recreated with new resolution
        self.path_tracer = None;
        self.rasterizer = None;
    }

    /// Get the current render mode
    pub fn get_render_mode(&self) -> RenderMode {
        self.render_mode
    }

    /// Set the render mode (PathTracer or Rasterizer)
    /// Automatically initializes the selected renderer if not already initialized
    pub fn set_render_mode(&mut self, mode: RenderMode) -> Result<(), String> {
        println!("Setting render mode to: {:?}", mode);
        self.render_mode = mode;

        // Initialize the selected renderer if needed
        match mode {
            RenderMode::PathTracer => {
                if self.path_tracer.is_none() {
                    println!("Initializing path tracer...");
                    self.initialize_path_tracer()?;
                    println!("Path tracer initialized successfully");
                }
            }
            RenderMode::Rasterizer => {
                if self.rasterizer.is_none() {
                    println!("Initializing rasterizer...");
                    self.initialize_rasterizer()?;
                    println!("Rasterizer initialized successfully");
                } else {
                    println!("Rasterizer already initialized");
                }
            }
        }

        println!("Render mode set successfully to: {:?}", mode);
        Ok(())
    }

    pub fn get_samples_per_frame(&self) -> u32 {
        if let Some(ref tracer) = self.path_tracer {
            tracer.get_samples_per_frame()
        } else {
            self.config.samples_per_frame
        }
    }

    pub fn set_samples_per_frame(&mut self, samples: u32) -> Result<(), String> {
        self.config.samples_per_frame = samples;
        if let Some(ref mut tracer) = self.path_tracer {
            tracer
                .set_samples_per_frame(samples)
                .map_err(|e| format!("Failed to set samples per frame: {}", e))?;
        }
        Ok(())
    }

    pub fn get_max_bounces(&self) -> u32 {
        if let Some(ref tracer) = self.path_tracer {
            tracer.get_max_bounces()
        } else {
            self.config.max_bounces
        }
    }

    pub fn set_max_bounces(&mut self, bounces: u32) -> Result<(), String> {
        self.config.max_bounces = bounces;
        if let Some(ref mut tracer) = self.path_tracer {
            tracer
                .set_max_bounces(bounces)
                .map_err(|e| format!("Failed to set max bounces: {}", e))?;
        }
        Ok(())
    }

    // Scene query methods
    pub fn get_actor_name(&self, actor_uid: u64) -> Option<String> {
        let cpp_scene = self.cpp_scene.lock().ok()?;
        cpp_scene.get_actor_name(actor_uid)
    }

    pub fn get_actor_children(&self, actor_uid: u64) -> Vec<u64> {
        if let Ok(cpp_scene) = self.cpp_scene.lock() {
            cpp_scene.get_actor_children(actor_uid)
        } else {
            Vec::new()
        }
    }

    pub fn get_actor_instance_count(&self, actor_uid: u64) -> u32 {
        if let Ok(cpp_scene) = self.cpp_scene.lock() {
            cpp_scene.get_actor_instance_count(actor_uid)
        } else {
            0
        }
    }

    pub fn get_actor_instance(&self, actor_uid: u64, index: u32) -> Result<InstanceInfo, String> {
        let cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;
        cpp_scene
            .get_actor_instance(actor_uid, index)
            .map_err(|e| format!("Failed to get instance: {}", e))
    }

    pub fn get_mesh_count(&self) -> u32 {
        if let Ok(cpp_scene) = self.cpp_scene.lock() {
            cpp_scene.get_mesh_count()
        } else {
            0
        }
    }

    pub fn get_mesh_names(&self) -> Vec<String> {
        if let Ok(cpp_scene) = self.cpp_scene.lock() {
            cpp_scene.get_mesh_names()
        } else {
            Vec::new()
        }
    }

    pub fn get_mesh_info(&self, name: &str) -> Result<MeshInfo, String> {
        let cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;
        cpp_scene
            .get_mesh_info(name)
            .map_err(|e| format!("Failed to get mesh info: {}", e))
    }

    pub fn get_texture_count(&self) -> u32 {
        if let Ok(cpp_scene) = self.cpp_scene.lock() {
            cpp_scene.get_texture_count()
        } else {
            0
        }
    }

    pub fn get_texture_ids(&self) -> Vec<String> {
        if let Ok(cpp_scene) = self.cpp_scene.lock() {
            cpp_scene.get_texture_ids()
        } else {
            Vec::new()
        }
    }

    pub fn get_texture_info(&self, id: &str) -> Result<TextureInfo, String> {
        let cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;
        cpp_scene
            .get_texture_info(id)
            .map_err(|e| format!("Failed to get texture info: {}", e))
    }

    /// Update an actor's transform in the C++ scene
    pub fn set_actor_transform(
        &self,
        actor_uid: u64,
        transform: &crate::ffi::Transform,
    ) -> Result<(), String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;
        cpp_scene
            .set_actor_transform(actor_uid, transform)
            .map_err(|e| format!("Failed to set actor transform: {}", e))
    }

    // Primitive creation methods
    pub fn add_cube(&self, name: &str, size: f32) -> Result<u64, String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;
        cpp_scene
            .add_cube(name, size)
            .map_err(|e| format!("Failed to add cube: {}", e))
    }

    pub fn add_sphere(&self, name: &str, radius: f32, segments: u32, rings: u32) -> Result<u64, String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;
        cpp_scene
            .add_sphere(name, radius, segments, rings)
            .map_err(|e| format!("Failed to add sphere: {}", e))
    }

    pub fn add_torus(
        &self,
        name: &str,
        major_radius: f32,
        minor_radius: f32,
        major_segments: u32,
        minor_segments: u32,
    ) -> Result<u64, String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;
        cpp_scene
            .add_torus(name, major_radius, minor_radius, major_segments, minor_segments)
            .map_err(|e| format!("Failed to add torus: {}", e))
    }

    pub fn add_plane(&self, name: &str, width: f32, depth: f32) -> Result<u64, String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;
        cpp_scene
            .add_plane(name, width, depth)
            .map_err(|e| format!("Failed to add plane: {}", e))
    }

    pub fn add_cylinder(&self, name: &str, radius: f32, height: f32, segments: u32) -> Result<u64, String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;
        cpp_scene
            .add_cylinder(name, radius, height, segments)
            .map_err(|e| format!("Failed to add cylinder: {}", e))
    }

    pub fn add_cone(&self, name: &str, radius: f32, height: f32, segments: u32) -> Result<u64, String> {
        let mut cpp_scene = self
            .cpp_scene
            .lock()
            .map_err(|_| "Failed to lock scene".to_string())?;
        cpp_scene
            .add_cone(name, radius, height, segments)
            .map_err(|e| format!("Failed to add cone: {}", e))
    }

    /// Get device reference (for presenter creation)
    pub fn device(&self) -> &Device {
        &self.device
    }

    /// Get path tracer reference (for direct presentation)
    pub fn pathtracer(&self) -> Option<&PathTracer> {
        self.path_tracer.as_ref()
    }

    /// Get rasterizer reference (for direct presentation)
    pub fn rasterizer(&self) -> Option<&Rasterizer> {
        self.rasterizer.as_ref()
    }
}

pub struct RenderResult {
    pub pixels: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub sample_count: u32,
    pub render_time_ms: f64,
}
