# Tracey

A path-tracing renderer with a Houdini-style scene editor. The C++ engine
([`src/`](src/)) handles geometry, materials, BLAS/TLAS, the path tracer, and
a Vulkan rasterizer for the live viewport. The desktop editor
([`editor/`](editor/)) is a native C++ window hosting a Solid.js frontend over
an IPC bridge; it ships a SOP graph (nested subnets, per-actor materials),
per-point VOP graphs, a keyframe timeline + dopesheet, glTF import, and
offline video export.

## Quick start (macOS)

```sh
git clone --recursive <repo-url> traceyv2
cd traceyv2
cmake -S . -B build
cmake --build build -j8
open build/editor/native/tracey_editor.app
```

If you forgot `--recursive` on clone:

```sh
git submodule update --init --recursive
```

The CMake build runs `npm install` + `vite build` for the frontend
automatically — no separate frontend step required.

## Prerequisites

- **macOS** with Xcode Command Line Tools (Apple Clang 14+, C++20). Linux /
  Windows aren't supported yet — the editor uses Metal + AVFoundation
  through an Objective-C++ TU.
- **CMake** 3.16+.
- **Vulkan SDK** with **MoltenVK**. Install from
  [vulkan.lunarg.com](https://vulkan.lunarg.com/) and make sure
  `VULKAN_SDK` is exported (the SDK installer sets this up; if not, source
  `setup-env.sh` from your install).
- **Node.js + npm** on `$PATH` (the CMake editor target shells out to them).
- **Git submodules**: `deps/glm`, `deps/volk`, `deps/tinygltf` — pulled by
  `--recursive` clone, or by `git submodule update --init --recursive`.

## What gets built

- `libtracey.a` — the renderer + scene library.
- `tracey_editor.app` — the desktop editor (under `build/editor/native/`).
- A pile of CLI smoke tests + examples under `build/examples/` —
  `path_tracer`, `scene_renderer`, `sop_eval_test`, etc.

## Repository layout

```
src/                   Engine library (renderer, scene, SOP/VOP framework)
  path_tracer/         Path tracer module (tracey_pathtracer library):
                       Metal RT backend (macOS, hardware RT on M3+),
                       native CPU fallback, Vulkan-RT slot for Windows
  rendering/           Vulkan rasterizer (live viewport), post-processing
  scene/               Scene graph, glTF loader, scene compiler (TLAS)
  sops/                Surface-operator nodes (geometry graph)
  vops/                Per-point attribute graph nodes
  device/              Vulkan + CPU compute back-ends

editor/
  native/              C++ desktop host (window, Metal/Vulkan viewport, IPC)
  src/                 Solid.js frontend (canvas editors, inspector, timeline)

examples/              CLI binaries that exercise the engine standalone
deps/                  Vendored / submodule third-party (glm, volk, tinygltf)
shaders/               GLSL sources compiled offline into the build
docs/                  Design notes
```

## Running the editor

After the build, the app launches from:

```sh
open build/editor/native/tracey_editor.app
```

First-launch flow: **File → Import glTF** to register a model in the asset
browser, then click **Load** on its row to drop a parented subnet tree into
the SOP graph. Animate by toggling **AK** in the playbar and dragging
parameter sliders, or use **K** to snapshot the selected actor's pose into
keyframes at the current playhead.

Notable keyboard shortcuts:

| Key              | Action                                         |
| ---------------- | ---------------------------------------------- |
| `Space`          | Play / pause                                   |
| `← / →`          | Step one frame                                 |
| `Home / End`     | Jump to range start / end                      |
| `K`              | Key selected actor's pose at current playhead  |
| `Cmd+Z / ⇧Cmd+Z` | Undo / redo SOP graph edits                    |
| `Alt-drag`       | Pan the SOP canvas (inside the graph editor)   |
| `Esc`            | Pop out of a subnet (inside the graph editor)  |

## Troubleshooting

- **`find_package(Vulkan)` fails** — install the Vulkan SDK and make sure
  `VULKAN_SDK` is set in your shell. On macOS, `source ~/VulkanSDK/<version>/setup-env.sh`.
- **`add_subdirectory(deps/glm)` says directory does not contain `CMakeLists.txt`** —
  you cloned without `--recursive`. Run `git submodule update --init --recursive`.
- **Frontend assets missing after the editor launches** — re-run
  `cmake --build build -j8`; the Vite build is a `CMakeLists.txt`
  post-build step on the editor target.

## Status

Active development. APIs and on-disk scene format (`.tracey`) shift
frequently; nothing is stable yet.
