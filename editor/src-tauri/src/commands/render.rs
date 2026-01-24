//! Rendering Tauri commands

use crate::ffi::Camera;
use crate::renderer::RenderResult;
use crate::AppState;
use tauri::State;

#[derive(serde::Serialize)]
pub struct RenderResultDto {
    pub width: u32,
    pub height: u32,
    pub sample_count: u32,
    pub render_time_ms: f64,
    // Pixels are sent separately via base64 or shared memory
}

#[tauri::command]
pub async fn render_frame(
    state: State<'_, AppState>,
    camera: Camera,
    clear_accumulation: bool,
) -> Result<RenderResultDto, String> {
    let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    let viewport = state.viewport.lock().map_err(|_| "Failed to lock viewport")?;

    let result = viewport.render(&scene, &camera, clear_accumulation)?;

    // Store pixels in state for later retrieval
    *state
        .last_render_pixels
        .lock()
        .map_err(|_| "Failed to lock pixels")? = Some(result.pixels);

    Ok(RenderResultDto {
        width: result.width,
        height: result.height,
        sample_count: result.sample_count,
        render_time_ms: result.render_time_ms,
    })
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

#[tauri::command]
pub async fn compile_scene(state: State<'_, AppState>) -> Result<(), String> {
    let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    let mut engine = state
        .engine
        .lock()
        .map_err(|_| "Failed to lock engine")?;

    engine.compile_scene(&scene)?;
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

#[tauri::command]
pub async fn get_samples_per_frame(state: State<'_, AppState>) -> Result<u32, String> {
    let engine = state
        .engine
        .lock()
        .map_err(|_| "Failed to lock engine")?;
    Ok(engine.get_samples_per_frame())
}

#[tauri::command]
pub async fn set_samples_per_frame(
    state: State<'_, AppState>,
    samples: u32,
) -> Result<(), String> {
    let mut engine = state
        .engine
        .lock()
        .map_err(|_| "Failed to lock engine")?;
    engine.set_samples_per_frame(samples)
}

#[tauri::command]
pub async fn get_max_bounces(state: State<'_, AppState>) -> Result<u32, String> {
    let engine = state
        .engine
        .lock()
        .map_err(|_| "Failed to lock engine")?;
    Ok(engine.get_max_bounces())
}

#[tauri::command]
pub async fn set_max_bounces(
    state: State<'_, AppState>,
    bounces: u32,
) -> Result<(), String> {
    let mut engine = state
        .engine
        .lock()
        .map_err(|_| "Failed to lock engine")?;
    engine.set_max_bounces(bounces)
}
