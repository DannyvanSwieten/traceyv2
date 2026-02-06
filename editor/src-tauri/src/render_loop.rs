//! Dedicated render loop that runs on a single thread
//!
//! This module processes scene commands and renders frames continuously.
//! The frontend sends commands via MPSC channel and receives render results via Tauri events.

use crate::commands::scene::PrimitiveParams;
use crate::ffi::{Camera, ViewportBounds};
use crate::renderer::RenderEngine;
use crate::scene::SceneState;
use crate::window_manager::ViewportManager;
use crate::SceneCommand;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Emitter};
use tokio::sync::mpsc;

/// Render result sent to frontend via Tauri event
#[derive(Clone, serde::Serialize)]
pub struct RenderEvent {
    pub width: u32,
    pub height: u32,
    pub sample_count: u32,
    pub render_time_ms: f64,
}

/// Starts the render loop as a background task
pub fn start_render_loop(
    app_handle: AppHandle,
    engine: Arc<Mutex<RenderEngine>>,
    mut command_rx: mpsc::UnboundedReceiver<SceneCommand>,
    last_render_pixels: Arc<Mutex<Option<Vec<u8>>>>,
    viewport_manager: Arc<Mutex<ViewportManager>>,
    max_samples: Arc<AtomicU32>,
) {
    // Spawn the render loop on a dedicated thread (not tokio - we don't want async)
    std::thread::spawn(move || {
        let mut current_camera = Camera::default();
        let mut clear_accumulation = true;
        let mut current_sample_count: u32 = 0;

        loop {
            // Drain all pending commands (non-blocking)
            let mut needs_recompile = false;
            while let Ok(cmd) = command_rx.try_recv() {
                match cmd {
                    SceneCommand::SetCamera(camera) => {
                        current_camera = camera;
                        clear_accumulation = true;
                    }
                    SceneCommand::ClearAccumulation => {
                        clear_accumulation = true;
                    }
                    SceneCommand::SetTransform(actor_id, transform) => {
                        // Process immediately with engine lock
                        if let Ok(mut engine) = engine.lock() {
                            let _ = engine.set_actor_transform(actor_id, &transform);
                        }
                        clear_accumulation = true;
                    }
                    SceneCommand::AddPrimitive(actor_id, name, params) => {
                        if let Ok(mut engine) = engine.lock() {
                            let result = match params {
                                PrimitiveParams::Cube { size } => {
                                    engine.add_cube_with_id(actor_id, &name, size.unwrap_or(1.0))
                                }
                                PrimitiveParams::Sphere { radius, segments, rings } => {
                                    engine.add_sphere_with_id(
                                        actor_id,
                                        &name,
                                        radius.unwrap_or(1.0),
                                        segments.unwrap_or(16),
                                        rings.unwrap_or(16),
                                    )
                                }
                                PrimitiveParams::Torus {
                                    major_radius,
                                    minor_radius,
                                    major_segments,
                                    minor_segments,
                                } => engine.add_torus_with_id(
                                    actor_id,
                                    &name,
                                    major_radius.unwrap_or(1.0),
                                    minor_radius.unwrap_or(0.3),
                                    major_segments.unwrap_or(32),
                                    minor_segments.unwrap_or(16),
                                ),
                                PrimitiveParams::Plane { width, depth } => {
                                    engine.add_plane_with_id(
                                        actor_id,
                                        &name,
                                        width.unwrap_or(1.0),
                                        depth.unwrap_or(1.0),
                                    )
                                }
                                PrimitiveParams::Cylinder { radius, height, segments } => {
                                    engine.add_cylinder_with_id(
                                        actor_id,
                                        &name,
                                        radius.unwrap_or(0.5),
                                        height.unwrap_or(1.0),
                                        segments.unwrap_or(32),
                                    )
                                }
                                PrimitiveParams::Cone { radius, height, segments } => {
                                    engine.add_cone_with_id(
                                        actor_id,
                                        &name,
                                        radius.unwrap_or(0.5),
                                        height.unwrap_or(1.0),
                                        segments.unwrap_or(32),
                                    )
                                }
                            };
                            if result.is_ok() {
                                needs_recompile = true;
                            }
                        }
                    }
                    SceneCommand::DeleteActors(actor_ids) => {
                        if let Ok(mut engine) = engine.lock() {
                            let _ = engine.remove_actors(&actor_ids);
                            needs_recompile = true;
                        }
                    }
                    SceneCommand::Recompile => {
                        needs_recompile = true;
                    }
                    SceneCommand::SetSamplesPerFrame(samples) => {
                        if let Ok(mut engine) = engine.lock() {
                            let _ = engine.set_samples_per_frame(samples);
                        }
                        clear_accumulation = true;
                    }
                    SceneCommand::SetMaxBounces(bounces) => {
                        if let Ok(mut engine) = engine.lock() {
                            let _ = engine.set_max_bounces(bounces);
                        }
                        clear_accumulation = true;
                    }
                    SceneCommand::SetMaxSamples(samples) => {
                        let new_max = samples.max(1); // Minimum of 1
                        max_samples.store(new_max, Ordering::Relaxed);
                        // If current samples exceed new max, clear to restart
                        if current_sample_count > new_max {
                            clear_accumulation = true;
                        }
                    }
                    SceneCommand::CompileScene(scene_state) => {
                        if let Ok(mut engine) = engine.lock() {
                            let _ = engine.compile_scene(&scene_state);
                        }
                        clear_accumulation = true;
                    }
                    SceneCommand::SetEnvironmentMap(path, intensity, rotation) => {
                        if let Ok(mut engine) = engine.lock() {
                            let _ = engine.set_environment_map(path.as_deref(), intensity, rotation);
                        }
                        needs_recompile = true;
                    }
                }
            }

            // Recompile if needed (short lock)
            if needs_recompile {
                if let Ok(mut engine) = engine.lock() {
                    // compile_scene_no_sync handles GPU synchronization internally
                    let _ = engine.compile_scene_no_sync();
                    clear_accumulation = true;
                }
            }

            // Reset sample count when accumulation is cleared
            if clear_accumulation {
                current_sample_count = 0;
            }

            // Check if we have a native viewport (quick check, separate lock)
            let has_native_viewport = viewport_manager
                .lock()
                .map(|vm| vm.has_native_viewport())
                .unwrap_or(false);

            // Render frame (separate lock scope to minimize lock duration)
            // Skip if we've reached max samples
            let current_max = max_samples.load(Ordering::Relaxed);
            let render_result = {
                if current_sample_count >= current_max {
                    // Already at max samples, skip rendering but still yield
                    std::thread::sleep(std::time::Duration::from_millis(16));
                    None
                } else if let Ok(mut engine) = engine.lock() {
                    let need_pixels = !has_native_viewport;
                    Some(engine.render_frame(&current_camera, clear_accumulation, need_pixels))
                } else {
                    None
                }
            };

            // Process render result (no engine lock needed)
            if let Some(Ok(result)) = render_result {
                // Track sample count for max samples limit
                current_sample_count = result.sample_count;

                // Store pixels for retrieval (only if we did readback)
                if !has_native_viewport {
                    if let Ok(mut pixels) = last_render_pixels.lock() {
                        *pixels = Some(result.pixels);
                    }
                }

                // Present to native viewport if available (separate lock scope)
                if has_native_viewport {
                    if let Ok(engine) = engine.lock() {
                        if let Some(pathtracer) = engine.pathtracer() {
                            if let Ok(vm) = viewport_manager.lock() {
                                if let Some(native_viewport) = vm.get_native_viewport() {
                                    if let Ok(viewport) = native_viewport.lock() {
                                        if let Some(presenter) = viewport.presenter() {
                                            let bounds = viewport.bounds();
                                            let ffi_bounds = ViewportBounds {
                                                x: bounds.x,
                                                y: bounds.y,
                                                width: bounds.width,
                                                height: bounds.height,
                                            };
                                            let _ = presenter.present_pathtracer_to_region(pathtracer, &ffi_bounds);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Emit event to frontend (no lock needed)
                let _ = app_handle.emit(
                    "render-complete",
                    RenderEvent {
                        width: result.width,
                        height: result.height,
                        sample_count: result.sample_count,
                        render_time_ms: result.render_time_ms,
                    },
                );

                // Don't clear accumulation on next frame unless something changes
                clear_accumulation = false;
            } else if let Some(Err(e)) = render_result {
                eprintln!("Render error: {}", e);
            }

            // Small yield to prevent busy-spinning when idle
            // The GPU work provides natural pacing, but this helps when scene is empty
            std::thread::sleep(std::time::Duration::from_micros(100));
        }
    });
}
