//! Scene management Tauri commands

use crate::ffi::{Camera, InstanceInfo, MeshInfo, TextureInfo, Transform, Vec3, Quat};
use crate::scene::{Actor, SceneState};
use crate::AppState;
use serde::{Deserialize, Serialize};
use tauri::State;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum PrimitiveParams {
    #[serde(rename = "cube")]
    Cube { size: Option<f32> },
    #[serde(rename = "sphere")]
    Sphere {
        radius: Option<f32>,
        segments: Option<u32>,
        rings: Option<u32>,
    },
    #[serde(rename = "torus")]
    Torus {
        major_radius: Option<f32>,
        minor_radius: Option<f32>,
        major_segments: Option<u32>,
        minor_segments: Option<u32>,
    },
    #[serde(rename = "plane")]
    Plane {
        width: Option<f32>,
        depth: Option<f32>,
    },
    #[serde(rename = "cylinder")]
    Cylinder {
        radius: Option<f32>,
        height: Option<f32>,
        segments: Option<u32>,
    },
    #[serde(rename = "cone")]
    Cone {
        radius: Option<f32>,
        height: Option<f32>,
        segments: Option<u32>,
    },
}

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
    // Update Rust scene state (local transform)
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    let updated = scene.set_actor_transform(actor_id, transform.clone());

    // Compute world transform and update C++ scene
    if updated {
        let world_transform = scene.compute_world_transform(actor_id);
        drop(scene); // Release scene lock before acquiring engine lock

        let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        engine.set_actor_transform(actor_id, &world_transform)?;
    }

    Ok(updated)
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

#[tauri::command]
pub async fn set_actor_parent(
    state: State<'_, AppState>,
    actor_id: u64,
    parent_id: Option<u64>,
) -> Result<bool, String> {
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.set_parent(actor_id, parent_id))
}

#[tauri::command]
pub async fn get_root_actors(state: State<'_, AppState>) -> Result<Vec<u64>, String> {
    let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.get_root_actors())
}

#[tauri::command]
pub async fn get_world_transform(
    state: State<'_, AppState>,
    actor_id: u64,
) -> Result<Transform, String> {
    let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    Ok(scene.compute_world_transform(actor_id))
}

// ============================================================================
// Scene Resource Query Commands
// ============================================================================

#[tauri::command]
pub async fn get_actor_instances(
    state: State<'_, AppState>,
    actor_id: u64,
) -> Result<Vec<InstanceInfo>, String> {
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let count = engine.get_actor_instance_count(actor_id);
    let mut instances = Vec::new();
    for i in 0..count {
        if let Ok(info) = engine.get_actor_instance(actor_id, i) {
            instances.push(info);
        }
    }
    Ok(instances)
}

#[tauri::command]
pub async fn get_mesh_names(state: State<'_, AppState>) -> Result<Vec<String>, String> {
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    Ok(engine.get_mesh_names())
}

#[tauri::command]
pub async fn get_mesh_info(
    state: State<'_, AppState>,
    name: String,
) -> Result<MeshInfo, String> {
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    engine.get_mesh_info(&name)
}

#[tauri::command]
pub async fn get_all_meshes(state: State<'_, AppState>) -> Result<Vec<MeshInfo>, String> {
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let names = engine.get_mesh_names();
    let mut meshes = Vec::new();
    for name in names {
        if let Ok(info) = engine.get_mesh_info(&name) {
            meshes.push(info);
        }
    }
    Ok(meshes)
}

#[tauri::command]
pub async fn get_texture_ids(state: State<'_, AppState>) -> Result<Vec<String>, String> {
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    Ok(engine.get_texture_ids())
}

#[tauri::command]
pub async fn get_texture_info(
    state: State<'_, AppState>,
    id: String,
) -> Result<TextureInfo, String> {
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    engine.get_texture_info(&id)
}

#[tauri::command]
pub async fn get_all_textures(state: State<'_, AppState>) -> Result<Vec<TextureInfo>, String> {
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let ids = engine.get_texture_ids();
    let mut textures = Vec::new();
    for id in ids {
        if let Ok(info) = engine.get_texture_info(&id) {
            textures.push(info);
        }
    }
    Ok(textures)
}

// ============================================================================
// Primitive Creation Commands
// ============================================================================

#[tauri::command]
pub async fn add_primitive(
    state: State<'_, AppState>,
    name: String,
    params: PrimitiveParams,
) -> Result<Actor, String> {
    // Add primitive to C++ scene
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    let actor_uid = match params {
        PrimitiveParams::Cube { size } => {
            engine.add_cube(&name, size.unwrap_or(1.0))?
        }
        PrimitiveParams::Sphere { radius, segments, rings } => {
            engine.add_sphere(&name, radius.unwrap_or(1.0), segments.unwrap_or(16), rings.unwrap_or(16))?
        }
        PrimitiveParams::Torus { major_radius, minor_radius, major_segments, minor_segments } => {
            engine.add_torus(
                &name,
                major_radius.unwrap_or(1.0),
                minor_radius.unwrap_or(0.3),
                major_segments.unwrap_or(32),
                minor_segments.unwrap_or(16),
            )?
        }
        PrimitiveParams::Plane { width, depth } => {
            engine.add_plane(&name, width.unwrap_or(1.0), depth.unwrap_or(1.0))?
        }
        PrimitiveParams::Cylinder { radius, height, segments } => {
            engine.add_cylinder(&name, radius.unwrap_or(0.5), height.unwrap_or(1.0), segments.unwrap_or(32))?
        }
        PrimitiveParams::Cone { radius, height, segments } => {
            engine.add_cone(&name, radius.unwrap_or(0.5), height.unwrap_or(1.0), segments.unwrap_or(32))?
        }
    };
    drop(engine);

    // Add to Rust scene state
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
    let actor = Actor {
        id: actor_uid,
        name: name.clone(),
        transform: Transform {
            position: Vec3::zero(),
            rotation: Quat::identity(),
            scale: Vec3::one(),
        },
        children: Vec::new(),
        parent: None,
    };
    scene.actors.insert(actor_uid, actor.clone());

    Ok(actor)
}
