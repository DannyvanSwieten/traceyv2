# Tracey Scene Editor

A desktop scene editor for the Tracey ray tracer, built with Tauri, Rust, and SolidJS.

## Architecture

```
SolidJS Frontend (TypeScript/TSX)
    ↕ Tauri IPC (invoke/events)
Rust Backend (FFI bridge + state management)
    ↕ C FFI (extern "C" API)
C++ Rendering Engine (tracey library)
```

### Key Design Decisions

- **Scene state lives in Rust**: Easily serializable, manipulatable, safe
- **C++ used only for rendering**: Compiled scenes passed via FFI on render
- **Progressive rendering**: Low quality (1 sample) during interaction, high quality on demand
- **Single build command**: Rebuilds C++, Rust, and UI automatically

## Project Structure

```
editor/
├── src/                          # SolidJS frontend
│   ├── index.tsx                 # Entry point
│   ├── App.tsx                   # Main application
│   ├── api/                      # Tauri command wrappers
│   ├── stores/                   # SolidJS stores (scene, viewport)
│   └── components/               # UI components
│       ├── layout/
│       ├── viewport/
│       └── panels/
├── src-tauri/                    # Rust backend
│   ├── Cargo.toml                # Dependencies
│   ├── build.rs                  # Triggers CMake build
│   ├── src/
│   │   ├── main.rs               # Tauri entry point
│   │   ├── ffi/                  # FFI bindings
│   │   │   ├── raw.rs            # Raw C bindings
│   │   │   └── types.rs          # Safe Rust wrappers
│   │   ├── scene/                # Scene state (Rust-owned)
│   │   │   └── state.rs          # SceneState struct
│   │   ├── renderer/             # Rendering engine
│   │   │   ├── engine.rs         # RenderEngine
│   │   │   └── viewport.rs       # Viewport
│   │   └── commands/             # Tauri commands
│   │       ├── scene.rs          # Scene manipulation
│   │       ├── render.rs         # Rendering
│   │       └── io.rs             # Import/export
│   ├── libs/                     # C++ library (auto-installed)
│   └── include/                  # C API headers (auto-installed)
├── package.json                  # Frontend dependencies
├── vite.config.ts                # Vite configuration
└── tauri.conf.json               # Tauri configuration
```

## Development

### Prerequisites

- CMake 3.16+
- Vulkan SDK
- Rust 1.70+
- Node.js 18+ (npm or pnpm)
- macOS/Linux (Windows support coming)

### Quick Start

From the project root:

```bash
./scripts/dev.sh
```

This will:
1. Build the C++ library (Debug mode)
2. Install headers and libs to `editor/src-tauri/`
3. Build Rust backend
4. Start Vite dev server with hot reload
5. Launch the Tauri application

### Manual Development

If you want more control:

```bash
# 1. Build C++ library
cd build
cmake .. -DBUILD_C_API=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target tracey_c_api -j8
cmake --install .

# 2. Run Tauri dev mode
cd ../editor
pnpm install
pnpm tauri dev
```

### Hot Reload

- **Frontend changes**: Instant HMR via Vite
- **Rust changes**: Automatic rebuild on save
- **C++ changes**: Requires restart - run `./scripts/dev.sh` again

## Building for Production

```bash
./scripts/build-all.sh
```

Output will be in `editor/src-tauri/target/release/bundle/`

## Available Tauri Commands

### Scene Management
- `create_actor(name: string) -> u64`
- `delete_actor(actor_id: u64) -> bool`
- `get_all_actors() -> Actor[]`
- `set_actor_transform(actor_id: u64, transform: Transform)`
- `set_camera(camera: Camera)`
- `get_camera() -> Camera`

### Rendering
- `render_frame(camera: Camera, clear_accumulation: bool) -> RenderResult`
- `get_render_pixels() -> Vec<u8>`
- `compile_scene()`
- `set_viewport_resolution(width: u32, height: u32)`

### Import/Export
- `save_scene(path: string)`
- `load_scene(path: string)`
- `import_gltf(path: string)`
- `export_image(path: string, format: string)`

## Example Usage

```typescript
import { invoke } from '@tauri-apps/api/core';

// Create an actor
const actorId = await invoke('create_actor', { name: 'Cube' });

// Set its transform
await invoke('set_actor_transform', {
  actorId,
  transform: {
    position: { x: 0, y: 1, z: 0 },
    rotation: { w: 1, x: 0, y: 0, z: 0 },
    scale: { x: 1, y: 1, z: 1 },
  },
});

// Render frame
const result = await invoke('render_frame', {
  camera: /* camera data */,
  clearAccumulation: true,
});

// Get pixels
const pixels = await invoke('get_render_pixels');
```

## Future Extensions

1. **Material Editor**: Node graph UI for shader editing
2. **Geometry Graphs**: Procedural modeling with nodes
3. **Native Viewport**: Direct GPU context (no memory copies)
4. **Real-time Collaboration**: WebSocket sync between clients

## Troubleshooting

### "CMake build failed"
- Ensure Vulkan SDK is installed
- Check that shaderc is available: `pkg-config --libs shaderc`

### "Failed to link tracey_c_api"
- Run `cmake --install build` to copy library to editor directory
- Check that `editor/src-tauri/libs/libtracey_c_api.dylib` exists

### "Shader compilation failed"
- Verify shader paths in `main.rs` point to `examples/scene_renderer/shaders/`
- Check shader files exist and are valid ISF format

## License

Same as the main Tracey project.
