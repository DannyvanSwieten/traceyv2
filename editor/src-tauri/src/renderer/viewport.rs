//! Viewport with shared memory framebuffer

use super::engine::{RenderEngine, RenderResult};
use crate::ffi::Camera;
use crate::scene::SceneState;
use std::sync::{Arc, Mutex};

pub struct Viewport {
    engine: Arc<Mutex<RenderEngine>>,
    shared_memory_name: String,
}

impl Viewport {
    pub fn new(engine: Arc<Mutex<RenderEngine>>) -> Self {
        // Generate unique shared memory name
        let shared_memory_name = format!("tracey_viewport_{}", std::process::id());

        Self {
            engine,
            shared_memory_name,
        }
    }

    /// Render to internal buffer (for MVP - no shared memory yet)
    pub fn render(
        &self,
        scene: &SceneState,
        camera: &Camera,
        clear_accumulation: bool,
    ) -> Result<RenderResult, String> {
        let mut engine = self
            .engine
            .lock()
            .map_err(|_| "Failed to lock engine".to_string())?;

        // Scene should be compiled explicitly via compile_scene command
        // Not on every camera movement - that would be very slow!

        engine.render_frame(camera, clear_accumulation)
    }

    pub fn shared_memory_name(&self) -> &str {
        &self.shared_memory_name
    }

    pub fn get_resolution(&self) -> Result<(u32, u32), String> {
        let engine = self
            .engine
            .lock()
            .map_err(|_| "Failed to lock engine".to_string())?;
        Ok(engine.get_resolution())
    }

    pub fn set_resolution(&self, width: u32, height: u32) -> Result<(), String> {
        let mut engine = self
            .engine
            .lock()
            .map_err(|_| "Failed to lock engine".to_string())?;
        engine.set_resolution(width, height);
        Ok(())
    }
}
