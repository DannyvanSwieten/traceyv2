# Houdini-style SOP Backend — Status & Roadmap

This is the working notebook for the SOP (Surface Operator) graph backend.
Use it to context-switch onto another machine: it lists what's shipped, what
the architecture looks like, what's left, and how to verify any of it.

The original plan that produced this work was approved on 2026-05-09. v1
shipped the same day in five phases. This doc supersedes the planning file;
the plan file in `~/.claude/plans/` was throwaway scratch.

---

## TL;DR

A Houdini-style SOP graph now lives at the scene level. Cooking it produces
the list of actors the path tracer renders. The graph editor is reachable
from the toolbar's **SOP Graph** button.

- **Geometry container**: attribute tables (point / vertex / primitive /
  detail) — `tracey::Geometry` in [src/geometry/](../src/geometry/).
- **SOP framework**: `tracey::sops::SopGraph` (inherits `tracey::Graph`),
  `tracey::sops::SopNode` (inherits `tracey::Node`) — see
  [src/sops/](../src/sops/).
- **Built-in nodes**: 6 primitives (cube/sphere/plane/torus/cylinder/cone),
  `gltf_import`, `transform`, `merge`, `object_output`. Registered via
  [register_builtins.cpp](../src/sops/register_builtins.cpp) — explicit, not
  static-init magic.
- **Editor host**: `EditorServer` owns one `SopGraph`. JSON commands
  `list_sop_node_catalog` / `get_sop_graph` / `set_sop_graph`. Cook runs on
  every `set_sop_graph` and rebuilds the live `tracey::Scene` from emitted
  actors.
- **Frontend**: SOP graph editor as a modal (mirrors the existing material
  graph editor). Pan/zoom canvas, node palette, parameter inspector. Server-
  driven catalog so the TS palette stays in lockstep with the C++ side.
- **Persistence**: scene save format bumped to schema v2 with embedded
  `sop_graph` JSON. v1 scene files load best-effort (actors only).

---

## Architecture

```
                                  ┌──────────────────────┐
                                  │  Frontend (Solid.js) │
                                  │ ┌──────────────────┐ │
                                  │ │  SopGraphPanel   │ │
                                  │ │  ┌─────────────┐ │ │
                                  │ │  │SopGraphCanvas │ │  pan / zoom / wires
                                  │ │  │SopNodePalette │ │  catalog-driven
                                  │ │  │SopNodeInspector│ │ params by spec
                                  │ │  └─────────────┘ │ │
                                  │ │   sop_graph.ts   │ │  TS types + catalog
                                  │ │   sops.ts        │ │  Solid store
                                  │ └──────────────────┘ │
                                  └──────────┬───────────┘
                                             │ JSON over __traceyBridge
                                             │ list_sop_node_catalog
                                             │ get_sop_graph / set_sop_graph
                                             ▼
┌────────────────────────────────────────────────────────────┐
│  EditorServer (editor/native/editor_server.cpp)            │
│  • Owns one SopGraph                                       │
│  • Cooks on every set_sop_graph (currently main-thread)    │
│  • Broadcasts scene_changed after cook                     │
│  • Broadcasts sop_graph_changed after set_actor_transform  │
│    writes back into a source ObjectOutput node             │
└────────────────────────────┬───────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────────┐
│ tracey::sops  (src/sops/)                                           │
│                                                                     │
│  SopGraph : public tracey::Graph                                    │
│   ├─ topo_sort + per-node result cache                              │
│   └─ cook() → vector<EmittedActor>                                  │
│                                                                     │
│  SopNode : public tracey::Node                                      │
│   ├─ kind() string id (codegen-friendly)                            │
│   ├─ ports() InputsAndOutputs                                       │
│   ├─ Parameter table (typed: float/int/bool/vec3/string)            │
│   └─ cook(span<const Geometry*>) → Geometry  (PURE)                 │
│                                                                     │
│  EmittedActor (carries sourceNodeUid for stable mapping)            │
│  SopRegistry (string-kind keyed; explicit registerBuiltinSops())    │
│  serialization {graph_kind:"sop", version:1, ...}                   │
└─────────────────────────────────────────────────────────────────────┘
                             │  cook produces
                             ▼
┌─────────────────────────────────────────────────────────────────────┐
│ tracey::Geometry  (src/geometry/)                                   │
│  Attribute<T>, AttributeTable, four classes                         │
│  GeometryConverter to/from SceneObject                              │
└─────────────────────────────────────────────────────────────────────┘
                             │  GeometryConverter::toSceneObject
                             ▼
┌─────────────────────────────────────────────────────────────────────┐
│ tracey::SceneObject + Actor + SceneInstance  (existing, unchanged)  │
│  ← path tracer pipeline downstream is completely unaware of SOPs.   │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Phase status

| Phase | Status | Notes |
|-------|--------|-------|
| 1 — Geometry container | ✅ shipped | `Geometry`, `Attribute<T>`, `AttributeTable`, `GeometryConverter` |
| 2 — SOP framework | ✅ shipped | `SopGraph`, `SopNode`, registry, serialization, 11 built-in nodes |
| 3 — Editor host integration | ✅ shipped | `sop_*` commands, scene save/load v2, camera preserved across cooks, stable actor↔SOP mapping via threaded `sourceNodeUid` |
| 4 — Frontend graph editor UI | ✅ shipped | Canvas / palette / inspector / modal — all under [editor/src/components/sop-graph/](../editor/src/components/sop-graph/) |
| 5 — Cleanup (`add_primitive` removal) | ✅ shipped | Imperative add/import/parenting handlers removed from `editor_server.cpp`; `<AddObjectMenu>` UI deleted; `RenderEngine::load_gltf` removed (dead). Top-level Import menu now only registers the asset; geometry comes in via `gltf_import` SOP node. |
| 6 — Graph → C++/dylib codegen | ⏸ future | Architectural invariants in v1 keep this option open; not implemented |

---

## Files

### New (C++)

```
src/geometry/
  attribute_class.hpp       Point | Vertex | Primitive | Detail enum
  attribute.{hpp,cpp}       AttributeBase + Attribute<T>
  attribute_table.{hpp,cpp} name → unique_ptr<AttributeBase>
  geometry.{hpp,cpp}        Houdini-style 4-class geometry container
  geometry_converter.{hpp,cpp}  to/from tracey::SceneObject

src/sops/
  parameter.hpp             ParamType + Parameter (typed variant)
  sop_node.{hpp,cpp}        Abstract base + parameter table + EmittedActor
  sop_graph.{hpp,cpp}       DAG, topo sort, cook → EmittedActor[]
  sop_registry.{hpp,cpp}    string-kind keyed type registry
  serialization.{hpp,cpp}   JSON ↔ SopGraph (schema v1 with graph_kind:"sop")
  register_builtins.cpp     Aggregator that pulls every node TU into the link
  nodes/
    primitive_sops.cpp      Cube/Sphere/Plane/Torus/Cylinder/Cone
    transform_sop.cpp       SRT, applies to P + N
    merge_sop.cpp           2-input geometry concatenation
    object_output_sop.cpp   Terminal — emits an EmittedActor
    gltf_import_sop.cpp     Wraps GltfLoader, merges all loaded objects

examples/sop_eval_test/main.cpp   Standalone smoke test (22 checks)
```

### New (Frontend)

```
editor/src/lib/sop_graph.ts          TS types + server-driven catalog
editor/src/stores/sops.ts            Solid store + 50ms-debounced push
editor/src/components/sop-graph/
  SopGraphPanel.{tsx,css}            Three-column layout (palette | canvas | inspector)
  SopGraphEditor.tsx                 Modal wrapper (mirrors MaterialGraphEditor)
  SopGraphCanvas.tsx                 SVG pan/zoom/wires (adapted from material canvas)
  SopNodePalette.tsx                 Catalog-grouped buttons
  SopNodeInspector.tsx               Param editor typed by ParamSpec
```

### Modified

- `CMakeLists.txt` — added `src/geometry/` and `src/sops/` source lists.
- `editor/native/editor_server.{hpp,cpp}` — owns `SopGraph`, new `sop_*`
  command handlers, repurposed `set_actor_transform` to write back into
  source SOP, save/load v2 with embedded SOP graph.
- `editor/src/lib/api.ts` — exported the internal `send<T>()` so feature
  modules (`sop_graph.ts`) can reuse the envelope-unwrapping behaviour.
- `editor/src/App.tsx` — added "SOP Graph" toolbar button + `<SopGraphEditor>`.
- `examples/CMakeLists.txt` — added the `sop_eval_test` target.

### Untouched (load-bearing)

- The path tracer pipeline ([src/rendering/](../src/rendering/),
  [src/ray_tracing/](../src/ray_tracing/), [src/device/](../src/device/),
  [src/gpu/](../src/gpu/)) — graph layer is purely upstream of `SceneCompiler`.
- Existing material graph C++ code under [src/graph/](../src/graph/) — only
  consumes the same `tracey::Graph` / `tracey::Node` base classes; no
  behavioural change.
- [src/scene/scene_object.{hpp,cpp}](../src/scene/scene_object.cpp),
  [actor.{hpp,cpp}](../src/scene/actor.cpp),
  [scene_instance.{hpp,cpp}](../src/scene/scene_instance.cpp) — geometry the
  path tracer sees stays this shape.

---

## Build & verify

### C++ smoke test (no UI, no Vulkan)

```bash
cmake --build build --target sop_eval_test
./build/examples/sop_eval_test
```

Should produce 22 lines starting with `ok   ` and end with
`[sop_eval_test] all checks passed`. Exits non-zero if any check fails.

What it covers:

- `SopRegistry::registerBuiltinSops()` populates the catalog with all 10
  v1 node kinds.
- A `Cube → Transform → ObjectOutput` graph cooks correctly.
- The `Transform` SOP shifts positions by its `translate` parameter.
- `GeometryConverter::toSceneObject` preserves vertex / triangle counts.
- `serialize → deserialize → cook` round-trip is byte-stable.

### Editor end-to-end

```bash
cmake --build build --target tracey_editor
open build/editor/native/tracey_editor.app   # macOS
```

Steps to validate the SOP loop interactively:

1. Click **SOP Graph** in the toolbar — modal opens with a populated palette.
2. Click *Cube* in the palette → cube node appears at the canvas origin.
3. Click *Object Output* in the palette → another node spawns.
4. Click the cube's right-side output port (right edge), then click the
   Object Output's left-side input port → bezier wire connects them.
5. Close the modal — viewport should show a default-grey cube.
6. Reopen the modal, select the Object Output, edit `translate.x` in the
   inspector → the cube slides; path tracer accumulator resets every change.
7. Click *glTF Import* in the palette, type a `.gltf` / `.glb` path in its
   inspector, wire it to a new Object Output → imported geometry appears.
8. **File → Save Scene** → reopen the editor → **File → Open Scene** → graph
   restored, viewport re-cooks identically.

### Live regression check (material editor)

The Phase 4 SOP UI was deliberately implemented as a parallel set of
components rather than refactoring the material editor. Validate the
material editor still works:

1. Toolbar → **Material Graph** — modal opens.
2. Add a `Constant` node + an `Output WriteAlbedo` → wire → save to library.
3. Select an actor in the scene hierarchy → the material assignment
   dropdown still applies the saved graph.

---

## Recommended follow-ups (priority order)

### 1. End-to-end interactive validation

The unit test covers C++ correctness; only a live run shakes out UI / bridge
bugs. Specific things to look for:

- Catalog default-string parsing in
  [editor/src/lib/sop_graph.ts](../editor/src/lib/sop_graph.ts) —
  `parseDefault()` expects `"\"actor\""` for string defaults; verify the
  C++ catalog ships them with embedded quotes.
- After moving an actor via the Actor Properties panel and reopening the
  SOP editor, verify the source ObjectOutput's `translate` matches the new
  position. (`set_actor_transform` writes back; `sop_graph_changed`
  broadcast forces frontend reload.)
- Save / reload round-trip after editing material library assignments via
  ObjectOutput's `material_library_name` parameter.

### 2. Worker-thread cook ✅

`set_sop_graph` no longer cooks synchronously on the message thread.
[editor_server.cpp](../editor/native/editor_server.cpp) now runs a single
worker thread (`cook_worker_loop`) that owns its own deserialized SopGraph
copy for the duration of each cook; `m_sop_graph` stays canonical for the
message thread (read by `get_sop_graph`, written by `set_actor_transform`).

- `set_sop_graph` parses the JSON synchronously (so parse errors surface
  in the response), replaces `m_sop_graph`, calls `post_cook_request(json)`
  which hands the JSON to the worker, and returns ok.
- `cook_worker_loop` waits on a CV, deserializes a private SopGraph from
  the JSON, cooks it, and pushes the resulting `vector<EmittedActor>` into
  `m_pending_cook_result` (latest-wins).
- `render_tick` calls `drain_cook_result` once per frame on the main
  thread; if a result is waiting it runs `apply_emitted` (scene rebuild +
  path tracer recompile + `scene_changed` broadcast). One frame of
  latency between cook completion and visibility, capped by the display
  link tick.
- `cook_and_apply` is kept as the synchronous path used by `load_scene`,
  where blocking on a one-shot file load is fine.

Mid-cook race notes (acceptable for v1):

- Rapid edits during a long cook just overwrite `m_pending_cook_request`;
  the worker only ever cooks the most recently posted JSON. Intermediate
  edits never produce visible frames.
- `set_actor_transform` writeback to ObjectOutput's translate/scale
  parameters happens on `m_sop_graph` while a worker cook is in flight —
  the writeback persists for the *next* cook, not the in-flight one.
  Visible delta is one cook cycle, same as today's debounced model.

### 3. Additional SOPs

The framework is ready; each new node is a single `.cpp` under
[src/sops/nodes/](../src/sops/nodes/) plus one line in
[register_builtins.cpp](../src/sops/register_builtins.cpp). Worth adding
soon:

- **`subdivide`** (Loop or Catmull-Clark) — first interesting topology
  modifier.
- **`triangulate`** — when polygon SOPs land later.
- **`color`** — sets a `Cd` (Vec3) point or vertex attribute. Pure math,
  perfect first GPU-codegen candidate.
- **`attribute_create`** — declares a new attribute on a chosen class with
  a constant default.
- **`normalize_normals`** — re-normalises `N` (Houdini's NormalAdjust).

### 4. UI polish (low priority)

- Empty-state message in `SopGraphPanel` for new users ("Drag a node from
  the palette →").
- "New" button on the modal toolbar that wipes the graph and inserts a
  starter `Cube → ObjectOutput` template.
- Right-click on a node for a context menu (Delete, Disconnect inputs).

### 5. Phase 4a refactor (was deferred)

The plan called for promoting `GraphCanvas` / `NodePalette` /
`NodeInspector` into a shared `editor/src/components/graph/` directory so
both the material and SOP editors use the same components. We instead
copied + adapted to keep the material editor untouched. Worth doing when
the duplication starts costing more than the refactor risk.

### 6. Future: graph → C++/dylib codegen (Phase 6)

v1 invariants are already in place (pure cook, string `kind`, typed param
table, no `std::function` in nodes, one TU per node). To add codegen later:

- Per-node `emit_cpp(SourceWriter&, params)` method.
- `SopGraph::compile_to_cpp(out_path)` walks topologically, glues per-node
  snippets into `cook_graph_<hash>(params, inputs) → Geometry`.
- Build step shells out to clang for `.dylib`/`.so`/`.dll`; cache keyed on
  graph content hash.
- Editor toggle: "Optimize graph" — use compiled dylib if available, else
  fall back to interpreter.

`gltf_import` is the one node that stays interpreted (file IO is opaque).

### 7. Future: DOPs (particle / physics simulation)

Out of scope for this work. Architecturally compatible:

- Point attributes (`P`, `v`, `age`, etc.) are already first-class.
- A new `SimGraph` graph type would be a sibling of `SopGraph`, also
  inheriting `tracey::Graph`. Per-frame cook with time delta, persistent
  state attribute table.
- Same UI scaffolding (canvas / palette / inspector) once the components
  are properly extracted (see follow-up #5).

---

## Known issues / non-issues

- **Mid-edit race after `set_actor_transform`** — frontend SOP store
  refresh is best-effort. If you're typing in the SOP inspector at the
  exact moment `set_actor_transform` fires from the actor panel, the
  pending push may not include the latest. Skipping the reload while a
  push is pending mitigates the common case; full lockstep would need
  per-node version stamps.
- **Quaternion → euler round-trip** — `set_actor_transform` doesn't
  currently write back to ObjectOutput's `rotate_euler_deg` param (the
  wire is a quaternion; we'd need a stable quat→euler conversion that
  handles gimbal correctly). Translate and scale are persisted.
- **Schema v1 scene files** — load best-effort: actors and camera are
  read, but no SOP graph is recovered. Document; don't migrate.
- **Material editor's existing TS warnings** — `Setter` unused import,
  `Show` typing issue. Pre-existing, vite still builds. Unrelated to SOP
  work.
