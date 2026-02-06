//! Native Viewport
//!
//! Coordinates Vulkan presentation using Tauri's native window.
//! The Vulkan swapchain is created on Tauri's window with region-based blitting.

use crate::ffi::{Device, Presenter, PresenterConfig};
use std::sync::Arc;
use tauri::{AppHandle, Manager};

#[cfg(target_os = "macos")]
use metal::foreign_types::ForeignType;

#[cfg(target_os = "macos")]
use block;

/// Viewport layout information
#[derive(Debug, Clone, Copy)]
pub struct ViewportBounds {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

/// Safe wrapper for NSView pointer that can be sent between threads
#[cfg(target_os = "macos")]
struct MetalViewHandle(*mut std::ffi::c_void);

#[cfg(target_os = "macos")]
unsafe impl Send for MetalViewHandle {}
#[cfg(target_os = "macos")]
unsafe impl Sync for MetalViewHandle {}

/// Native viewport state
pub enum ViewportState {
    /// Not yet initialized
    Uninitialized,
    /// Presenter ready (using Tauri's window)
    Ready {
        presenter: Arc<Presenter>,
        #[cfg(target_os = "macos")]
        metal_view: MetalViewHandle,
    },
    /// Error state
    Error(String),
}

/// Native viewport coordinator
/// Manages the lifecycle of native window + presenter
pub struct NativeViewport {
    state: ViewportState,
    bounds: ViewportBounds,
    /// Swapchain/window size (physical pixels)
    window_size: (u32, u32),
}

impl NativeViewport {
    pub fn new() -> Self {
        Self {
            state: ViewportState::Uninitialized,
            bounds: ViewportBounds {
                x: 0,
                y: 0,
                width: 1280,
                height: 720,
            },
            window_size: (0, 0),
        }
    }

    /// Initialize the native viewport using Tauri's window
    ///
    /// Gets Tauri's main window handle and creates Vulkan presenter on it
    pub fn initialize_with_tauri_window(
        &mut self,
        app: &AppHandle,
        device: &Device,
        bounds: ViewportBounds,
    ) -> Result<(), String> {
        // Enable MoltenVK debug logging for detailed error information
        #[cfg(target_os = "macos")]
        {
            std::env::set_var("MVK_CONFIG_DEBUG", "1");
            std::env::set_var("MVK_CONFIG_LOG_LEVEL", "3");
            std::env::set_var("MVK_CONFIG_TRACE_VULKAN_CALLS", "1");
            println!("MoltenVK debug logging enabled");
        }

        // Get Tauri's main window
        let tauri_window = app.get_webview_window("main")
            .ok_or("Main window not found")?;

        // Get window size (full window, not viewport)
        let window_size = tauri_window.inner_size()
            .map_err(|e| format!("Failed to get window size: {}", e))?;

        // Get raw window handle (cross-platform!)
        #[cfg(target_os = "macos")]
        let (native_window_ptr, native_display_ptr) = {
            use cocoa::appkit::{NSView, NSWindow};
            use cocoa::base::{id, nil, YES, NO};
            use cocoa::foundation::{NSRect, NSPoint, NSSize};
            use objc::runtime::Class;
            use std::sync::{Arc, Mutex, Condvar};

            let ns_window = tauri_window.ns_window()
                .map_err(|e| format!("Failed to get NSWindow: {}", e))?;

            // CRITICAL: CAMetalLayer must be created on the main thread!
            unsafe {
                let window = ns_window as id;

                // Shared state for communication between threads
                let result: Arc<Mutex<Option<(id, id, Result<(), String>)>>> = Arc::new(Mutex::new(None));
                let result_clone = result.clone();
                let condvar = Arc::new(Condvar::new());
                let condvar_clone = condvar.clone();

                // Get the main operation queue (NSOperationQueue is the Objective-C wrapper for dispatch queues)
                let main_queue: id = msg_send![class!(NSOperationQueue), mainQueue];

                // Create a block that will execute on the main thread
                let block = block::ConcreteBlock::new(move || {
                    let content_view: id = msg_send![window, contentView];

                    println!("NSWindow: {:?}", window);
                    println!("ContentView: {:?}", content_view);

                    let layer_result: Result<(id, id), String> = (|| {
                        // Always create a fresh CAMetalLayer
                        println!("Creating new CAMetalLayer on main thread");
                        let metal_layer_class = Class::get("CAMetalLayer")
                            .ok_or("Failed to get CAMetalLayer class")?;
                        let metal_layer: id = msg_send![metal_layer_class, layer];

                        let backing_scale: f64 = msg_send![window, backingScaleFactor];
                        let content_bounds: NSRect = msg_send![content_view, bounds];

                        // Metal layer should be FULL WINDOW SIZE (matches swapchain)
                        // The Vulkan blit will only render to the viewport region
                        println!("CAMetalLayer size: {}x{}, scale: {}",
                                content_bounds.size.width, content_bounds.size.height, backing_scale);

                        let _: () = msg_send![metal_layer, setContentsScale: backing_scale];
                        let _: () = msg_send![metal_layer, setFrame: content_bounds];
                        let _: () = msg_send![metal_layer, setOpaque: NO];

                        // CRITICAL: Add as sublayer instead of replacing the contentView's layer
                        // This preserves the WebView's rendering ability
                        let _: () = msg_send![content_view, setWantsLayer: YES];
                        let existing_layer: id = msg_send![content_view, layer];

                        if existing_layer.is_null() {
                            return Err("ContentView layer is null after setWantsLayer".to_string());
                        }

                        // Make the WebView layer transparent so Metal layer shows through
                        let _: () = msg_send![existing_layer, setOpaque: NO];
                        // Set transparent background color
                        let clear_color: id = msg_send![class!(NSColor), clearColor];
                        let cg_color: id = msg_send![clear_color, CGColor];
                        let _: () = msg_send![existing_layer, setBackgroundColor: cg_color];

                        // Add our metal layer as a sublayer BENEATH WebView (at bottom)
                        let _: () = msg_send![existing_layer, insertSublayer:metal_layer atIndex:0u32];

                        println!("Added CAMetalLayer as sublayer (beneath WebView, WebView layer transparent)");

                        // Retain the layer to keep it alive
                        let _: id = msg_send![metal_layer, retain];

                        Ok((metal_layer, content_view))
                    })();

                    // Store result and notify waiting thread
                    let mut result_guard = result_clone.lock().unwrap();
                    *result_guard = Some(match layer_result {
                        Ok((layer, view)) => (layer, view, Ok(())),
                        Err(e) => (nil, nil, Err(e)),
                    });
                    condvar_clone.notify_one();
                });

                let block = block.copy();

                // dispatch_async to main queue
                let _: () = msg_send![main_queue, addOperationWithBlock: block];

                // Wait for the block to complete
                let mut result_guard = result.lock().unwrap();
                while result_guard.is_none() {
                    result_guard = condvar.wait(result_guard).unwrap();
                }

                let (metal_layer, content_view, res) = result_guard.take().unwrap();
                res?;

                (metal_layer as *mut std::ffi::c_void, content_view as *mut std::ffi::c_void)
            }
        };

        #[cfg(target_os = "windows")]
        let (native_window_ptr, native_display_ptr) = {
            let hwnd = tauri_window.hwnd()
                .map_err(|e| format!("Failed to get HWND: {}", e))?;
            (hwnd.0 as *mut std::ffi::c_void, std::ptr::null_mut())
        };

        #[cfg(not(any(target_os = "macos", target_os = "windows")))]
        return Err("Platform not yet supported for native rendering".to_string());

        // Create presenter config (full window size)
        let config = PresenterConfig {
            width: window_size.width,
            height: window_size.height,
            enable_hdr: false,
            desired_image_count: 3, // Triple buffering
        };

        // Create presenter with Tauri's window (pass layer pointer, not view)
        let presenter = Presenter::new(device, native_window_ptr, std::ptr::null_mut(), &config)
            .map_err(|e| format!("Failed to create presenter: {}", e))?;

        // Update state
        #[cfg(target_os = "macos")]
        {
            self.state = ViewportState::Ready {
                presenter: Arc::new(presenter),
                // Store the metal_layer pointer for cleanup (not contentView)
                metal_view: MetalViewHandle(native_window_ptr),
            };
        }

        #[cfg(not(target_os = "macos"))]
        {
            self.state = ViewportState::Ready {
                presenter: Arc::new(presenter),
            };
        }

        self.bounds = bounds;
        self.window_size = (window_size.width, window_size.height);

        Ok(())
    }

    /// Resize the presenter/swapchain when window size changes
    ///
    /// Returns true if resize was performed
    pub fn resize_if_needed(&mut self, new_width: u32, new_height: u32) -> Result<bool, String> {
        if new_width == 0 || new_height == 0 {
            return Ok(false);
        }

        // Check if size actually changed
        let (current_w, current_h) = self.window_size;
        if current_w == new_width && current_h == new_height {
            return Ok(false);
        }

        match &self.state {
            #[cfg(target_os = "macos")]
            ViewportState::Ready { presenter, metal_view } => {
                println!(
                    "Resizing presenter from {}x{} to {}x{}",
                    current_w, current_h, new_width, new_height
                );

                // Wait for any pending work
                presenter.wait_idle();

                // CRITICAL: Also resize the CAMetalLayer on macOS
                // The surface capabilities are derived from the layer size
                unsafe {
                    use cocoa::base::id;
                    use cocoa::foundation::{NSRect, NSPoint, NSSize};
                    use std::sync::{Arc, Mutex, Condvar};

                    let metal_layer = metal_view.0 as id;
                    if !metal_layer.is_null() {
                        let done = Arc::new((Mutex::new(false), Condvar::new()));
                        let done_clone = done.clone();
                        let w = new_width as f64;
                        let h = new_height as f64;

                        let main_queue: id = msg_send![class!(NSOperationQueue), mainQueue];
                        let block = block::ConcreteBlock::new(move || {
                            // Get current frame to preserve position
                            let current_frame: NSRect = msg_send![metal_layer, frame];
                            let new_frame = NSRect::new(
                                current_frame.origin,
                                NSSize::new(w, h),
                            );
                            let _: () = msg_send![metal_layer, setFrame: new_frame];
                            println!("CAMetalLayer resized to {}x{}", w, h);

                            let (lock, cvar) = &*done_clone;
                            let mut finished = lock.lock().unwrap();
                            *finished = true;
                            cvar.notify_one();
                        });
                        let block = block.copy();
                        let _: () = msg_send![main_queue, addOperationWithBlock: block];

                        // Wait for resize to complete
                        let (lock, cvar) = &*done;
                        let mut finished = lock.lock().unwrap();
                        while !*finished {
                            finished = cvar.wait(finished).unwrap();
                        }
                    }
                }

                // Resize the swapchain
                presenter
                    .resize(new_width, new_height)
                    .map_err(|e| format!("Failed to resize presenter: {}", e))?;

                self.window_size = (new_width, new_height);
                Ok(true)
            }
            #[cfg(not(target_os = "macos"))]
            ViewportState::Ready { presenter } => {
                println!(
                    "Resizing presenter from {}x{} to {}x{}",
                    current_w, current_h, new_width, new_height
                );

                // Wait for any pending work
                presenter.wait_idle();

                // Resize the swapchain
                presenter
                    .resize(new_width, new_height)
                    .map_err(|e| format!("Failed to resize presenter: {}", e))?;

                self.window_size = (new_width, new_height);
                Ok(true)
            }
            ViewportState::Uninitialized => {
                // Not yet initialized, just store the size
                self.window_size = (new_width, new_height);
                Ok(false)
            }
            ViewportState::Error(e) => Err(format!("Viewport in error state: {}", e)),
        }
    }

    /// Get current window size
    pub fn window_size(&self) -> (u32, u32) {
        self.window_size
    }

    /// Update viewport bounds
    ///
    /// Note: This updates the stored bounds for region-based blitting.
    /// The Metal layer is always full-window size, only the blit region changes.
    pub fn update_bounds(&mut self, bounds: ViewportBounds) -> Result<(), String> {
        match &self.state {
            ViewportState::Ready { .. } => {
                // Update stored bounds for region-based blitting
                // Metal layer is always full-window size, so no frame update needed
                self.bounds = bounds;
                Ok(())
            }
            ViewportState::Uninitialized => Err("Viewport not initialized".to_string()),
            ViewportState::Error(e) => Err(format!("Viewport in error state: {}", e)),
        }
    }

    /// Show the viewport (no-op since Tauri manages window visibility)
    pub fn show(&mut self) -> Result<(), String> {
        match &self.state {
            ViewportState::Ready { .. } => Ok(()),
            ViewportState::Uninitialized => Err("Viewport not initialized".to_string()),
            ViewportState::Error(e) => Err(format!("Viewport in error state: {}", e)),
        }
    }

    /// Hide the viewport (no-op since Tauri manages window visibility)
    pub fn hide(&mut self) -> Result<(), String> {
        match &self.state {
            ViewportState::Ready { .. } => Ok(()),
            ViewportState::Uninitialized => Err("Viewport not initialized".to_string()),
            ViewportState::Error(e) => Err(format!("Viewport in error state: {}", e)),
        }
    }

    /// Get the presenter (if ready)
    pub fn presenter(&self) -> Option<Arc<Presenter>> {
        match &self.state {
            ViewportState::Ready { presenter, .. } => Some(presenter.clone()),
            _ => None,
        }
    }

    /// Get current bounds
    pub fn bounds(&self) -> ViewportBounds {
        self.bounds
    }

    /// Check if viewport is ready
    pub fn is_ready(&self) -> bool {
        matches!(self.state, ViewportState::Ready { .. })
    }

    /// Check if viewport is in error state
    pub fn is_error(&self) -> bool {
        matches!(self.state, ViewportState::Error(_))
    }

    /// Get error message if in error state
    pub fn error(&self) -> Option<&str> {
        match &self.state {
            ViewportState::Error(e) => Some(e),
            _ => None,
        }
    }
}

impl Default for NativeViewport {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for NativeViewport {
    fn drop(&mut self) {
        #[cfg(target_os = "macos")]
        {
            use cocoa::base::id;
            use std::sync::{Arc, Mutex, Condvar};

            // Clean up CAMetalLayer if it exists
            if let ViewportState::Ready { metal_view, .. } = &self.state {
                unsafe {
                    // Get the metal_layer we stored
                    let metal_layer = metal_view.0 as id;
                    if !metal_layer.is_null() {
                        println!("Removing CAMetalLayer sublayer");

                        // CRITICAL: Layer removal must happen on main thread
                        let done = Arc::new((Mutex::new(false), Condvar::new()));
                        let done_clone = done.clone();

                        let main_queue: id = msg_send![class!(NSOperationQueue), mainQueue];
                        let block = block::ConcreteBlock::new(move || {
                            let _: () = msg_send![metal_layer, removeFromSuperlayer];
                            let _: () = msg_send![metal_layer, release];
                            println!("CAMetalLayer removed and released on main thread");

                            let (lock, cvar) = &*done_clone;
                            let mut finished = lock.lock().unwrap();
                            *finished = true;
                            cvar.notify_one();
                        });
                        let block = block.copy();
                        let _: () = msg_send![main_queue, addOperationWithBlock: block];

                        // Wait for cleanup to complete
                        let (lock, cvar) = &*done;
                        let mut finished = lock.lock().unwrap();
                        while !*finished {
                            finished = cvar.wait(finished).unwrap();
                        }
                    }
                }
            }
        }
    }
}
