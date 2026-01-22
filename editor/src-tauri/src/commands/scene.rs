//! Scene management Tauri commands

use crate::ffi::{Camera, Transform};
use crate::scene::{Actor, SceneState};
use crate::AppState;
use tauri::State;

#[tauri::command]
pub async fn create_actor(state: State<'_, AppState>, name: String) -> Result<u64, String> {
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.create_actor(name))
}

#[tauri::command]
pub async fn delete_actor(state: State<'_, AppState>, actor_id: u64) -> Result<bool, String> {
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.delete_actor(actor_id))
}

#[tauri::command]
pub async fn get_all_actors(state: State<'_, AppState>) -> Result<Vec<Actor>, String> {
    let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.actors.values().cloned().collect())
}

#[tauri::command]
pub async fn get_actor(state: State<'_, AppState>, actor_id: u64) -> Result<Option<Actor>, String> {
    let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.get_actor(actor_id).cloned())
}

#[tauri::command]
pub async fn set_actor_transform(
    state: State<'_, AppState>,
    actor_id: u64,
    transform: Transform,
) -> Result<bool, String> {
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.set_actor_transform(actor_id, transform))
}

#[tauri::command]
pub async fn set_actor_name(
    state: State<'_, AppState>,
    actor_id: u64,
    name: String,
) -> Result<bool, String> {
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.set_actor_name(actor_id, name))
}

#[tauri::command]
pub async fn set_camera(state: State<'_, AppState>, camera: Camera) -> Result<(), String> {
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    scene.set_camera(camera);
    Ok(())
}

#[tauri::command]
pub async fn get_camera(state: State<'_, AppState>) -> Result<Camera, String> {
    let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.camera.clone())
}

#[tauri::command]
pub async fn add_child(
    state: State<'_, AppState>,
    parent_id: u64,
    child_id: u64,
) -> Result<bool, String> {
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.add_child(parent_id, child_id))
}

#[tauri::command]
pub async fn remove_child(
    state: State<'_, AppState>,
    parent_id: u64,
    child_id: u64,
) -> Result<bool, String> {
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.remove_child(parent_id, child_id))
}
