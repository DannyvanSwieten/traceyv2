//! Native window presentation commands
//!
//! Commands for creating and managing native render windows with direct GPU presentation.

use crate::window_manager::native_viewport::ViewportBounds;
use crate::AppState;
use serde::{Deserialize, Serialize};
use tauri::State;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ViewportBoundsDto {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl From<ViewportBoundsDto> for ViewportBounds {
    fn from(dto: ViewportBoundsDto) -> Self {
        ViewportBounds {
            x: dto.x,
            y: dto.y,
            width: dto.width,
            height: dto.height,
        }
    }
}

/// Create or update native viewport
#[tauri::command]
pub async fn create_native_viewport(
    app: tauri::AppHandle,
    state: State<'_, AppState>,
    bounds: ViewportBoundsDto,
) -> Result<(), String> {
    let mut viewport_manager = state
        .viewport_manager
        .lock()
        .map_err(|e| format!("Failed to lock viewport manager: {}", e))?;

    let engine = state
        .engine
        .lock()
        .map_err(|e| format!("Failed to lock engine: {}", e))?;

    let device = engine.device();

    // Get or create native viewport
    let native_viewport = viewport_manager.get_or_create_native_viewport();

    let mut viewport = native_viewport
        .lock()
        .map_err(|e| format!("Failed to lock native viewport: {}", e))?;

    let bounds: ViewportBounds = bounds.into();

    // If not initialized, initialize it with Tauri's window
    if !viewport.is_ready() {
        viewport.initialize_with_tauri_window(&app, device, bounds)?;
    } else {
        // Otherwise just update bounds
        viewport.update_bounds(bounds)?;
    }

    Ok(())
}

/// Update native viewport position/size
#[tauri::command]
pub async fn sync_native_viewport(
    state: State<'_, AppState>,
    bounds: ViewportBoundsDto,
) -> Result<(), String> {
    let viewport_manager = state
        .viewport_manager
        .lock()
        .map_err(|e| format!("Failed to lock viewport manager: {}", e))?;

    if let Some(native_viewport) = viewport_manager.get_native_viewport() {
        let mut viewport = native_viewport
            .lock()
            .map_err(|e| format!("Failed to lock native viewport: {}", e))?;

        let bounds: ViewportBounds = bounds.into();
        viewport.update_bounds(bounds)?;
    }

    Ok(())
}

/// Destroy native viewport
#[tauri::command]
pub async fn destroy_native_viewport(state: State<'_, AppState>) -> Result<(), String> {
    let mut viewport_manager = state
        .viewport_manager
        .lock()
        .map_err(|e| format!("Failed to lock viewport manager: {}", e))?;

    viewport_manager.destroy_native_viewport();
    Ok(())
}

/// Check if native viewport is active
#[tauri::command]
pub async fn has_native_viewport(state: State<'_, AppState>) -> Result<bool, String> {
    let viewport_manager = state
        .viewport_manager
        .lock()
        .map_err(|e| format!("Failed to lock viewport manager: {}", e))?;

    Ok(viewport_manager.has_native_viewport())
}

/// Present PathTracer output to native viewport
#[tauri::command]
pub async fn present_pathtracer(state: State<'_, AppState>) -> Result<(), String> {
    let viewport_manager = state
        .viewport_manager
        .lock()
        .map_err(|e| format!("Failed to lock viewport manager: {}", e))?;

    let engine = state
        .engine
        .lock()
        .map_err(|e| format!("Failed to lock engine: {}", e))?;

    if let Some(native_viewport) = viewport_manager.get_native_viewport() {
        let viewport = native_viewport
            .lock()
            .map_err(|e| format!("Failed to lock native viewport: {}", e))?;

        if let Some(presenter) = viewport.presenter() {
            if let Some(pathtracer) = engine.pathtracer() {
                // Get viewport bounds for region-based presentation
                let bounds = viewport.bounds();
                let ffi_bounds = crate::ffi::ViewportBounds {
                    x: bounds.x,
                    y: bounds.y,
                    width: bounds.width,
                    height: bounds.height,
                };

                // Present to specific region of swapchain
                presenter
                    .present_pathtracer_to_region(pathtracer, &ffi_bounds)
                    .map_err(|e| format!("Failed to present: {}", e))?;
            } else {
                return Err("PathTracer not initialized".to_string());
            }
        } else {
            return Err("Presenter not ready".to_string());
        }
    } else {
        return Err("Native viewport not created".to_string());
    }

    Ok(())
}
