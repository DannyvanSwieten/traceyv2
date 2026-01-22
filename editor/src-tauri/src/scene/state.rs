//! Rust-owned scene state (source of truth)
//!
//! The scene state lives in Rust for easy serialization and manipulation.
//! It syncs to C++ only when rendering is needed.

use crate::ffi::{Camera, Scene as CppScene, Transform, Vec3};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SceneState {
    pub actors: HashMap<u64, Actor>,
    pub camera: Camera,
    next_actor_id: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Actor {
    pub id: u64,
    pub name: String,
    pub transform: Transform,
    pub children: Vec<u64>,
}

impl SceneState {
    pub fn new() -> Self {
        Self {
            actors: HashMap::new(),
            camera: Camera::default(),
            next_actor_id: 1,
        }
    }

    pub fn create_actor(&mut self, name: String) -> u64 {
        let id = self.next_actor_id;
        self.next_actor_id += 1;

        let actor = Actor {
            id,
            name,
            transform: Transform::identity(),
            children: Vec::new(),
        };

        self.actors.insert(id, actor);
        id
    }

    pub fn get_actor(&self, id: u64) -> Option<&Actor> {
        self.actors.get(&id)
    }

    pub fn get_actor_mut(&mut self, id: u64) -> Option<&mut Actor> {
        self.actors.get_mut(&id)
    }

    pub fn delete_actor(&mut self, id: u64) -> bool {
        self.actors.remove(&id).is_some()
    }

    pub fn set_actor_transform(&mut self, id: u64, transform: Transform) -> bool {
        if let Some(actor) = self.actors.get_mut(&id) {
            actor.transform = transform;
            true
        } else {
            false
        }
    }

    pub fn set_actor_name(&mut self, id: u64, name: String) -> bool {
        if let Some(actor) = self.actors.get_mut(&id) {
            actor.name = name;
            true
        } else {
            false
        }
    }

    pub fn add_child(&mut self, parent_id: u64, child_id: u64) -> bool {
        if let Some(parent) = self.actors.get_mut(&parent_id) {
            if !parent.children.contains(&child_id) {
                parent.children.push(child_id);
            }
            true
        } else {
            false
        }
    }

    pub fn remove_child(&mut self, parent_id: u64, child_id: u64) -> bool {
        if let Some(parent) = self.actors.get_mut(&parent_id) {
            parent.children.retain(|&id| id != child_id);
            true
        } else {
            false
        }
    }

    pub fn set_camera(&mut self, camera: Camera) {
        self.camera = camera;
    }

    /// Sync this Rust scene state to a C++ scene for rendering
    pub fn sync_to_cpp(&self, cpp_scene: &mut CppScene) -> Result<(), String> {
        // Set camera
        cpp_scene
            .set_camera(&self.camera)
            .map_err(|e| format!("Failed to set camera: {}", e))?;

        // Create actors in C++ scene
        let mut actor_map = HashMap::new();
        for (id, actor) in &self.actors {
            let cpp_uid = cpp_scene
                .create_actor(&actor.name)
                .map_err(|e| format!("Failed to create actor: {}", e))?;

            cpp_scene
                .set_actor_transform(cpp_uid, &actor.transform)
                .map_err(|e| format!("Failed to set transform: {}", e))?;

            actor_map.insert(*id, cpp_uid);
        }

        Ok(())
    }

    /// Load a GLTF file into this scene
    pub fn load_gltf(&mut self, cpp_scene: &mut CppScene, path: &str) -> Result<(), String> {
        // Clear existing scene state
        self.actors.clear();
        self.next_actor_id = 1;

        cpp_scene
            .load_gltf(path)
            .map_err(|e| format!("Failed to load GLTF: {}", e))?;

        // Sync back from C++ to Rust
        self.sync_from_cpp(cpp_scene)?;

        Ok(())
    }

    /// Sync from C++ scene to this Rust scene state
    pub fn sync_from_cpp(&mut self, cpp_scene: &CppScene) -> Result<(), String> {
        // Get camera
        if let Ok(camera) = cpp_scene.get_camera() {
            self.camera = camera;
        }

        // Get actors
        let uids = cpp_scene.get_actor_uids();
        for uid in uids {
            if let Ok(transform) = cpp_scene.get_actor_transform(uid) {
                if let Some(actor) = self.actors.get_mut(&uid) {
                    actor.transform = transform;
                } else {
                    // Create new actor
                    let actor = Actor {
                        id: uid,
                        name: format!("Actor_{}", uid),
                        transform,
                        children: Vec::new(),
                    };
                    self.actors.insert(uid, actor);

                    // Update next_actor_id if needed
                    if uid >= self.next_actor_id {
                        self.next_actor_id = uid + 1;
                    }
                }
            }
        }

        Ok(())
    }

    /// Save scene to JSON file
    pub fn save_to_file(&self, path: &str) -> Result<(), String> {
        let json = serde_json::to_string_pretty(self)
            .map_err(|e| format!("Failed to serialize: {}", e))?;
        std::fs::write(path, json).map_err(|e| format!("Failed to write file: {}", e))?;
        Ok(())
    }

    /// Load scene from JSON file
    pub fn load_from_file(path: &str) -> Result<Self, String> {
        let json =
            std::fs::read_to_string(path).map_err(|e| format!("Failed to read file: {}", e))?;
        let scene: SceneState = serde_json::from_str(&json)
            .map_err(|e| format!("Failed to deserialize: {}", e))?;
        Ok(scene)
    }
}

impl Default for SceneState {
    fn default() -> Self {
        Self::new()
    }
}
