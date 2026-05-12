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

        // Base color factor (RGBA)
        float baseColorR = 1.0f;
        float baseColorG = 1.0f;
        float baseColorB = 1.0f;
        float baseColorA = 1.0f;

        // Metallic-roughness factors + emissive start
        float metallicFactor = 0.0f;
        float roughnessFactor = 0.5f;
        float emissiveR = 0.0f;
        float emissiveG = 0.0f;

        // Emissive (continued) + padding
        float emissiveB = 0.0f;
        float _pad3 = 0.0f;
        float _pad4 = 0.0f;
        float _pad5 = 0.0f;
    };
    static_assert(sizeof(GPUMaterial) == 80, "GPUMaterial must be 80 bytes");

    // GPU light record consumed by the path tracer's NEE loop. Three vec4s
    // chosen so the std430 stride matches sizeof(GPULight) exactly and we can
    // index `lights.data[i * 3 + k]` from GLSL without per-field padding
    // headaches. Encoding:
    //   slot 0: xyz = world position (Point), w = LightType as float
    //   slot 1: xyz = world direction (Distant), w = intensity scalar
    //   slot 2: xyz = linear RGB color, w = unused (pad to 16)
    struct GPULight
    {
        float positionAndType[4]{0.0f, 0.0f, 0.0f, 0.0f};
        float directionAndIntensity[4]{0.0f, 0.0f, -1.0f, 0.0f};
        float colorAndPad[4]{0.0f, 0.0f, 0.0f, 0.0f};
    };
    static_assert(sizeof(GPULight) == 48, "GPULight must be 48 bytes (3 * vec4)");

    class SceneCompiler
    {
    public:
        struct CompiledScene
        {
            // Acceleration structures
            std::vector<std::unique_ptr<BottomLevelAccelerationStructure>> blases;
            std::unique_ptr<TopLevelAccelerationStructure> tlas;

            // Vertex buffers (one per unique object)
            std::vector<std::unique_ptr<Buffer>> vertexBuffers;

            // Per-vertex Cd buffers (vec3 per corner, parallel to
            // vertexBuffers). Always populated — when the source SceneObject
            // has no Cd, the buffer is filled with white so the rasterizer's
            // vertex input description always has a valid binding 1.
            std::vector<std::unique_ptr<Buffer>> colorBuffers;

            // Vertex counts per buffer (parallel to vertexBuffers).
            // The buffers store positions (vec3) per triangle vertex, so this
            // doubles as the draw count for non-indexed rasterization.
            std::vector<uint32_t> vertexCounts;

            // Material data
            std::unique_ptr<Buffer> materialBuffer;
            std::vector<uint32_t> instanceToMaterialIndex;
            std::vector<GPUMaterial> materials;

            // Textures (sampled images)
            std::vector<std::unique_ptr<Image2D>> textures;
            std::unordered_map<std::string, size_t> texturePathToIndex;

            // UV buffer (vec2 per vertex, parallel to triangle data)
            std::unique_ptr<Buffer> uvBuffer;
            bool hasUVs = false;

            // Instance data for reference
            std::vector<Tlas::Instance> instances;

            // Per-instance material program lookup. instanceProgramIndex[i] is
            // the index into materialPrograms.headers() (== the GPU programId)
            // for TLAS instance i. instanceProgramIndexBuffer is the SSBO the
            // hit shader and the sort kernel read from.
            MaterialProgramBuffer materialPrograms;
            std::vector<uint32_t> instanceProgramIndex;
            std::unique_ptr<Buffer> instanceProgramIndexBuffer;

            // Per-instance UV-buffer base offset, in *per-vertex* slot
            // counts. The hit shader's `hitInfo.triangleIndex` is local to
            // the BLAS that was hit, so we add this offset before stepping
            // into the global uvBuffer — otherwise every instance's UV
            // lookups would alias to BLAS 0's UVs and produce visibly
            // scrambled texturing on multi-object scenes (e.g. glTF Sponza).
            std::vector<uint32_t> instanceUvOffset;
            std::unique_ptr<Buffer> instanceUvOffsetBuffer;

            // Maps object name to BLAS index
            std::unordered_map<std::string, size_t> objectToBlasIndex;

            // Per-frame light list. lightBuffer is ALWAYS allocated (at least
            // one dummy entry) so the wavefront descriptor set always has a
            // valid SSBO bound; `lightCount` gates the NEE loop in the hit
            // shader. Scene with zero lights → lightCount = 0, NEE skipped.
            std::vector<GPULight> lights;
            std::unique_ptr<Buffer> lightBuffer;
            uint32_t lightCount = 0;

            // BVH statistics
            size_t totalNodes = 0;
            size_t totalTriangles = 0;
        };

        /// Compile scene with default BVH configuration
        static CompiledScene compile(Device *device, const Scene &scene);

        /// Compile scene with custom BVH configuration
        static CompiledScene compile(Device *device, const Scene &scene, const BVHConfig &bvhConfig);

    private:
        struct ObjectData
        {
            std::unique_ptr<Buffer> vertexBuffer;
            std::unique_ptr<Buffer> colorBuffer;  // per-vertex Cd (vec3)
            std::unique_ptr<BottomLevelAccelerationStructure> blas;
            size_t vertexCount;
            size_t nodeCount;
            std::vector<Vec2> uvs; // Per-vertex UVs for this object
        };

        static ObjectData compileObject(Device *device, const SceneObject &obj, const BVHConfig &bvhConfig);
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
