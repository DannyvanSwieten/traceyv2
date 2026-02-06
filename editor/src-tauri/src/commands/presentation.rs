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
/// Also updates render resolution if viewport size changed significantly
/// If window_width/window_height are provided, also resizes the swapchain if needed
#[tauri::command]
pub async fn sync_native_viewport(
    state: State<'_, AppState>,
    bounds: ViewportBoundsDto,
    window_width: Option<u32>,
    window_height: Option<u32>,
) -> Result<(), String> {
    println!(
        "[sync_native_viewport] bounds: ({}, {}, {}x{}), window: {:?}x{:?}",
        bounds.x, bounds.y, bounds.width, bounds.height,
        window_width, window_height
    );

    // Resize swapchain if window size changed
    if let (Some(w), Some(h)) = (window_width, window_height) {
        let viewport_manager = state
            .viewport_manager
            .lock()
            .map_err(|e| format!("Failed to lock viewport manager: {}", e))?;

        if let Some(native_viewport) = viewport_manager.get_native_viewport() {
            let mut viewport = native_viewport
                .lock()
                .map_err(|e| format!("Failed to lock native viewport: {}", e))?;

            // This will resize the swapchain if size actually changed
            viewport.resize_if_needed(w, h)?;
        }
    }

    // Update viewport bounds
    {
        let viewport_manager = state
            .viewport_manager
            .lock()
            .map_err(|e| format!("Failed to lock viewport manager: {}", e))?;

        if let Some(native_viewport) = viewport_manager.get_native_viewport() {
            let mut viewport = native_viewport
                .lock()
                .map_err(|e| format!("Failed to lock native viewport: {}", e))?;

            let bounds_native: ViewportBounds = bounds.clone().into();
            viewport.update_bounds(bounds_native)?;
        }
    }

    // Check if resolution needs to be updated based on new viewport size
    let resolution_changed = {
        let mut engine = state
            .engine
            .lock()
            .map_err(|e| format!("Failed to lock engine: {}", e))?;

        let (current_w, current_h) = engine.get_resolution();
        let (new_w, new_h) = engine.calculate_render_resolution(bounds.width, bounds.height);

        // Only update if resolution changed by more than 10% to avoid constant recreation
        let w_diff = (new_w as i32 - current_w as i32).abs() as f32 / current_w.max(1) as f32;
        let h_diff = (new_h as i32 - current_h as i32).abs() as f32 / current_h.max(1) as f32;

        if w_diff > 0.1 || h_diff > 0.1 {
            engine.update_resolution_for_viewport(bounds.width, bounds.height)
        } else {
            false
        }
    };

    // Trigger recompile if resolution changed
    if resolution_changed {
        let _ = state.scene_command_tx.send(crate::SceneCommand::Recompile);
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
                let window_size = viewport.window_size();
                let (render_w, render_h) = engine.get_resolution();

                println!(
                    "[present_pathtracer] render: {}x{}, viewport bounds: ({}, {}, {}x{}), window: {}x{}",
                    render_w, render_h,
                    bounds.x, bounds.y, bounds.width, bounds.height,
                    window_size.0, window_size.1
                );

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
