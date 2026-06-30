# USD Pipeline Layout & Contracts

*Status: Phase 0 (foundations). This is the contract the USD-native, multi-user,
department-layered editor is built on — see the roadmap in
`.claude/plans/sleepy-roaming-sloth.md`.*

A small team produces a short film by working in **department workspaces**, each
authoring its own **USD layer file per shot**. One **composed stage** is the single
source of truth; the viewport always shows the composed result; edits are
**non-destructive** (`over` opinions in the active layer only). Because the unit of
concurrent work is **`shot × department`**, two artists edit different *files* of the
same shot and never collide — collaboration falls out of USD's file-granular layering
(git-LFS + publish + reload), not live co-editing.

This document fixes three things everything else composes against:
1. the **project / file layout**,
2. the **sublayer strength order**,
3. the **stable-identity contract**.

---

## 1. Project / file layout

A project is a **git repository** (git-LFS for `*.usdc` + textures; department layers
stay mergeable `*.usda`).

```
project/                       # git repo
  assets/<asset>/
    <asset>.usd                # asset "interface": references model + look; stable namespace
    model.usd                  # geometry (SOP-baked or imported); procedural recipe in sidecar
    look.usd                   # materials + textures
  shots/<seq>/<shot>/
    shot.usd                   # ASSEMBLY: sublayers the dept layers (strength order) + refs assets
    layout.usd                 # Assets / Layout dept   (set dressing: asset refs + placement)
    anim.usd                   # Animation dept         (time samples on xforms / skel)
    fx.usd                     # FX dept                (DOP / sim bakes)
    lighting.usd               # Lighting dept          (UsdLux + light linking)
    render.usd                 # Rendering dept         (UsdRender settings/products/vars)
  edit/film.usd                # Directing / editorial: sequences the shots (value clips)
```

- An **asset** publishes a stable namespace (`<asset>.usd`) that downstream departments
  reference and override. Whether it was built procedurally (SOP/DOP recipe, baked to
  `model.usd`) or imported (glTF/USD) is invisible to consumers.
- A **shot** (`shot.usd`) is a thin **assembly**: it sublayers the per-department files
  and references the assets it uses. Each department edits *only* its own file.
- `edit/film.usd` assembles shots into the cut (editorial).

Procedural recipes (SOP/DOP graphs) are **not** USD scene description. They ride as
layer custom-data / a sidecar next to the layer they author, and are re-cooked or
loaded from the baked geometry on open. (Existing `serializeSopGraph`/`serializeDopGraph`
JSON is the recipe payload.)

**Scaffolding.** Saving a project (`save_scene`, `editor_server_cmds_io.cpp`) creates
this skeleton at the project root — `assets/ shots/ edit/ materials/` (with `.gitkeep`
so the empty structure persists in git) plus a `.gitattributes` routing the heavy
binaries (`*.usdc/usdz/exr/png/jpg/dds/abc/vdb`) through git-LFS while leaving `*.usda`
department layers text-mergeable. It is idempotent and non-destructive: re-saving an
existing project is a no-op and never clobbers an existing `.gitattributes`. **New Shot**
then lands a shot under `shots/seq01/sh01/` to match.

---

## 2. Sublayer strength order (LIVRPS)

In `shot.usd`, `subLayerPaths` lists the department layers **strongest opinion first**:

```
render  →  lighting  →  fx  →  anim  →  layout  →  (asset references, weakest)
```

So a render override beats a lighting opinion, which beats an animated transform, which
beats the layout placement, which beats whatever the referenced asset authored. This is
the one ordering everything composes against — changing it changes who wins.

Authoring routes to the **active department's layer via a `UsdEditTarget`**; the
composed stage resolves opinions by this order. This is proven end-to-end by
`examples/usd_write_smoke` (author a `def` into a weak layer + an `over` into a strong
layer through edit targets → the strong layer wins on reload, and neither file mutates
the other).

---

## 3. Stable-identity contract

Cross-department `over`s are keyed by **prim path**. For a lighting `over` to attach to
the right geometry — and for a viewport pick to resolve to the right source — the chain
**prim Sdf path ⇄ engine object ⇄ render instance ⇄ actor** must be stable across
re-imports and cooks.

```
USD prim Sdf path                     e.g. "/stage/layout/Marbles/A_marble_08/SM_sphere"
  ⇅  (import:   usd_loader.cpp:814  obj->setName(prim.GetPath().GetString())
      peek:     usd_loader.cpp:928  meshObjectNames = prim path
      export:   usd_exporter.cpp    mesh authored AT the object's path when it is one)
SceneObject name  (Scene::getObject key, SceneInstance::objectRef)
  ⇊  (SceneCompiler::compile flattens)
Tlas::Instance    + instanceToActorUid[id] = actor->getUid()   (scene_compiler.cpp:1139)
  ⇊
GPU TLAS hit → instanceToActorUid → actor   (viewport pick / selection)
```

**Rules:**

- The **object key is the full Sdf prim path** on import (already true). The exporter
  **preserves** that path when the name is a valid absolute Sdf path (verified: the
  marble round-trips as `/A_marble/SM_sphere`); procedurally-named objects sanitise
  under `/World/<ident>` and an object used N times yields N distinct prims
  (`…_inst{n}`).
- A **published asset's prim namespace is an API**: republishing must not churn prim
  paths, or downstream `over`s dangle. Modeling owns the namespace; lighting/anim/layout
  depend on it.
- **Actor-uid churn is the hazard.** The cook *slow path* recreates actors (new uids);
  the `sourceNodeUid → actor-uid` map (`m_emitted_actor_to_actor`, `editor_server.cpp`)
  re-points identity, and the *fast path* mutates in place (uid stable). As the
  StageDocument lands (Phase 1), prim path — not actor uid — is the durable key;
  actor uid is a per-session render-side handle.

---

## Phase-0 tooling that establishes these contracts

| Tool | Proves |
|---|---|
| `examples/usd_write_smoke` | edit targets → separate sublayer files → strength-ordered composition, non-destructively |
| `src/scene/usd_exporter.{hpp,cpp}` (`tracey_usd`) | `Scene → USD layer` (UsdGeomMesh + UsdPreviewSurface), Y-up + metersPerUnit, prim-path-preserving |
| `examples/usd_roundtrip_smoke` | import → export → re-import preserves draws / verts / tris / world bbox / resolved materials |
| `usdchecker` on exported layers | authored USD is valid & self-describing (Success!) |

Build: `cmake --build build -j8`; run with `DYLD_LIBRARY_PATH=/usr/local/lib`.
USD is gated behind `TRACEY_WITH_USD`; the write APIs (`sdf`/`tf` + `usd*`) link only
into `tracey_usd`, never the core `tracey` lib.
