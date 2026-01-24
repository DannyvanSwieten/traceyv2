// Prevents additional console window on Windows in release
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod commands;
mod ffi;
mod menu;
mod renderer;
mod scene;

use renderer::{RenderConfig, RenderEngine, Viewport};
use scene::SceneState;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

pub struct AppState {
    pub scene: Arc<Mutex<SceneState>>,
    pub engine: Arc<Mutex<RenderEngine>>,
    pub viewport: Arc<Mutex<Viewport>>,
    pub last_render_pixels: Arc<Mutex<Option<Vec<u8>>>>,
}

fn main() {
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
        samples_per_frame: 16,
        max_bounces: 8,
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

    // Build app state
    let app_state = AppState {
        scene: Arc::new(Mutex::new(scene)),
        engine,
        viewport: Arc::new(Mutex::new(viewport)),
        last_render_pixels: Arc::new(Mutex::new(None)),
    };

    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(app_state)
        .setup(|app| {
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
            // Scene resource query commands
            commands::get_actor_instances,
            commands::get_mesh_names,
            commands::get_mesh_info,
            commands::get_all_meshes,
            commands::get_texture_ids,
            commands::get_texture_info,
            commands::get_all_textures,
            // Primitive creation command
            commands::add_primitive,
            // Render commands
            commands::render_frame,
            commands::get_render_pixels,
            commands::get_render_pixels_base64,
            commands::compile_scene,
            commands::get_viewport_resolution,
            commands::set_viewport_resolution,
            commands::get_samples_per_frame,
            commands::set_samples_per_frame,
            commands::get_max_bounces,
            commands::set_max_bounces,
            // IO commands
            commands::save_scene,
            commands::load_scene,
            commands::import_gltf,
            commands::export_image,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
