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
    pub transform: Transform,  // Local transform (relative to parent)
    pub children: Vec<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub parent: Option<u64>,   // Parent actor ID
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
            parent: None,
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
        // Add to parent's children list
        if let Some(parent) = self.actors.get_mut(&parent_id) {
            if !parent.children.contains(&child_id) {
                parent.children.push(child_id);
            }
        } else {
            return false;
        }

        // Set child's parent
        if let Some(child) = self.actors.get_mut(&child_id) {
            child.parent = Some(parent_id);
            true
        } else {
            false
        }
    }

    pub fn remove_child(&mut self, parent_id: u64, child_id: u64) -> bool {
        // Remove from parent's children list
        if let Some(parent) = self.actors.get_mut(&parent_id) {
            parent.children.retain(|&id| id != child_id);
        } else {
            return false;
        }

        // Clear child's parent
        if let Some(child) = self.actors.get_mut(&child_id) {
            child.parent = None;
            true
        } else {
            false
        }
    }

    /// Set an actor's parent (automatically updates parent's children list)
    pub fn set_parent(&mut self, actor_id: u64, parent_id: Option<u64>) -> bool {
        // Remove from old parent if exists
        if let Some(actor) = self.actors.get(&actor_id) {
            if let Some(old_parent_id) = actor.parent {
                if let Some(old_parent) = self.actors.get_mut(&old_parent_id) {
                    old_parent.children.retain(|&id| id != actor_id);
                }
            }
        }

        // Set new parent
        if let Some(actor) = self.actors.get_mut(&actor_id) {
            actor.parent = parent_id;
        } else {
            return false;
        }

        // Add to new parent's children list
        if let Some(new_parent_id) = parent_id {
            if let Some(new_parent) = self.actors.get_mut(&new_parent_id) {
                if !new_parent.children.contains(&actor_id) {
                    new_parent.children.push(actor_id);
                }
            } else {
                return false;
            }
        }

        true
    }

    /// Get all root actors (actors with no parent)
    pub fn get_root_actors(&self) -> Vec<u64> {
        self.actors
            .values()
            .filter(|actor| actor.parent.is_none())
            .map(|actor| actor.id)
            .collect()
    }

    /// Reorder a child within its parent's children list
    pub fn reorder_child(&mut self, parent_id: u64, child_id: u64, new_index: usize) -> Result<(), String> {
        if let Some(parent) = self.actors.get_mut(&parent_id) {
            // Find and remove child
            if let Some(old_pos) = parent.children.iter().position(|&id| id == child_id) {
                parent.children.remove(old_pos);
                // Insert at new position (clamped to valid range)
                let insert_pos = new_index.min(parent.children.len());
                parent.children.insert(insert_pos, child_id);
                Ok(())
            } else {
                Err(format!("Child {} not found in parent {}", child_id, parent_id))
            }
        } else {
            Err(format!("Parent actor {} not found", parent_id))
        }
    }

    pub fn set_camera(&mut self, camera: Camera) {
        self.camera = camera;
    }

    /// Compute world transform for an actor by walking up the parent chain
    pub fn compute_world_transform(&self, actor_id: u64) -> Transform {
        let actor = match self.actors.get(&actor_id) {
            Some(a) => a,
            None => return Transform::identity(),
        };

        // If no parent, local transform is world transform
        let parent_id = match actor.parent {
            Some(id) => id,
            None => return actor.transform.clone(),
        };

        // Recursively compute parent's world transform
        let parent_world = self.compute_world_transform(parent_id);

        // Multiply: parent_world * local = world
        parent_world.multiply(&actor.transform)
    }

    /// Sync this Rust scene state to a C++ scene for rendering
    pub fn sync_to_cpp(&self, cpp_scene: &mut CppScene) -> Result<(), String> {
        // Clear the C++ scene first to avoid duplicates
        cpp_scene.clear();

        // Set camera
        cpp_scene
            .set_camera(&self.camera)
            .map_err(|e| format!("Failed to set camera: {}", e))?;

        // Create actors in C++ scene with world transforms
        let mut actor_map = HashMap::new();
        for (id, actor) in &self.actors {
            let cpp_uid = cpp_scene
                .create_actor(&actor.name)
                .map_err(|e| format!("Failed to create actor: {}", e))?;

            // Compute world transform from local transform + parent chain
            let world_transform = self.compute_world_transform(*id);

            cpp_scene
                .set_actor_transform(cpp_uid, &world_transform)
                .map_err(|e| format!("Failed to set transform: {}", e))?;

            actor_map.insert(*id, cpp_uid);
        }

        Ok(())
    }

    /// Load a GLTF file into this scene
    pub fn load_gltf(&mut self, cpp_scene: &mut CppScene, path: &str) -> Result<(), String> {
        self.load_gltf_with_project(cpp_scene, path, None)
    }

    /// Load a GLTF file into this scene with optional project root
    pub fn load_gltf_with_project(
        &mut self,
        cpp_scene: &mut CppScene,
        path: &str,
        project_root: Option<&str>,
    ) -> Result<(), String> {
        // Clear existing scene state
        self.actors.clear();
        self.next_actor_id = 1;

        cpp_scene
            .load_gltf_with_project(path, project_root)
            .map_err(|e| format!("Failed to load GLTF: {}", e))?;

        // Sync back from C++ to Rust
        self.sync_from_cpp(cpp_scene)?;

        Ok(())
    }

    /// Add a GLTF file to this scene without clearing existing actors
    pub fn add_gltf_with_project(
        &mut self,
        cpp_scene: &mut CppScene,
        path: &str,
        project_root: Option<&str>,
    ) -> Result<(), String> {
        // Don't sync_to_cpp here! The C++ scene should already be up to date
        // from the previous compile_scene call. If we sync here, we'll create
        // duplicate actors in the C++ scene.

        println!("add_gltf_with_project: Before adding, Rust scene has {} actors", self.actors.len());

        // Add GLTF to C++ scene (this adds to existing actors without clearing)
        cpp_scene
            .add_gltf_with_project(path, project_root)
            .map_err(|e| format!("Failed to add GLTF: {}", e))?;

        println!("add_gltf_with_project: After adding to C++, syncing back to Rust...");

        // Sync back from C++ to Rust (this will include both old and new actors)
        self.sync_from_cpp(cpp_scene)?;

        println!("add_gltf_with_project: After sync, Rust scene has {} actors", self.actors.len());

        Ok(())
    }

    /// Sync from C++ scene to this Rust scene state
    pub fn sync_from_cpp(&mut self, cpp_scene: &CppScene) -> Result<(), String> {
        // Get camera
        if let Ok(camera) = cpp_scene.get_camera() {
            self.camera = camera;
        }

        // Get actors - first pass: create/update actors with transforms and children
        let uids = cpp_scene.get_actor_uids();
        println!("sync_from_cpp: Found {} actors", uids.len());

        for uid in uids {
            if let Ok(transform) = cpp_scene.get_actor_transform(uid) {
                // Get children from C++ scene
                let children = cpp_scene.get_actor_children(uid);
                let name = cpp_scene.get_actor_name(uid).unwrap_or_else(|| format!("Actor_{}", uid));

                println!("  Actor {}: name='{}', children={:?}", uid, name, children);

                if let Some(actor) = self.actors.get_mut(&uid) {
                    actor.name = name;
                    actor.transform = transform;
                    actor.children = children.clone();
                    actor.parent = None; // Will set in second pass
                } else {
                    // Create new actor
                    let actor = Actor {
                        id: uid,
                        name,
                        transform,
                        children: children.clone(),
                        parent: None, // Will set in second pass
                    };
                    self.actors.insert(uid, actor);

                    // Update next_actor_id if needed
                    if uid >= self.next_actor_id {
                        self.next_actor_id = uid + 1;
                    }
                }
            }
        }

        // Second pass: set parent relationships based on children
        println!("sync_from_cpp: Setting parent relationships...");
        let mut parent_child_pairs = Vec::new();
        for actor in self.actors.values() {
            for &child_id in &actor.children {
                parent_child_pairs.push((child_id, actor.id));
            }
        }

        for (child_id, parent_id) in parent_child_pairs {
            if let Some(child) = self.actors.get_mut(&child_id) {
                println!("  Setting actor {} parent to {}", child_id, parent_id);
                child.parent = Some(parent_id);
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
