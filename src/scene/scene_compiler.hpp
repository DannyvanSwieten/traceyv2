#pragma once
#include "scene.hpp"
#include "../device/device.hpp"
#include "../device/buffer.hpp"
#include "../device/image_2d.hpp"
#include "../device/bottom_level_acceleration_structure.hpp"
#include "../device/top_level_acceleration_structure.hpp"
#include "../core/tlas.hpp"
#include "../core/blas.hpp"
#include "../shading/material_program/material_program.hpp"
#include <memory>
#include <vector>
#include <unordered_map>

namespace tracey
{
    // GPU material structure for shader consumption
    // Layout: 5 texture indices + sampler bits + 2 pad + 4 baseColor + 4 params + 4 emissive = 80 bytes
    struct GPUMaterial
    {
        // Texture indices (-1 if no texture)
        int32_t albedoTexIndex = -1;
        int32_t normalTexIndex = -1;
        int32_t metallicRoughnessTexIndex = -1;
        int32_t emissiveTexIndex = -1;
        int32_t occlusionTexIndex = -1;

        // 2-bit SamplerKind per texture slot, in this order:
        //   bits 0-1: albedo, 2-3: normal, 4-5: metallicRoughness,
        //   bits 6-7: emissive, 8-9: occlusion.
        // The hit shader picks one of the four bound samplers per access.
        uint32_t samplerBits = 0;

        // Padding for vec4 alignment
        int32_t _pad1 = 0;
        int32_t _pad2 = 0;

        // Base color factor (RGB) + opacity (A). baseColorA doubles as the
        // surface opacity: 1 = opaque, <1 = stochastic see-through.
        float baseColorR = 1.0f;
        float baseColorG = 1.0f;
        float baseColorB = 1.0f;
        float baseColorA = 1.0f; // opacity

        // Metallic-roughness factors + emissive start
        float metallicFactor = 0.0f;
        float roughnessFactor = 0.5f;
        float emissiveR = 0.0f;
        float emissiveG = 0.0f;

        // Emissive (continued) + transparency/emission params. These three
        // were padding; the path tracer reads them by fixed offset (17/18/19)
        // so the layout must stay put. transmissionFactor>0 (+ low metallic)
        // makes the surface refractive glass; iorFactor is the dielectric
        // index; emissiveStrength scales the emissive RGB (HDR emitters).
        float emissiveB = 0.0f;
        float transmissionFactor = 0.0f; // offset 17 (was _pad3)
        float iorFactor = 1.5f;          // offset 18 (was _pad4)
        float emissiveStrength = 1.0f;   // offset 19 (was _pad5)

        // R3 advanced-BSDF lobe factors, read directly by the integrator's uber
        // surface (0 = lobe off, so these defaults preserve the prior look).
        // The MSL kernel reads them by fixed offset (20-23) via as_type<float>,
        // so this order must stay in lockstep with MATERIAL_STRIDE and the
        // getMaterial* accessors in pathtrace_msl.hpp.
        float clearcoatFactor = 0.0f;          // offset 20 — clear-coat weight
        float clearcoatRoughnessFactor = 0.0f; // offset 21 — coat GGX roughness
        float sheenFactor = 0.0f;              // offset 22 — retroreflective sheen weight
        float anisotropyFactor = 0.0f;         // offset 23 — [-1,1] GGX anisotropy

        // R3d subsurface: wrap-diffusion approximation. subsurfaceFactor blends
        // the diffuse NEE toward a softened response that wraps light past the
        // terminator; subsurfaceColor is the scatter tint (skin/wax/marble).
        // 0 = off (diffuse NEE unchanged). Offsets 24-27, read by fixed offset.
        float subsurfaceFactor = 0.0f;         // offset 24 — subsurface weight
        float subsurfaceColorR = 1.0f;         // offset 25 — scatter tint R
        float subsurfaceColorG = 1.0f;         // offset 26 — scatter tint G
        float subsurfaceColorB = 1.0f;         // offset 27 — scatter tint B
    };
    static_assert(sizeof(GPUMaterial) == 112, "GPUMaterial must be 112 bytes");

    // GPU light record consumed by the rasterizer's PBR pass AND the path
    // tracer's NEE / miss shaders. Five vec4s chosen so the std430 stride
    // matches sizeof(GPULight) exactly — GLSL can index `lights.data[i * 5
    // + k]` without per-field padding. Type-conditional encoding:
    //   slot 0: xyz = world position (Point / Area centre); w = LightType
    //   slot 1: xyz = world direction (Distant / Area normal = +Z);
    //           w = intensity scalar
    //   slot 2: xyz = linear RGB color (the multiplier across all types);
    //           w = Point.radius, or Area.sizeX, or 0 otherwise
    //   slot 3: xyz = Dome.skyColor (linear RGB); w = Area.sizeY (the Y
    //           half of the area-light extent — packed alongside sizeX so
    //           rectangle area lights survive in 80 bytes)
    //   slot 4: xyz = Dome.horizonColor; w = packed flag bits (reserved
    //           for future light flags such as cast-shadow toggles)
    //   slot 5: xyz = Dome.groundColor; w = unused (pad to 16)
    //
    // 6 × vec4 = 96 bytes. The shader-side LightRecord struct in
    // position_only.frag + sky_miss.glsl must mirror this exactly.
    struct GPULight
    {
        float positionAndType[4]     {0.0f, 0.0f, 0.0f, 0.0f};
        float directionAndIntensity[4]{0.0f, 0.0f, -1.0f, 0.0f};
        float colorAndExtraX[4]       {0.0f, 0.0f, 0.0f, 0.0f};
        float skyColorAndExtraY[4]    {0.0f, 0.0f, 0.0f, 0.0f};
        float horizonColorAndFlags[4] {0.0f, 0.0f, 0.0f, 0.0f};
        float groundColorAndPad[4]    {0.0f, 0.0f, 0.0f, 0.0f};
    };
    static_assert(sizeof(GPULight) == 96, "GPULight must be 96 bytes (6 * vec4)");

    class BlasCache;

    class SceneCompiler
    {
    public:
        struct CompiledScene
        {
            // Acceleration structures. BLASes are observer pointers into the
            // BlasCache passed to compile() — they outlive any single
            // CompiledScene so a recompile can reuse them when the object's
            // geometry hasn't changed. The TLAS is per-compile (it depends
            // on instance transforms + materials, not just geometry).
            std::vector<const BottomLevelAccelerationStructure *> blases;
            std::unique_ptr<TopLevelAccelerationStructure> tlas;

            // Vertex buffers (one per unique object). Observer pointers into
            // the BlasCache for the same reason as blases above — re-uploading
            // GPU vertex data per cook on a static glTF is the main cost the
            // cache eliminates.
            std::vector<const Buffer *> vertexBuffers;

            // Per-vertex Cd buffers (vec3 per corner, parallel to
            // vertexBuffers). Always populated — when the source SceneObject
            // has no Cd, the buffer is filled with white so the rasterizer's
            // vertex input description always has a valid binding 1. Same
            // observer-into-cache contract as vertexBuffers.
            std::vector<const Buffer *> colorBuffers;

            // Keep-alive: shared_ptr copies of every BlasCache resource the
            // observer pointers above (blases / vertexBuffers / colorBuffers)
            // point at. These pin the GPU resources for the lifetime of THIS
            // CompiledScene, so the async render worker — which may still hold a
            // shared_ptr<CompiledScene> snapshot after the engine recompiled and
            // the cache evicted those entries — never dereferences a freed
            // buffer/BLAS. Not indexed directly; existence is what matters.
            std::vector<std::shared_ptr<const BottomLevelAccelerationStructure>> retainedBlases;
            std::vector<std::shared_ptr<const Buffer>> retainedBuffers;

            // Vertex counts per buffer (parallel to vertexBuffers).
            // The buffers store positions (vec3) per triangle vertex, so this
            // doubles as the draw count for non-indexed rasterization.
            std::vector<uint32_t> vertexCounts;

            // Material data
            std::unique_ptr<Buffer> materialBuffer;
            std::vector<uint32_t> instanceToMaterialIndex;
            std::vector<GPUMaterial> materials;

            // Source actor UID per TLAS instance (parallel to `instances`). Lets
            // a viewport ray-cast (pick) resolve a hit instanceId back to the
            // actor it came from, for click-to-select.
            std::vector<uint64_t> instanceToActorUid;

            // Textures (sampled images)
            std::vector<std::unique_ptr<Image2D>> textures;
            std::unordered_map<std::string, size_t> texturePathToIndex;

            // CPU copy of every sampled texture, index-parallel to `textures`.
            // The decoded pixels would otherwise be freed right after the
            // Vulkan upload; path tracer backends that own their textures
            // (Metal builds MTLTextures, the CPU backend samples directly)
            // read from here instead of the device images.
            struct TextureSource
            {
                uint32_t width = 0;
                uint32_t height = 0;
                bool srgb = false;            // colour data (albedo/emissive)
                std::vector<uint8_t> rgba8;   // tightly packed RGBA8
            };
            std::vector<TextureSource> textureSources;

            // UV buffer (vec2 per vertex, parallel to triangle data)
            std::unique_ptr<Buffer> uvBuffer;
            bool hasUVs = false;

            // Per-vertex normal buffer (vec3 per vertex, parallel to UVs and
            // indexed by the same per-instance offset). When `hasNormals`
            // is true the hit shader interpolates these with the barycentric
            // coordinates of the hit; when false (or when the per-instance
            // slice is all zero), it falls back to the BLAS face normal.
            std::unique_ptr<Buffer> normalBuffer;
            bool hasNormals = false;

            // Per-vertex position buffer (vec3-in-vec4 per vertex, parallel to
            // UVs/normals and indexed by the same per-instance offset). The CPU
            // path tracer reads it to reconstruct a hit triangle's object-space
            // vertices for UV-aligned tangent derivation (anisotropic GGX). The
            // Metal backend already binds an equivalent concatenated positions
            // buffer for its hit shader, so both index by the same `base`.
            std::unique_ptr<Buffer> positionBuffer;

            // Instance data for reference
            std::vector<Tlas::Instance> instances;

            // R4 motion blur: shutter-close pose for each instance, parallel to
            // `instances` (the shutter-open pose). Equal to `instances` when
            // there's no motion; `hasMotion` gates the blur so static scenes
            // stay bit-identical. Populated by the sequence renderer, which
            // re-evaluates the scene at t + shutter and recompiles the TLAS.
            std::vector<Tlas::Instance> instancesEnd;
            bool hasMotion = false;

            // Per-instance material program lookup and UV base offset.
            // instanceProgramIndex[i] is the GPU programId (= index into
            // materialPrograms.headers()); instanceUvOffset[i] is the
            // per-vertex base offset into the global uvBuffer/normalBuffer
            // for TLAS instance i. The hit shader's hitInfo.triangleIndex
            // is BLAS-local so we add this offset before stepping into the
            // global arrays — otherwise every instance's lookups alias to
            // BLAS 0 and multi-object scenes show scrambled UVs/normals.
            //
            // On the GPU side these two parallel uint arrays are uploaded
            // as a single uvec2[] (instanceDataBuffer) so the wavefront
            // compute pipeline doesn't blow the per-stage storage-buffer
            // descriptor budget on MoltenVK / Apple GPUs (31 SSBs/stage).
            // .x = programId, .y = uvOffset.
            MaterialProgramBuffer materialPrograms;
            std::vector<uint32_t> instanceProgramIndex;
            std::vector<uint32_t> instanceUvOffset;
            std::unique_ptr<Buffer> instanceDataBuffer;

            // Maps object name to BLAS index
            std::unordered_map<std::string, size_t> objectToBlasIndex;

            // Per-frame light list. lightBuffer is ALWAYS allocated (at least
            // one dummy entry) so the wavefront descriptor set always has a
            // valid SSBO bound; `lightCount` gates the NEE loop in the hit
            // shader. Scene with zero lights → lightCount = 0, NEE skipped.
            std::vector<GPULight> lights;
            std::unique_ptr<Buffer> lightBuffer;
            uint32_t lightCount = 0;

            // Emissive geometry, flattened to world-space triangles, for the
            // path tracer's next-event estimation (sampling emitters directly
            // instead of relying on random bounces hitting them). Built from
            // any instance whose material emission is non-zero; the backends
            // upload/copy this and sample it with shadow rays. Capped to keep
            // a high-poly emissive mesh from exploding the list.
            struct EmissiveTri
            {
                Vec3 p0{0.0f}, p1{0.0f}, p2{0.0f}; // world-space
                Vec3 emission{0.0f};               // radiance (incl. strength)
                float area = 0.0f;
            };
            std::vector<EmissiveTri> emitters;

            // BVH statistics
            size_t totalNodes = 0;
            size_t totalTriangles = 0;

            // Monotonic change stamp. Bumped on every compile() and by every
            // in-place mutation (RenderEngine::refresh_tlas_only). Backends
            // that cache per-scene resources (acceleration structures, scene
            // buffer copies) compare this instead of re-uploading per frame.
            uint64_t revision = 0;
        };

        // Allocate the next scene revision stamp. Used by compile() and by
        // callers that mutate a CompiledScene in place.
        static uint64_t nextSceneRevision();

        /// Compile scene with default BVH configuration
        static CompiledScene compile(Device *device, const Scene &scene);
        static CompiledScene compile(Device *device, const Scene &scene, BlasCache *cache);

        /// Compile scene with custom BVH configuration
        static CompiledScene compile(Device *device, const Scene &scene, const BVHConfig &bvhConfig);
        static CompiledScene compile(Device *device, const Scene &scene,
                                     const BVHConfig &bvhConfig, BlasCache *cache);

        /// Compile scene with full control. When `buildAccelerationStructures`
        /// is false the BLAS BVH build and TLAS construction are both skipped
        /// — the resulting CompiledScene still carries vertex / color buffers
        /// and instance transforms (the rasterizer's full input set), but no
        /// `blases` entries, no `tlas`, and `totalNodes` reports zero. The
        /// editor sets this to false when the path-traced inset preview is
        /// off, since the rasterizer doesn't traverse a BVH. The `cache`
        /// parameter is also ignored in that mode — there's nothing to cache
        /// because cached entries are inseparable from their BLAS.
        static CompiledScene compile(Device *device, const Scene &scene,
                                     const BVHConfig &bvhConfig, BlasCache *cache,
                                     bool buildAccelerationStructures);

        // Analytic-light data: the GPULight list + its uploaded buffer. Factored
        // out of compile() so an editor light edit (add / delete / tweak) can
        // refresh lighting IN PLACE — rebuilding only this small buffer — instead
        // of recompiling all geometry (which re-aggregates + re-uploads every
        // vertex buffer and rebuilds the TLAS, freezing the UI on large scenes).
        // Does NOT touch emitters: those derive from emissive geometry, which a
        // light edit never changes.
        struct LightData
        {
            std::vector<GPULight> lights;
            uint32_t lightCount = 0;
            std::unique_ptr<Buffer> lightBuffer;
        };
        static LightData compileLights(Device *device, const Scene &scene);

        // Material-program aggregation result: the program buffer (program 0 =
        // passthrough, then each unique shader graph in first-encounter order) and
        // a graph-JSON → {programId, rasterizer preview albedo} map. Factored out
        // of compile()'s instance loop so an in-place shader-graph edit refresh can
        // rebuild the program buffer with IDENTICAL program IDs — which is what
        // keeps the instanceProgramIndex compile() baked still valid (the edit
        // changes bytecode, not which program each instance uses).
        struct ActorMaterialEntry
        {
            uint32_t programId = 0;
            std::optional<Vec3> previewAlbedo;
        };
        struct MaterialProgramData
        {
            MaterialProgramBuffer programs;
            std::unordered_map<std::string, ActorMaterialEntry> graphToEntry;
        };
        static MaterialProgramData compileMaterialPrograms(const Scene &scene);

    private:
        struct ObjectData
        {
            std::unique_ptr<Buffer> vertexBuffer;
            std::unique_ptr<Buffer> colorBuffer;  // per-vertex Cd (vec3)
            std::unique_ptr<BottomLevelAccelerationStructure> blas;
            size_t vertexCount;
            size_t nodeCount;
            std::vector<Vec2> uvs;       // Per-vertex UVs for this object
            std::vector<Vec3> normals;   // Per-vertex normals for this object
            bool hasNormals = false;
        };

        // When `buildAccelerationStructures` is false the BLAS build is
        // skipped — vertex / color / uv / normal data is still uploaded
        // because the rasterizer needs it.
        static ObjectData compileObject(Device *device, const SceneObject &obj,
                                        const BVHConfig &bvhConfig,
                                        bool buildAccelerationStructures = true);
        static Mat4 computeWorldTransform(const Scene &scene, const Actor &actor);

        // Load a texture and return its index, or -1 if failed. `isColorData`
        // selects the GPU format: true → R8G8B8A8Srgb (gamma-decoded on
        // sample, for albedo/emissive), false → R8G8B8A8Unorm (raw bytes,
        // for normal/MR/occlusion). Two distinct uploads are kept if the
        // same texture path is referenced as both color and data — the
        // cache key includes the format.
        static int32_t loadTexture(Device *device, CompiledScene &result, const Scene &scene, const std::string &texturePath, bool isColorData);

        // Convert MaterialInstance to GPUMaterial, loading textures as needed
        static GPUMaterial convertMaterial(Device *device, CompiledScene &result, const Scene &scene, const MaterialInstance &material);
    };
}
