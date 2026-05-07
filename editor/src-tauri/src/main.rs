// Prevents additional console window on Windows in release
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

// Enable objc macros (msg_send!, sel!, etc.) for macOS platform
#[cfg(target_os = "macos")]
#[macro_use]
extern crate objc;

mod commands;
mod ffi;
mod menu;
mod render_loop;
mod renderer;
mod scene;
mod project;
mod native_window;
mod window_manager;

use renderer::{RenderConfig, RenderEngine, RenderMode, Viewport};
use scene::SceneState;
use project::{ProjectState, RecentProjects};
use window_manager::ViewportManager;
use crate::ffi::Transform;
use crate::commands::scene::PrimitiveParams;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use tauri::Manager;
use tokio::sync::mpsc;

/// Commands that can be queued for the render thread to process
#[derive(Debug, Clone)]
pub enum SceneCommand {
    /// Update an actor's transform (actor_id, world_transform)
    SetTransform(u64, Transform),
    /// Add a primitive to the scene (actor_id, name, params)
    AddPrimitive(u64, String, PrimitiveParams),
    /// Delete an actor and its children (actor_ids to remove from C++)
    DeleteActors(Vec<u64>),
    /// Request scene recompile
    Recompile,
    /// Update the camera for rendering
    SetCamera(ffi::Camera),
    /// Clear accumulation buffer (e.g., when camera moves)
    ClearAccumulation,
    /// Set samples per frame
    SetSamplesPerFrame(u32),
    /// Set max bounces
    SetMaxBounces(u32),
    /// Compile scene with Rust state sync
    CompileScene(SceneState),
}

pub struct AppState {
    pub scene: Arc<Mutex<SceneState>>,
    pub engine: Arc<Mutex<RenderEngine>>,
    pub viewport: Arc<Mutex<Viewport>>,
    pub last_render_pixels: Arc<Mutex<Option<Vec<u8>>>>,
    pub project: Arc<Mutex<Option<ProjectState>>>,
    pub recent_projects: Arc<Mutex<RecentProjects>>,
    pub app_data_dir: PathBuf,
    pub viewport_manager: Arc<Mutex<ViewportManager>>,
    /// MPSC channel sender for scene commands - lock-free sends from any thread
    pub scene_command_tx: mpsc::UnboundedSender<SceneCommand>,
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            // Determine shader directory
            let shader_dir = if cfg!(debug_assertions) {
                // In development, use examples/scene_renderer/shaders
                PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                    .parent()
                    .unwrap()
                    .parent()
                    .unwrap()
                    .join("examples/scene_renderer/shaders")
            } else {
                // In production, use bundled shaders
                PathBuf::from("shaders")
            };

            println!("Using shader directory: {:?}", shader_dir);

            // Initialize render engine
            let config = RenderConfig {
                width: 1280,
                height: 720,
                shader_dir,
                hdr_output: false, // LDR - tonemapping done in resolve shader
                samples_per_frame: 4,
                max_bounces: 2,
            };

            let mut engine = RenderEngine::new(config).expect("Failed to create render engine");

            engine
                .initialize_path_tracer()
                .expect("Failed to initialize path tracer");

            let engine = Arc::new(Mutex::new(engine));

            // Create viewport
            let viewport = Viewport::new(Arc::clone(&engine));

            // Create scene state
            let scene = SceneState::new();

            // Get app data directory
            let app_data_dir = app
                .path()
                .app_data_dir()
                .expect("Failed to get app data directory");

            // Load recent projects
            let recent_projects = RecentProjects::load(&app_data_dir);

            // Create MPSC channel for scene commands
            let (scene_command_tx, scene_command_rx) = mpsc::unbounded_channel::<SceneCommand>();

            // Create shared state for pixels (render loop writes, frontend reads)
            let last_render_pixels = Arc::new(Mutex::new(None));

            // Create viewport manager (shared between render loop and commands)
            let viewport_manager = Arc::new(Mutex::new(ViewportManager::new()));

            // Start the dedicated render loop thread
            render_loop::start_render_loop(
                app.handle().clone(),
                Arc::clone(&engine),
                scene_command_rx,
                Arc::clone(&last_render_pixels),
                Arc::clone(&viewport_manager),
            );

            // Build app state
            let app_state = AppState {
                scene: Arc::new(Mutex::new(scene)),
                engine,
                viewport: Arc::new(Mutex::new(viewport)),
                last_render_pixels,
                project: Arc::new(Mutex::new(None)),
                recent_projects: Arc::new(Mutex::new(recent_projects)),
                app_data_dir,
                viewport_manager,
                scene_command_tx,
            };

            app.manage(app_state);
            menu::setup(app)?;
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            // Scene commands
            commands::create_actor,
            commands::delete_actor,
            commands::get_all_actors,
            commands::get_actor,
            commands::set_actor_transform,
            commands::set_actor_name,
            commands::set_camera,
            commands::get_camera,
            commands::add_child,
            commands::remove_child,
            commands::set_actor_parent,
            commands::get_root_actors,
            commands::get_world_transform,
            commands::reorder_child,
            // Scene resource query commands
            commands::get_actor_instances,
            commands::get_mesh_names,
            commands::get_mesh_info,
            commands::get_all_meshes,
            commands::get_texture_ids,
            commands::get_texture_info,
            commands::get_all_textures,
            // Material editing commands
            commands::get_instance_material_shader,
            commands::get_material_property,
            commands::get_material_property_count,
            commands::set_material_float,
            commands::set_material_vec3,
            commands::set_material_vec4,
            commands::set_material_texture,
            // Primitive creation command
            commands::add_primitive,
            // Render commands (render loop runs continuously, these control it)
            commands::update_camera,
            commands::clear_accumulation,
            commands::get_render_pixels,
            commands::get_render_pixels_base64,
            commands::compile_scene,
            commands::compile_scene_no_sync,
            commands::update_scene_transforms,
            commands::get_viewport_resolution,
            commands::set_viewport_resolution,
            commands::get_samples_per_frame,
            commands::set_samples_per_frame,
            commands::get_max_bounces,
            commands::set_max_bounces,
            commands::get_render_mode,
            commands::set_render_mode,
            // IO commands
            commands::save_scene,
            commands::load_scene,
            commands::import_gltf,
            commands::add_gltf_to_scene,
            commands::export_image,
            // Project commands
            commands::project_new,
            commands::project_open,
            commands::project_save,
            commands::project_import_asset,
            commands::project_check_shaders,
            commands::get_recent_projects,
            commands::remove_recent_project,
            commands::get_project_info,
            // Presentation commands
            commands::create_native_viewport,
            commands::sync_native_viewport,
            commands::destroy_native_viewport,
            commands::has_native_viewport,
            commands::present_pathtracer,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
