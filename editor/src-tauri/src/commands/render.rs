//! Rendering Tauri commands

use crate::ffi::Camera;
use crate::renderer::RenderMode;
use crate::{AppState, SceneCommand};
use tauri::State;

#[derive(serde::Serialize)]
pub struct RenderResultDto {
    pub width: u32,
    pub height: u32,
    pub sample_count: u32,
    pub render_time_ms: f64,
}

/// Update the camera - the render loop will use this for subsequent frames
/// This is the primary way to control rendering - just update the camera and
/// the render loop will automatically render with the new camera.
#[tauri::command]
pub async fn update_camera(
    state: State<'_, AppState>,
    camera: Camera,
) -> Result<(), String> {
    state
        .scene_command_tx
        .send(SceneCommand::SetCamera(camera))
        .map_err(|_| "Failed to send camera update")?;
    Ok(())
}

/// Force clear the accumulation buffer (e.g., after scene changes)
#[tauri::command]
pub async fn clear_accumulation(state: State<'_, AppState>) -> Result<(), String> {
    state
        .scene_command_tx
        .send(SceneCommand::ClearAccumulation)
        .map_err(|_| "Failed to send clear accumulation")?;
    Ok(())
}

#[tauri::command]
pub async fn get_render_pixels(state: State<'_, AppState>) -> Result<Vec<u8>, String> {
    let pixels = state
        .last_render_pixels
        .lock()
        .map_err(|_| "Failed to lock pixels")?;

    pixels
        .as_ref()
        .cloned()
        .ok_or_else(|| "No render available".to_string())
}

#[tauri::command]
pub async fn get_render_pixels_base64(state: State<'_, AppState>) -> Result<String, String> {
    let pixels = state
        .last_render_pixels
        .lock()
        .map_err(|_| "Failed to lock pixels")?;

    pixels
        .as_ref()
        .map(|p| base64::encode(p))
        .ok_or_else(|| "No render available".to_string())
}

/// Compile scene - routes through render loop channel
#[tauri::command]
pub async fn compile_scene(state: State<'_, AppState>) -> Result<(), String> {
    // Clone scene data and send to render loop
    let scene_snapshot = {
        let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
        scene.clone()
    };

    state
        .scene_command_tx
        .send(SceneCommand::CompileScene(scene_snapshot))
        .map_err(|_| "Failed to send compile command")?;
    Ok(())
}

/// Compile scene without sync - routes through render loop channel
#[tauri::command]
pub async fn compile_scene_no_sync(state: State<'_, AppState>) -> Result<(), String> {
    state
        .scene_command_tx
        .send(SceneCommand::Recompile)
        .map_err(|_| "Failed to send recompile command")?;
    Ok(())
}

/// Update scene transforms - routes through render loop channel
#[tauri::command]
pub async fn update_scene_transforms(state: State<'_, AppState>) -> Result<(), String> {
    // Clone scene data and send compile command (which will sync transforms)
    let scene_snapshot = {
        let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
        scene.clone()
    };

    state
        .scene_command_tx
        .send(SceneCommand::CompileScene(scene_snapshot))
        .map_err(|_| "Failed to send compile command")?;
    Ok(())
}

#[tauri::command]
pub async fn get_viewport_resolution(state: State<'_, AppState>) -> Result<(u32, u32), String> {
    let viewport = state.viewport.lock().map_err(|_| "Failed to lock viewport")?;
    viewport.get_resolution()
}

#[tauri::command]
pub async fn set_viewport_resolution(
    state: State<'_, AppState>,
    width: u32,
    height: u32,
) -> Result<(), String> {
    let viewport = state.viewport.lock().map_err(|_| "Failed to lock viewport")?;
    viewport.set_resolution(width, height)
}

/// Get resolution scale factor
#[tauri::command]
pub async fn get_resolution_scale(state: State<'_, AppState>) -> Result<f32, String> {
    let engine = state.engine.try_lock().map_err(|_| "Engine busy")?;
    Ok(engine.get_resolution_scale())
}

/// Set resolution scale factor (0.25, 0.5, 0.75, 1.0)
/// Triggers recompile to apply new resolution
#[tauri::command]
pub async fn set_resolution_scale(state: State<'_, AppState>, scale: f32) -> Result<(), String> {
    {
        let mut engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        engine.set_resolution_scale(scale);
    }
    // Trigger recompile to apply new resolution
    let _ = state.scene_command_tx.send(crate::SceneCommand::Recompile);
    Ok(())
}

/// Get maximum render resolution
#[tauri::command]
pub async fn get_max_resolution(state: State<'_, AppState>) -> Result<(u32, u32), String> {
    let engine = state.engine.try_lock().map_err(|_| "Engine busy")?;
    Ok(engine.get_max_resolution())
}

/// Set maximum render resolution (0 = no limit)
#[tauri::command]
pub async fn set_max_resolution(
    state: State<'_, AppState>,
    width: u32,
    height: u32,
) -> Result<(), String> {
    {
        let mut engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        engine.set_max_resolution(width, height);
    }
    Ok(())
}

/// Update render resolution based on viewport size
/// Returns the effective render resolution
#[tauri::command]
pub async fn update_viewport_size(
    state: State<'_, AppState>,
    viewport_width: u32,
    viewport_height: u32,
) -> Result<(u32, u32), String> {
    let resolution_changed;
    let effective_resolution;

    {
        let mut engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        resolution_changed = engine.update_resolution_for_viewport(viewport_width, viewport_height);
        effective_resolution = engine.get_resolution();
    }

    // Trigger recompile if resolution changed
    if resolution_changed {
        let _ = state.scene_command_tx.send(crate::SceneCommand::Recompile);
    }

    Ok(effective_resolution)
}

/// Get samples per frame - uses try_lock to avoid blocking render loop
#[tauri::command]
pub async fn get_samples_per_frame(state: State<'_, AppState>) -> Result<u32, String> {
    let engine = state
        .engine
        .try_lock()
        .map_err(|_| "Engine busy")?;
    Ok(engine.get_samples_per_frame())
}

/// Set samples per frame - routes through render loop channel
#[tauri::command]
pub async fn set_samples_per_frame(
    state: State<'_, AppState>,
    samples: u32,
) -> Result<(), String> {
    state
        .scene_command_tx
        .send(SceneCommand::SetSamplesPerFrame(samples))
        .map_err(|_| "Failed to send command")?;
    Ok(())
}

/// Get max bounces - uses try_lock to avoid blocking render loop
#[tauri::command]
pub async fn get_max_bounces(state: State<'_, AppState>) -> Result<u32, String> {
    let engine = state
        .engine
        .try_lock()
        .map_err(|_| "Engine busy")?;
    Ok(engine.get_max_bounces())
}

/// Set max bounces - routes through render loop channel
#[tauri::command]
pub async fn set_max_bounces(
    state: State<'_, AppState>,
    bounces: u32,
) -> Result<(), String> {
    state
        .scene_command_tx
        .send(SceneCommand::SetMaxBounces(bounces))
        .map_err(|_| "Failed to send command")?;
    Ok(())
}

/// Get max accumulated samples - reads from atomic (lock-free)
#[tauri::command]
pub async fn get_max_samples(state: State<'_, AppState>) -> Result<u32, String> {
    Ok(state.max_samples.load(std::sync::atomic::Ordering::Relaxed))
}

/// Set max accumulated samples - updates atomic and routes through render loop channel
#[tauri::command]
pub async fn set_max_samples(
    state: State<'_, AppState>,
    samples: u32,
) -> Result<(), String> {
    // Update the atomic directly for immediate effect
    state.max_samples.store(samples.max(1), std::sync::atomic::Ordering::Relaxed);
    // Also send command to render loop (for any additional handling like clearing)
    state
        .scene_command_tx
        .send(SceneCommand::SetMaxSamples(samples))
        .map_err(|_| "Failed to send command")?;
    Ok(())
}

/// Get render mode - uses try_lock to avoid blocking render loop
#[tauri::command]
pub async fn get_render_mode(state: State<'_, AppState>) -> Result<String, String> {
    let engine = state
        .engine
        .try_lock()
        .map_err(|_| "Engine busy")?;

    let mode = match engine.get_render_mode() {
        RenderMode::PathTracer => "PathTracer",
        RenderMode::Rasterizer => "Rasterizer",
    };

    Ok(mode.to_string())
}

/// Set render mode - uses try_lock (render mode changes are infrequent)
#[tauri::command]
pub async fn set_render_mode(
    state: State<'_, AppState>,
    mode: String,
) -> Result<(), String> {
    let mut engine = state
        .engine
        .try_lock()
        .map_err(|_| "Engine busy")?;

    let render_mode = match mode.as_str() {
        "PathTracer" => RenderMode::PathTracer,
        "Rasterizer" => RenderMode::Rasterizer,
        _ => return Err(format!("Invalid render mode: {}", mode)),
    };

    engine.set_render_mode(render_mode)
}

/// Set HDR environment map for the scene
/// @param path Path to HDR file, or null/empty to clear
/// @param intensity Brightness multiplier (default 1.0)
/// @param rotation Horizontal rotation in radians (default 0.0)
#[tauri::command]
pub async fn set_environment_map(
    state: State<'_, AppState>,
    path: Option<String>,
    intensity: f32,
    rotation: f32,
) -> Result<(), String> {
    state
        .scene_command_tx
        .send(SceneCommand::SetEnvironmentMap(path, intensity, rotation))
        .map_err(|_| "Failed to send environment map command")?;
    Ok(())
}
