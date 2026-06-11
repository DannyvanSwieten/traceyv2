# Tracey Editor

Desktop editor for the Tracey ray tracer. **Build instructions live in the
[root README](../README.md)** — TL;DR: `cmake -S . -B build && cmake --build build -j8`
from the repo root produces `build/editor/native/tracey_editor.app`.

This README only covers editor-internal layout and conventions.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│ tracey_editor.app                                       │
│                                                         │
│   ┌─────────────────────────────────────────────────┐  │
│   │ Solid.js frontend (editor/src/)                 │  │
│   │   • SOP / VOP / Material graph canvases         │  │
│   │   • Scene hierarchy, inspector, dopesheet       │  │
│   │   • Playbar + keyframe timeline                 │  │
│   └─────────────────────────────────────────────────┘  │
│                       ▲ JSON IPC                         │
│                       ▼                                  │
│   ┌─────────────────────────────────────────────────┐  │
│   │ Native C++ host (editor/native/)                │  │
│   │   • Window + Metal/Vulkan viewport              │  │
│   │   • EditorServer — handle_command / broadcast   │  │
│   │   • Cook worker, video export worker            │  │
│   │   • Owns the canonical SopGraph + Scene         │  │
│   └─────────────────────────────────────────────────┘  │
│                       ▼ links                            │
│   ┌─────────────────────────────────────────────────┐  │
│   │ libtracey (../src/)                             │  │
│   │   path tracer, rasterizer, BLAS/TLAS, glTF, …   │  │
│   └─────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

The frontend never links the engine directly — every state change is a
JSON IPC call (`set_sop_graph`, `set_actor_transform`, `param_set_keyframe`,
…) and every push from the server is a `broadcast` event (`scene_changed`,
`sop_graph_changed`, `timeline_tick`, …). The seam is documented in
[editor/src/lib/api.ts](src/lib/api.ts).

## Layout

```
editor/
  native/                C++ host
    main.cpp             App entry, display-link loop
    editor_server.{hpp,cpp}
                         JSON IPC dispatch, cook worker, animation override,
                         video export, scene IO
    render_engine.{hpp,cpp}
                         Path tracer + rasterizer lifecycle, scene compile
    viewport_renderer.{hpp,cpp}
                         PiP composite + swapchain present
    platform/            macOS-specific glue (Metal, window, dialogs)
    video_exporter.mm    AVAssetWriter offline encode

  src/                   Frontend (Solid.js + TypeScript)
    App.tsx              Top-level layout, global shortcuts, listeners
    lib/                 Pure-TS helpers (api, sop/vop schemas, gltf import,
                         dopesheet channel walker, marquee math)
    stores/              Solid signal stores (sops, vops, materials, assets,
                         timeline)
    components/
      sop-graph/         SOP canvas, palette, inspector, breadcrumb
      vop-graph/         VOP canvas, inspector (mirrors SOP)
      material-graph/    Material node graph editor
      actor-properties/  Right-panel inspector with keyframe dots
      scene-hierarchy/   Tree view, visibility eye
      playbar/           Transport, scrub, fps, loop, autokey
      dopesheet/         Channel list, ruler, key diamonds, drag/drop
      keyframe-dot/      Houdini-style diamond next to inspector fields
      resources-browser/ Asset list (import → load workflow)
      viewport/          Native viewport overlay
      camera-controls/, render-settings/, export-video/, scene-menu/

  package.json, vite.config.ts, tsconfig.json
                         Vite + TS config — driven by editor/native/CMakeLists.txt
```

## IPC conventions

- Requests: `{"cmd": "<name>", ...args}` → responses `{"ok": true, "data": ...}`
  or `{"ok": false, "error": "..."}`.
- Broadcasts: `{"event": "<name>", ...payload}` pushed unsolicited.
- The frontend never assumes consistent state between calls — each
  mutation broadcasts and stores re-fetch when they care.

Adding a new command: add a handler arm in
[editor_server.cpp `handle_command`](native/editor_server.cpp), add a typed
wrapper in [editor/src/lib/api.ts](src/lib/api.ts), call from the relevant
component or store.

## Frontend dev tips

- The CMake editor target runs `npm install --silent && vite build`
  automatically — no separate frontend step. If you want HMR, run
  `npm run dev` from `editor/` against a running `tracey_editor` (the host
  loads from `dist/` by default; you'll need to point it at the dev server
  during iteration).
- Stores debounce graph pushes 300ms; if you need an immediate cook after a
  programmatic mutation, call `flushSopGraph()` from
  [stores/sops.ts](src/stores/sops.ts).
- Keyframe edits go through `paramSetKeyframe / paramMoveKeyframe /
  paramDeleteKeyframe`. The keyframe dot, dopesheet, and auto-key path all
  route through these — see [stores/timeline.ts](src/stores/timeline.ts).
