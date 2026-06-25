# Character / performance animation — design + phased plan

## Goal

Make traceyv2 a *character* animation tool, not just a procedural / MoGraph /
sim tool. Today you can keyframe transforms, cook SOPs, sim DOPs, and drive
MoGraph effectors. You cannot animate a *character*: no skeleton, skinning,
IK, blendshapes, or mocap. This track closes that gap.

The **first slice** ("a character moves") is deliberately import-driven: load a
rigged + animated glTF, deform it by its baked clip, and play it through the
existing timeline. That proves the entire skeleton + skinning data model — which
IK, mocap, and blendshapes all build on — while leaning on glTF you already
import. No rig-authoring UI required to get the first win on screen.

## Landscape (what already exists — verified)

Much more is in place than a fresh "animation system" would assume:

- **Full keyframe / curve system.** `src/sops/parameter.hpp`: `ScalarChannel`
  with `Key{time, value, inTangent, outTangent, interp(Step|Linear|Bezier)}`,
  per-channel `Extrap{Hold|Cycle|Linear}`, `evaluate(time)`. `Parameter` holds
  up to 3 channels (vec3) + `evaluateAt(time)`. JS mirror in
  `editor/src/lib/curve_eval.ts`.
- **Dopesheet + Curve Editor already shipped.**
  `editor/src/components/dopesheet/Dopesheet.tsx`,
  `editor/src/components/curve-editor/CurveEditor.tsx` (bezier tangent handles,
  extrap menus, pan/zoom). Channel auto-discovery walks the SOP graph
  (`editor/src/lib/animated_channels.ts`).
- **Keyframe IPC complete.** `editor_server_cmds_timeline.cpp`:
  `param_set/move/delete_keyframe`, `param_set_channel_extrap`,
  `param_clear_channel`; transport `timeline_*`.
- **Timeline + sub-frame eval.** `TimelineState` (`editor_server.hpp` ~369),
  `apply_animation_at(time)` (`editor_server.cpp` ~1457) evaluates animated TRS
  and writes back to live actors; callable at fractional times; already used by
  `export_video_loop` for offline sub-frame sampling. `render_tick` re-cooks when
  `m_has_animated_sop_params || m_has_dop_imports` (latest-wins).
- **Houdini-style Geometry with a generic typed attribute system.**
  `src/geometry/geometry.hpp`: `Geometry` holds `AttributeTable` for
  Point/Vertex/Primitive/Detail classes; `Attribute<T>` for
  `T ∈ {float,int,Vec2,Vec3,Vec4,Mat3,Mat4,string}`. Standard attrs: `P`, `N`,
  `Cd`, `uv`, `orient`, `pscale`. Adding `jointIndices`/`jointWeights` is a
  natural `add<Vec4>(...)`.
- **SOP cook → deform → render pipeline.** `SopNode::cookAt(inputs, time)`
  (`src/sops/sop_node.hpp`); deformer pattern in `transform_sop.cpp` (copy input
  `Geometry`, mutate `P`/`N`, return). Cook output → `GeometryConverter` →
  `SceneObject` → `SceneCompiler::compileObject` builds the BLAS from positions;
  `BlasCache` keys on a content hash of positions so changed geometry rebuilds.
- **glTF loader (tinygltf).** `src/scene/gltf_loader.*`: reads meshes (triangles),
  PBR materials (+KHR ext), node transforms, hierarchy (preserved as Actors),
  cameras. **Does NOT read** `model.skins`, `model.animations`, or the
  `JOINTS_0`/`WEIGHTS_0` primitive attributes (insertion points below).

## Architecture

### Skinning is an upstream geometry deform (key decision)

Skinning runs at the **Geometry** level in the SOP cook, producing deformed
vertex positions. Both path-tracer backends then trace the deformed mesh
identically. Consequences:

- **No integrator / megakernel change → no CPU↔Metal lockstep risk.** Skinning
  is upstream of `pt_backend_compare`; parity is automatic. (Contrast with every
  BSDF/MIS change.)
- **Reuses the existing per-frame cook→compile→render path.** A skinning SOP that
  varies with time behaves like any animated SOP: `render_tick` re-cooks it per
  frame, the deformed `Geometry` recompiles, the BLAS rebuilds. DOPs already do
  per-frame geometry, so this path is proven.
- **Known cost:** per-frame BLAS rebuild for the animating character (BlasCache
  content hash changes each frame). Acceptable for v1 (single hero character).
  Later optimization: GPU skinning + BLAS *refit* instead of rebuild.

### Data model

```
Skeleton {
  joints: [ { name, parentIndex, inverseBind: Mat4, bindLocal: TRS } ]
}
AnimationClip {
  name, duration,
  channels: [ { targetJoint, path(T|R|S), times[], values[], interp } ]
}
```

Skinned mesh carries two Point (or Vertex) attributes: `jointIndices` (Vec4,
packed joint ids) and `jointWeights` (Vec4, normalized). Skinning math per cook
at time `t`:

```
localPose[j]   = clip.evaluate(j, t)  (fallback: bindLocal[j])
worldJoint[j]  = worldJoint[parent[j]] * matrix(localPose[j])   // hierarchy walk
skinMatrix[j]  = worldJoint[j] * inverseBind[j]
P'[v]          = Σ_k  weight[v][k] * (skinMatrix[ joint[v][k] ] * P[v])
N'[v]          = normalize( Σ_k weight[v][k] * (skinMatrix3x3 * N[v]) )
```

### Where the skeleton + clip live (v1)

For Phase 1, keep skin + skeleton + clip **with the imported asset** and do the
skinning inside the import path, so no new cross-system plumbing is needed:

- `gltf_loader` parses skins + clips into the `Scene` (new `Scene::skins()` /
  `Scene::animations()`), and stamps `jointIndices`/`jointWeights` onto the
  per-mesh geometry.
- A **skin-aware import SOP** (extend `gltf_import_sop` or add `character_import`)
  at `cookAt(t)`: evaluate the asset's clip at `t` → joint matrices → skinned
  `P`/`N` → output deformed `Geometry`. Self-contained; playback is just the
  existing "animated SOP re-cooks per frame" behavior.

Phase 2 promotes the skeleton to a first-class editable rig and routes joint
tracks into the existing `Parameter`/`ScalarChannel` system so the **Curve Editor
and Dopesheet edit joint animation with zero new UI**.

## Phasing

- **P1 — "A character moves" (this slice).** glTF skin + clip import; skinning
  deformer; baked-clip playback through the timeline. Render a rigged glTF
  (e.g. a CesiumMan / Fox sample) animating in the viewport.
- **P2 — Editable rig.** Promote `Skeleton` to a first-class object; expose
  per-joint TRS as `Parameter` channels so the existing Curve Editor / Dopesheet
  edit them; bake imported clips into editable keys; a viewport joint/bone
  overlay (reuse the gizmo/guides line-pipeline pattern).
- **P3 — IK/FK.** 2-bone analytic IK + CCD/FABRIK chains, pole vectors, FK/IK
  blend; solved at cook time before skinning.
- **P4 — Mocap.** FBX/BVH import → clips; retargeting between skeletons;
  USD skel (`UsdSkel`) once the USD track lands.
- **P5 — Blendshapes.** glTF morph targets (`POSITION` deltas) + weight tracks;
  a blendshape deformer upstream of skinning (face performance).

## Phase 1 — detailed plan (exact insertion points)

### glTF loader — `src/scene/gltf_loader.cpp`
- New accessor helpers (mirror `extractVec3Accessor`): `extractUvec4Accessor`,
  `extractVec4Accessor`, `extractMat4Accessor`, `extractFloatAccessor`.
- `processPrimitive()` (~441–538): after TEXCOORD_0, read `JOINTS_0` + `WEIGHTS_0`
  → `obj.setJoints(...) / obj.setWeights(...)`.
- `loadFromFile()` (~700–780): parse `model.skins` → `SkinData{skeletonRoot,
  joints[], inverseBindMatrices[]}`; parse `model.animations` → `AnimationClip`s.
  Attach skin to the mesh-owning Actor; `scene->addAnimations(...)`.

### Scene / SceneObject
- `SceneObject` (`src/scene/scene_object.hpp`): add `m_joints` (Vec4),
  `m_weights` (Vec4) + accessors + `hasJoints()`.
- `Scene`: add `m_skins`, `m_animations` + accessors.
- New header `src/scene/skeleton.hpp`: `Skeleton`, `AnimationClip`,
  `AnimationChannel` structs + `evaluate(joint, t)` / a `poseAt(t)` helper.

### Geometry bridge — `src/geometry/geometry_converter.cpp`
- `fromSceneObject`: if joints/weights present, `add<Vec4>("jointIndices")` /
  `add<Vec4>("jointWeights")` and fill them.

### Skinning — the import SOP
- Extend `src/sops/nodes/gltf_import_sop.cpp` (or add `character_import_sop`):
  at `cookAt(t)`, if the cached `Scene` has a skin + clip for this mesh, compute
  `skinMatrix[]` and deform `P`/`N`. Mark the node animated so `render_tick`
  re-cooks per frame (set the `m_has_animated_sop_params` condition).
- Pure CPU; no renderer changes — `SceneCompiler` rebuilds the BLAS from the
  deformed positions exactly as it does for any cooked geometry.

### Verify
- `examples/` or a smoke test: load a rigged glTF sample (CesiumMan, Fox, or
  RiggedFigure from the glTF-Sample-Assets), confirm `JOINTS_0/WEIGHTS_0` +
  inverse-bind parse, and that `poseAt(t)` × skinning reproduces the bind pose at
  the clip's t0 (skinned == original when all weights map to bind).
- In the editor: import the asset, press play → the character animates; scrub →
  it follows the playhead. `pt_backend_compare` parity is unaffected by
  construction (deform is upstream), but spot-check one frame CPU vs Metal.

## Risks / notes
- **Per-frame BLAS rebuild** is the main cost; fine for one character, optimize
  later (GPU skin + BLAS refit).
- **Joint-index packing:** glTF `JOINTS_0` may be u8/u16; normalize to a Vec4 of
  float-encoded ids on import. `WEIGHTS_0` may need renormalization.
- **Two-pass loader ordering:** skins/animations reference node indices — parse
  them against the same node→Actor map the hierarchy pass builds.
- **Don't over-build P1:** no rig authoring, no IK, no curve editing of joints
  yet — just import + skin + play. P2 unlocks editing by reusing the curve UI.
