//! Window Manager
//!
//! Coordinates the transparent Tauri webview with native render windows
//! for direct GPU presentation.

pub mod native_viewport;

pub use native_viewport::NativeViewport;

use std::sync::{Arc, Mutex};

/// Manages the relationship between webview and native render windows
pub struct ViewportManager {
    native_viewport: Option<Arc<Mutex<NativeViewport>>>,
}

impl ViewportManager {
    pub fn new() -> Self {
        Self {
            native_viewport: None,
        }
    }

    /// Get or create the native viewport
    pub fn get_or_create_native_viewport(&mut self) -> Arc<Mutex<NativeViewport>> {
        if let Some(viewport) = &self.native_viewport {
            viewport.clone()
        } else {
            let viewport = Arc::new(Mutex::new(NativeViewport::new()));
            self.native_viewport = Some(viewport.clone());
            viewport
        }
    }

    /// Get the native viewport if it exists
    pub fn get_native_viewport(&self) -> Option<Arc<Mutex<NativeViewport>>> {
        self.native_viewport.clone()
    }

    /// Destroy the native viewport
    pub fn destroy_native_viewport(&mut self) {
        self.native_viewport = None;
    }

    /// Check if native viewport is active
    pub fn has_native_viewport(&self) -> bool {
        self.native_viewport.is_some()
    }
}

impl Default for ViewportManager {
    fn default() -> Self {
        Self::new()
    }
}
