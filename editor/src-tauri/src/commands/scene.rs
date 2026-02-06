//! Scene management Tauri commands

use crate::ffi::{self, Camera, InstanceInfo, MeshInfo, TextureInfo, Transform, Vec3, Vec4, Quat, MaterialProperty, MaterialPropertyValue};
use crate::scene::{Actor, ActorInstance};
use crate::AppState;

/// Convert cached ActorInstance to API-compatible InstanceInfo
fn instance_to_info(instance: &ActorInstance) -> InstanceInfo {
    InstanceInfo {
        object_ref: instance.mesh_name.clone(),
        shader_id: instance.shader_id.clone(),
        has_local_transform: instance.local_transform.is_some(),
        local_transform: instance.local_transform.clone(),
    }
}
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
    // Collect all actor IDs to delete (parent + children) BEFORE deleting from Rust
    let actors_to_delete: Vec<u64>;
    let removed: bool;

    {
        let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
        let mut ids = Vec::new();
        collect_actor_tree(&scene, actor_id, &mut ids);
        actors_to_delete = ids;

        // Remove from Rust scene state (this will recursively remove children)
        removed = scene.delete_actor(actor_id);
    }

    if removed && !actors_to_delete.is_empty() {
        // Also remove corresponding ActorNodes from the scene-level node graph
        // ActorNodes have the same UID as their corresponding Actors
        {
            let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
            if let Some(scene_ptr) = engine.get_scene_ptr() {
                unsafe {
                    let graph = ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                    if !graph.is_null() {
                        for &actor_uid in &actors_to_delete {
                            // Remove the ActorNode (ignore errors - node might not exist)
                            let _ = ffi::raw::tracey_node_graph_remove_node(graph, actor_uid);
                        }
                    }
                }
            }
        }

        // Send delete command through channel - render loop will handle it
        // This ensures operations are queued and processed in order
        let _ = state.scene_command_tx.send(crate::SceneCommand::DeleteActors(actors_to_delete));
    }

    Ok(removed)
}

// Helper function to recursively collect actor and all its children
fn collect_actor_tree(scene: &crate::scene::SceneState, actor_id: u64, result: &mut Vec<u64>) {
    if let Some(actor) = scene.actors.get(&actor_id) {
        result.push(actor_id);
        for &child_id in &actor.children {
            collect_actor_tree(scene, child_id, result);
        }
    }
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
    // Update Rust scene state and collect transforms to queue
    let transforms_to_queue: Vec<(u64, Transform)>;
    let updated: bool;

    {
        let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
        updated = scene.set_actor_transform(actor_id, transform.clone());

        if updated {
            // Collect all transforms that need updating (actor and children)
            fn collect_transforms(
                scene: &crate::scene::SceneState,
                actor_id: u64,
                result: &mut Vec<(u64, Transform)>,
            ) {
                let world_transform = scene.compute_world_transform(actor_id);
                result.push((actor_id, world_transform));

                if let Some(actor) = scene.get_actor(actor_id) {
                    for &child_id in &actor.children {
                        collect_transforms(scene, child_id, result);
                    }
                }
            }

            let mut transforms = Vec::new();
            collect_transforms(&scene, actor_id, &mut transforms);
            transforms_to_queue = transforms;
        } else {
            transforms_to_queue = Vec::new();
        }
    }

    // Also update the ActorNode's transform in the node graph
    // This ensures transforms survive tracey_scene_sync_from_node_graph
    if updated {
        let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        if let Some(scene_ptr) = engine.get_scene_ptr() {
            unsafe {
                let scene_graph = crate::ffi::raw::tracey_scene_get_node_graph(scene_ptr);
                if !scene_graph.is_null() {
                    let actor_node = crate::ffi::raw::tracey_node_graph_get_node(scene_graph, actor_id);
                    if !actor_node.is_null() {
                        // Convert Transform to TraceyTransform
                        let tracey_transform = crate::ffi::raw::TraceyTransform {
                            position: crate::ffi::raw::TraceyVec3 {
                                x: transform.position.x,
                                y: transform.position.y,
                                z: transform.position.z,
                            },
                            rotation: crate::ffi::raw::TraceyQuat {
                                w: transform.rotation.w,
                                x: transform.rotation.x,
                                y: transform.rotation.y,
                                z: transform.rotation.z,
                            },
                            scale: crate::ffi::raw::TraceyVec3 {
                                x: transform.scale.x,
                                y: transform.scale.y,
                                z: transform.scale.z,
                            },
                        };
                        crate::ffi::raw::tracey_actor_node_set_transform(actor_node, &tracey_transform);
                    }
                }
            }
        }
    }

    // Send transforms through the channel - render thread will process them
    // This is lock-free - senders never block
    for (id, world_transform) in transforms_to_queue {
        let _ = state.scene_command_tx.send(crate::SceneCommand::SetTransform(id, world_transform));
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

#[tauri::command]
pub async fn reorder_child(
    state: State<'_, AppState>,
    parent_id: Option<u64>,
    child_id: u64,
    new_index: usize,
) -> Result<(), String> {
    let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;

    if let Some(pid) = parent_id {
        // Reorder within parent's children
        scene.reorder_child(pid, child_id, new_index)?;
    } else {
        // Reordering root actors not yet supported
        return Err("Reordering root actors not yet supported".to_string());
    }

    Ok(())
}

// ============================================================================
// Scene Resource Query Commands
// ============================================================================

// Scene resource query commands use try_lock to avoid blocking while rendering
// If the engine is busy, these return an error and the frontend can retry later

#[tauri::command]
pub async fn get_actor_instances(
    state: State<'_, AppState>,
    actor_id: u64,
) -> Result<Vec<InstanceInfo>, String> {
    // Query from C++ scene to get instances created via node graph
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    if let Some(scene_ptr) = engine.get_scene_ptr() {
        let mut instances = Vec::new();
        unsafe {
            let count = ffi::raw::tracey_scene_get_actor_instance_count(scene_ptr, actor_id);
            for i in 0..count {
                let mut info = std::mem::zeroed::<ffi::raw::TraceyInstanceInfo>();
                let result = ffi::raw::tracey_scene_get_actor_instance(
                    scene_ptr,
                    actor_id,
                    i,
                    &mut info,
                );
                if result == ffi::raw::TraceyResult::Success {
                    instances.push(InstanceInfo::from(info));
                }
            }
        }
        Ok(instances)
    } else {
        // Fallback to Rust state if engine not available
        let scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
        if let Some(actor) = scene.actors.get(&actor_id) {
            Ok(actor.instances.iter().map(instance_to_info).collect())
        } else {
            Ok(Vec::new())
        }
    }
}

#[tauri::command]
pub async fn get_mesh_names(state: State<'_, AppState>) -> Result<Vec<String>, String> {
    let engine = state.engine.try_lock().map_err(|_| "Engine busy")?;
    Ok(engine.get_mesh_names())
}

#[tauri::command]
pub async fn get_mesh_info(
    state: State<'_, AppState>,
    name: String,
) -> Result<MeshInfo, String> {
    let engine = state.engine.try_lock().map_err(|_| "Engine busy")?;
    engine.get_mesh_info(&name)
}

#[tauri::command]
pub async fn get_all_meshes(state: State<'_, AppState>) -> Result<Vec<MeshInfo>, String> {
    let engine = state.engine.try_lock().map_err(|_| "Engine busy")?;
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
    let engine = state.engine.try_lock().map_err(|_| "Engine busy")?;
    Ok(engine.get_texture_ids())
}

#[tauri::command]
pub async fn get_texture_info(
    state: State<'_, AppState>,
    id: String,
) -> Result<TextureInfo, String> {
    let engine = state.engine.try_lock().map_err(|_| "Engine busy")?;
    engine.get_texture_info(&id)
}

#[tauri::command]
pub async fn get_all_textures(state: State<'_, AppState>) -> Result<Vec<TextureInfo>, String> {
    let engine = state.engine.try_lock().map_err(|_| "Engine busy")?;
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
    // Derive mesh name from primitive type (matches C++ naming convention)
    let mesh_name = match &params {
        PrimitiveParams::Cube { .. } => format!("{}_cube_mesh", name),
        PrimitiveParams::Sphere { .. } => format!("{}_sphere_mesh", name),
        PrimitiveParams::Torus { .. } => format!("{}_torus_mesh", name),
        PrimitiveParams::Plane { .. } => format!("{}_plane_mesh", name),
        PrimitiveParams::Cylinder { .. } => format!("{}_cylinder_mesh", name),
        PrimitiveParams::Cone { .. } => format!("{}_cone_mesh", name),
    };

    // Create instance info (primitives use default shader)
    let instance = ActorInstance {
        mesh_name,
        shader_id: "default".to_string(),
        local_transform: None,
    };

    // Create actor in Rust scene state first - this gives us the ID immediately
    let actor = {
        let mut scene = state.scene.lock().map_err(|_| "Failed to lock scene")?;
        let actor_id = scene.create_actor(name.clone());
        let actor = Actor {
            id: actor_id,
            name: name.clone(),
            transform: Transform {
                position: Vec3::zero(),
                rotation: Quat::identity(),
                scale: Vec3::one(),
            },
            children: Vec::new(),
            parent: None,
            instances: vec![instance],
        };
        scene.actors.insert(actor_id, actor.clone());
        actor
    };

    // Create the primitive in C++ synchronously (blocking lock)
    // This ensures the primitive exists when material properties are queried
    {
        let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        match &params {
            PrimitiveParams::Cube { size } => {
                engine.add_cube_with_id(actor.id, &name, size.unwrap_or(1.0))?;
            }
            PrimitiveParams::Sphere { radius, segments, rings } => {
                engine.add_sphere_with_id(
                    actor.id,
                    &name,
                    radius.unwrap_or(1.0),
                    segments.unwrap_or(16),
                    rings.unwrap_or(16),
                )?;
            }
            PrimitiveParams::Torus { major_radius, minor_radius, major_segments, minor_segments } => {
                engine.add_torus_with_id(
                    actor.id,
                    &name,
                    major_radius.unwrap_or(1.0),
                    minor_radius.unwrap_or(0.3),
                    major_segments.unwrap_or(32),
                    minor_segments.unwrap_or(16),
                )?;
            }
            PrimitiveParams::Plane { width, depth } => {
                engine.add_plane_with_id(
                    actor.id,
                    &name,
                    width.unwrap_or(1.0),
                    depth.unwrap_or(1.0),
                )?;
            }
            PrimitiveParams::Cylinder { radius, height, segments } => {
                engine.add_cylinder_with_id(
                    actor.id,
                    &name,
                    radius.unwrap_or(0.5),
                    height.unwrap_or(1.0),
                    segments.unwrap_or(32),
                )?;
            }
            PrimitiveParams::Cone { radius, height, segments } => {
                engine.add_cone_with_id(
                    actor.id,
                    &name,
                    radius.unwrap_or(0.5),
                    height.unwrap_or(1.0),
                    segments.unwrap_or(32),
                )?;
            }
        }
    }

    // Send recompile command to render loop (so it compiles the new geometry)
    let _ = state.scene_command_tx.send(crate::SceneCommand::Recompile);

    Ok(actor)
}

// ============================================================================
// Material Editing Commands
// ============================================================================

/// Get the material shader ID for an actor's instance
#[tauri::command]
pub async fn get_instance_material_shader(
    state: State<'_, AppState>,
    actor_id: u64,
    instance_index: u32,
) -> Result<Option<String>, String> {
    // Use blocking lock - material queries should wait for render to finish
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    Ok(engine.get_instance_material_shader_id(actor_id, instance_index))
}

/// Get a specific material property by name
#[tauri::command]
pub async fn get_material_property(
    state: State<'_, AppState>,
    actor_id: u64,
    instance_index: u32,
    property_name: String,
) -> Result<MaterialProperty, String> {
    // Use blocking lock - material queries should wait for render to finish
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    engine
        .get_instance_material_property(actor_id, instance_index, &property_name)
        .map_err(|e| e.to_string())
}

/// Get the count of material properties for an actor's instance
#[tauri::command]
pub async fn get_material_property_count(
    state: State<'_, AppState>,
    actor_id: u64,
    instance_index: u32,
) -> Result<u32, String> {
    // Use blocking lock - material queries should wait for render to finish
    let engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
    Ok(engine.get_instance_material_property_count(actor_id, instance_index))
}

/// Set a float material property (e.g., metallic, roughness)
#[tauri::command]
pub async fn set_material_float(
    state: State<'_, AppState>,
    actor_id: u64,
    instance_index: u32,
    property_name: String,
    value: f32,
) -> Result<(), String> {
    {
        // Use blocking lock - material edits should wait for render to finish
        let mut engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        engine
            .set_instance_material_float(actor_id, instance_index, &property_name, value)
            .map_err(|e| e.to_string())?;
        // Sync material changes to GPU
        engine.update_materials().map_err(|e| e.to_string())?;
    }

    // Clear accumulation since material changed
    let _ = state.scene_command_tx.send(crate::SceneCommand::ClearAccumulation);
    Ok(())
}

/// Set a vec3 material property (e.g., albedo, emission color)
#[tauri::command]
pub async fn set_material_vec3(
    state: State<'_, AppState>,
    actor_id: u64,
    instance_index: u32,
    property_name: String,
    value: Vec3,
) -> Result<(), String> {
    {
        // Use blocking lock - material edits should wait for render to finish
        let mut engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        engine
            .set_instance_material_vec3(actor_id, instance_index, &property_name, value)
            .map_err(|e| e.to_string())?;
        // Sync material changes to GPU
        engine.update_materials().map_err(|e| e.to_string())?;
    }

    // Clear accumulation since material changed
    let _ = state.scene_command_tx.send(crate::SceneCommand::ClearAccumulation);
    Ok(())
}

/// Set a vec4 material property
#[tauri::command]
pub async fn set_material_vec4(
    state: State<'_, AppState>,
    actor_id: u64,
    instance_index: u32,
    property_name: String,
    value: Vec4,
) -> Result<(), String> {
    {
        // Use blocking lock - material edits should wait for render to finish
        let mut engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        engine
            .set_instance_material_vec4(actor_id, instance_index, &property_name, value)
            .map_err(|e| e.to_string())?;
        // Sync material changes to GPU
        engine.update_materials().map_err(|e| e.to_string())?;
    }

    // Clear accumulation since material changed
    let _ = state.scene_command_tx.send(crate::SceneCommand::ClearAccumulation);
    Ok(())
}

/// Set a texture material property (e.g., albedo_texture, normal_map)
/// Note: Texture changes require a full scene recompile to take effect
#[tauri::command]
pub async fn set_material_texture(
    state: State<'_, AppState>,
    actor_id: u64,
    instance_index: u32,
    property_name: String,
    texture_path: String,
) -> Result<(), String> {
    {
        // Use blocking lock - material edits should wait for render to finish
        let mut engine = state.engine.lock().map_err(|_| "Failed to lock engine")?;
        engine
            .set_instance_material_texture(actor_id, instance_index, &property_name, &texture_path)
            .map_err(|e| e.to_string())?;
        // Note: Textures need a full recompile to take effect (not just update_materials)
        // For now, just sync scalar properties
        engine.update_materials().map_err(|e| e.to_string())?;
    }

    // Clear accumulation since material changed
    let _ = state.scene_command_tx.send(crate::SceneCommand::ClearAccumulation);
    Ok(())
}
