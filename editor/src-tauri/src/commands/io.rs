//! Import/Export Tauri commands

use crate::AppState;
use tauri::State;

#[tauri::command]
pub async fn save_scene(state: State<'_, AppState>, path: String) -> Result<(), String> {
    let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    scene.save_to_file(&path)
}

#[tauri::command]
pub async fn load_scene(state: State<'_, AppState>, path: String) -> Result<(), String> {
    let loaded_scene = crate::scene::SceneState::load_from_file(&path)?;
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    *scene = loaded_scene;
    Ok(())
}

#[tauri::command]
pub async fn import_gltf(state: State<'_, AppState>, path: String) -> Result<(), String> {
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    let mut engine = state
        .engine
        .lock()
        .map_err(|_| "Failed to lock engine")?;

    engine.load_gltf(&mut scene, &path)?;
    Ok(())
}

#[tauri::command]
pub async fn export_image(
    state: State<'_, AppState>,
    path: String,
    format: String,
) -> Result<(), String> {
    let pixels = state
        .last_render_pixels
        .lock()
        .map_err(|_| "Failed to lock pixels")?;

    let pixels = pixels
        .as_ref()
        .ok_or_else(|| "No render available".to_string())?;

    let viewport = state.viewport.lock().map_err(|_| "Failed to lock viewport")?;
    let (width, height) = viewport.get_resolution()?;

    match format.to_lowercase().as_str() {
        "png" => {
            // For MVP: Just write raw RGBA data
            // In production, use image crate to encode PNG
            std::fs::write(&path, pixels).map_err(|e| format!("Failed to write image: {}", e))?;
            Ok(())
        }
        _ => Err(format!("Unsupported format: {}", format)),
    }
}
