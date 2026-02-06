#pragma once
#include "scene.hpp"
#include "../device/device.hpp"
#include "../device/buffer.hpp"
#include "../device/image_2d.hpp"
#include "../device/bottom_level_acceleration_structure.hpp"
#include "../device/top_level_acceleration_structure.hpp"
#include "../core/tlas.hpp"
#include "../core/blas.hpp"
#include <memory>
#include <vector>
#include <unordered_map>

namespace tracey
{
    // GPU material structure for shader consumption
    // Layout: 5 texture indices + 3 pad + baseColor + params + emissive + clearcoat/sheen = 96 bytes
    struct GPUMaterial
    {
        // Texture indices (-1 if no texture)
        int32_t albedoTexIndex = -1;
        int32_t normalTexIndex = -1;
        int32_t metallicRoughnessTexIndex = -1;
        int32_t emissiveTexIndex = -1;
        int32_t occlusionTexIndex = -1;

        // Padding for vec4 alignment
        int32_t _pad0 = 0;
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

        // Emissive (continued) + clearcoat parameters
        float emissiveB = 0.0f;
        float clearcoat = 0.0f;           // Clearcoat strength [0, 1]
        float clearcoatRoughness = 0.1f;  // Clearcoat roughness
        float _pad3 = 0.0f;

        // Sheen parameters
        float sheenColorR = 0.0f;
        float sheenColorG = 0.0f;
        float sheenColorB = 0.0f;
        float sheenRoughness = 0.5f;
    };
    static_assert(sizeof(GPUMaterial) == 96, "GPUMaterial must be 96 bytes");

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
            std::vector<uint32_t> vertexCounts; // Parallel to vertexBuffers - vertices per buffer

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

            // Vertex offset buffer (uint per instance - offset into UV buffer for this instance's BLAS)
            std::unique_ptr<Buffer> vertexOffsetBuffer;
            std::vector<uint32_t> instanceVertexOffsets;

            // Instance data for reference
            std::vector<Tlas::Instance> instances;

            // Maps object name to BLAS index
            std::unordered_map<std::string, size_t> objectToBlasIndex;

            // BVH statistics
            size_t totalNodes = 0;
            size_t totalTriangles = 0;

            // Environment map (HDR skybox)
            int32_t envMapIndex = -1;
            float envIntensity = 1.0f;
            float envRotation = 0.0f;
        };

        /// Compile scene with default BVH configuration
        static CompiledScene compile(Device *device, const Scene &scene);

        /// Compile scene with custom BVH configuration
        static CompiledScene compile(Device *device, const Scene &scene, const BVHConfig &bvhConfig);

        /// Update only instance transforms and rebuild TLAS (fast update for animations)
        /// Keeps existing BLASes, vertex buffers, materials, and textures
        /// @param device The device to use
        /// @param scene The updated scene with new transforms
        /// @param existing The existing compiled scene to update
        static void updateTransforms(Device *device, const Scene &scene, CompiledScene &existing);

        /// Update only material properties in the GPU buffer (fast update for material editing)
        /// Keeps existing BLASes, TLAS, vertex buffers, and textures
        /// Note: Does NOT update textures - only scalar/vector material properties
        /// @param device The device to use (for GPU sync)
        /// @param scene The updated scene with new material values
        /// @param existing The existing compiled scene to update
        static void updateMaterials(Device *device, const Scene &scene, CompiledScene &existing);

    private:
        struct ObjectData
        {
            std::unique_ptr<Buffer> vertexBuffer;
            std::unique_ptr<BottomLevelAccelerationStructure> blas;
            size_t vertexCount;
            size_t nodeCount;
            std::vector<Vec2> uvs; // Per-vertex UVs for this object
        };

        static ObjectData compileObject(Device *device, const SceneObject &obj, const BVHConfig &bvhConfig);
        static Mat4 computeWorldTransform(const Scene &scene, const Actor &actor);

        // Load a texture and return its index, or -1 if failed
        // Use R8G8B8A8Srgb for color textures (albedo, emissive), R8G8B8A8Unorm for data textures (normal, metallic-roughness, occlusion)
        static int32_t loadTexture(Device *device, CompiledScene &result, const Scene &scene, const std::string &texturePath, ImageFormat format);

        // Load an HDR texture (for environment maps) and return its index, or -1 if failed
        static int32_t loadHDRTexture(Device *device, CompiledScene &result, const std::string &texturePath);

        // Convert MaterialInstance to GPUMaterial, loading textures as needed
        static GPUMaterial convertMaterial(Device *device, CompiledScene &result, const Scene &scene, const MaterialInstance &material);
    };
}
