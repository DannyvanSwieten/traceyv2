#include "scene_compiler.hpp"
#include "material_instance.hpp"
#include <iostream>
#include <stdexcept>

// stb_image header (implementation is in gltf_loader.cpp)
#include <stb_image.h>

namespace tracey
{
    int32_t SceneCompiler::loadTexture(Device *device, CompiledScene &result, const Scene &scene, const std::string &texturePath)
    {
        // Check if texture is already loaded
        auto it = result.texturePathToIndex.find(texturePath);
        if (it != result.texturePathToIndex.end())
        {
            return static_cast<int32_t>(it->second);
        }

        int width, height;
        const unsigned char *data = nullptr;
        size_t dataSize = 0;
        bool needsStbiFree = false;
        bool needsPlainFree = false;

        // Check if this is an embedded texture (by checking if it exists in the scene)
        const EmbeddedTexture *embedded = scene.getEmbeddedTexture(texturePath);
        if (embedded)
        {
            // Handle embedded texture

            width = embedded->width;
            height = embedded->height;

            // If the embedded texture has 4 channels, use it directly
            // Otherwise, convert to RGBA
            if (embedded->channels == 4)
            {
                data = embedded->data.data();
                dataSize = embedded->data.size();
                // No free needed - data points to EmbeddedTexture's vector
            }
            else
            {
                // The data is already decoded by tinygltf, not compressed
                // We need to expand it to RGBA manually
                size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
                unsigned char *rgba = static_cast<unsigned char *>(malloc(pixelCount * 4));

                for (size_t i = 0; i < pixelCount; ++i)
                {
                    if (embedded->channels == 1)
                    {
                        // Grayscale
                        rgba[i * 4 + 0] = embedded->data[i];
                        rgba[i * 4 + 1] = embedded->data[i];
                        rgba[i * 4 + 2] = embedded->data[i];
                        rgba[i * 4 + 3] = 255;
                    }
                    else if (embedded->channels == 2)
                    {
                        // Grayscale + Alpha
                        rgba[i * 4 + 0] = embedded->data[i * 2 + 0];
                        rgba[i * 4 + 1] = embedded->data[i * 2 + 0];
                        rgba[i * 4 + 2] = embedded->data[i * 2 + 0];
                        rgba[i * 4 + 3] = embedded->data[i * 2 + 1];
                    }
                    else if (embedded->channels == 3)
                    {
                        // RGB
                        rgba[i * 4 + 0] = embedded->data[i * 3 + 0];
                        rgba[i * 4 + 1] = embedded->data[i * 3 + 1];
                        rgba[i * 4 + 2] = embedded->data[i * 3 + 2];
                        rgba[i * 4 + 3] = 255;
                    }
                }

                data = rgba;
                dataSize = pixelCount * 4;
                needsPlainFree = true;
            }

            std::cout << "Loaded embedded texture: " << texturePath << " (" << width << "x" << height << ")" << std::endl;
        }
        else
        {
            // Load image from file
            int channels;
            unsigned char *fileData = stbi_load(texturePath.c_str(), &width, &height, &channels, 4); // Force RGBA

            if (!fileData)
            {
                std::cerr << "Warning: Failed to load texture: " << texturePath << std::endl;
                return -1;
            }

            data = fileData;
            dataSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            needsStbiFree = true;

            std::cout << "Loaded texture: " << texturePath << " (" << width << "x" << height << ")" << std::endl;
        }

        // Create GPU texture
        Image2D *texture = device->createImage2DWithData(
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            ImageFormat::R8G8B8A8Srgb,
            data,
            dataSize,
            SamplerFilter::Linear,
            SamplerAddressMode::Repeat);

        if (needsStbiFree)
        {
            stbi_image_free(const_cast<unsigned char *>(data));
        }
        else if (needsPlainFree)
        {
            free(const_cast<unsigned char *>(data));
        }

        if (!texture)
        {
            std::cerr << "Warning: Failed to create GPU texture for: " << texturePath << std::endl;
            return -1;
        }

        // Store texture and return index
        int32_t index = static_cast<int32_t>(result.textures.size());
        result.textures.push_back(std::unique_ptr<Image2D>(texture));
        result.texturePathToIndex[texturePath] = static_cast<size_t>(index);

        return index;
    }

    GPUMaterial SceneCompiler::convertMaterial(Device *device, CompiledScene &result, const Scene &scene, const MaterialInstance &material)
    {
        GPUMaterial gpuMat;

        // Load textures
        auto albedoPath = material.getTexture(TEXTURE_ALBEDO);
        if (albedoPath)
        {
            gpuMat.albedoTexIndex = loadTexture(device, result, scene, *albedoPath);
        }

        auto normalPath = material.getTexture(TEXTURE_NORMAL);
        if (normalPath)
        {
            gpuMat.normalTexIndex = loadTexture(device, result, scene, *normalPath);
        }

        auto mrPath = material.getTexture(TEXTURE_METALLIC_ROUGHNESS);
        if (mrPath)
        {
            gpuMat.metallicRoughnessTexIndex = loadTexture(device, result, scene, *mrPath);
        }

        auto emissivePath = material.getTexture(TEXTURE_EMISSIVE);
        if (emissivePath)
        {
            gpuMat.emissiveTexIndex = loadTexture(device, result, scene, *emissivePath);
        }

        auto occlusionPath = material.getTexture(TEXTURE_OCCLUSION);
        if (occlusionPath)
        {
            gpuMat.occlusionTexIndex = loadTexture(device, result, scene, *occlusionPath);
        }

        // Set base color factor
        auto albedo = material.albedo();
        if (albedo)
        {
            gpuMat.baseColorR = albedo->r;
            gpuMat.baseColorG = albedo->g;
            gpuMat.baseColorB = albedo->b;
        }

        // Set metallic-roughness factors
        auto metallic = material.metallic();
        if (metallic)
        {
            gpuMat.metallicFactor = *metallic;
        }

        auto roughness = material.roughness();
        if (roughness)
        {
            gpuMat.roughnessFactor = *roughness;
        }

        // Set emissive factor
        auto emission = material.emission();
        if (emission)
        {
            gpuMat.emissiveR = emission->r;
            gpuMat.emissiveG = emission->g;
            gpuMat.emissiveB = emission->b;
        }

        return gpuMat;
    }
    SceneCompiler::ObjectData SceneCompiler::compileObject(Device *device, const SceneObject &obj, const BVHConfig &bvhConfig)
    {
        ObjectData data;
        data.vertexCount = obj.vertexCount();
        data.nodeCount = 0;

        if (data.vertexCount == 0)
        {
            return data;
        }

        // Create vertex buffer (with VertexBuffer usage for rasterization support)
        size_t bufferSize = data.vertexCount * sizeof(Vec3);
        data.vertexBuffer = std::unique_ptr<Buffer>(
            device->createBuffer(bufferSize,
                                 BufferUsage::AccelerationStructureBuildInput |
                                 BufferUsage::StorageBuffer |
                                 BufferUsage::VertexBuffer));

        // Copy vertex data
        auto *mapped = static_cast<Vec3 *>(data.vertexBuffer->mapForWriting());
        const auto &positions = obj.positions();
        std::copy(positions.begin(), positions.end(), mapped);
        data.vertexBuffer->flush();

        // Store UVs if available
        if (obj.hasUvs())
        {
            data.uvs = obj.uvs();
        }
        else
        {
            // Create default UVs (0,0) for each vertex
            data.uvs.resize(data.vertexCount, Vec2(0.0f, 0.0f));
        }

        // Create BLAS with custom BVH configuration
        data.blas = std::unique_ptr<BottomLevelAccelerationStructure>(
            device->createBottomLevelAccelerationStructure(
                data.vertexBuffer.get(),
                static_cast<uint32_t>(data.vertexCount),
                sizeof(Vec3),
                nullptr, 0,
                bvhConfig));

        // Get node count from the BLAS for statistics
        data.nodeCount = data.blas->nodeCount();

        return data;
    }

    Mat4 SceneCompiler::computeWorldTransform(const Scene &scene, const Actor &actor)
    {
        // For now, just return the actor's local transform
        // TODO: Implement full hierarchy traversal if needed
        return actor.transform().toMatrix();
    }

    SceneCompiler::CompiledScene SceneCompiler::compile(Device *device, const Scene &scene)
    {
        return compile(device, scene, BVHConfig{});
    }

    SceneCompiler::CompiledScene SceneCompiler::compile(Device *device, const Scene &scene, const BVHConfig &bvhConfig)
    {
        CompiledScene result;

        std::cout << "BVH Configuration: leafThreshold=" << bvhConfig.leafThreshold
                  << ", traversalCost=" << bvhConfig.traversalCost
                  << ", intersectionCost=" << bvhConfig.intersectionCost << std::endl;

        // Step 1: Compile all unique objects to BLAS
        const auto &objects = scene.objects();
        std::vector<const BottomLevelAccelerationStructure *> blasPtrs;
        std::vector<Vec2> allUVs; // Collect all UVs across all objects
        std::vector<uint32_t> blasVertexOffsets; // Vertex offset for each BLAS
        bool anyObjectHasUVs = false;
        uint32_t currentVertexOffset = 0;

        for (const auto &[name, objPtr] : objects)
        {
            if (objPtr->vertexCount() == 0)
            {
                continue;
            }

            ObjectData objData = compileObject(device, *objPtr, bvhConfig);
            if (objData.blas)
            {
                result.objectToBlasIndex[name] = result.blases.size();
                blasPtrs.push_back(objData.blas.get());
                result.blases.push_back(std::move(objData.blas));
                result.vertexBuffers.push_back(std::move(objData.vertexBuffer));
                result.vertexCounts.push_back(static_cast<uint32_t>(objData.vertexCount));
                result.totalNodes += objData.nodeCount;
                result.totalTriangles += objData.vertexCount / 3;

                // Store vertex offset for this BLAS
                blasVertexOffsets.push_back(currentVertexOffset);

                // Collect UVs (per vertex -> per triangle vertices)
                if (!objData.uvs.empty())
                {
                    anyObjectHasUVs = true;
                    allUVs.insert(allUVs.end(), objData.uvs.begin(), objData.uvs.end());
                }

                // Update offset for next BLAS
                currentVertexOffset += static_cast<uint32_t>(objData.vertexCount);
            }
        }

        if (result.blases.empty())
        {
            throw std::runtime_error("Scene has no valid geometry objects");
        }

        // Create UV buffer if any object has UVs
        if (anyObjectHasUVs && !allUVs.empty())
        {
            size_t uvBufferSize = allUVs.size() * sizeof(Vec2);
            result.uvBuffer = std::unique_ptr<Buffer>(
                device->createBuffer(static_cast<uint32_t>(uvBufferSize), BufferUsage::StorageBuffer));

            auto *uvMapped = static_cast<Vec2 *>(result.uvBuffer->mapForWriting());
            std::copy(allUVs.begin(), allUVs.end(), uvMapped);
            result.uvBuffer->flush();

            result.hasUVs = true;
            std::cout << "Created UV buffer with " << allUVs.size() << " entries" << std::endl;
        }

        // Step 2: Flatten scene and create instances with materials
        auto sceneNodes = scene.flatten();
        uint32_t materialIndex = 0;

        for (const auto &node : sceneNodes)
        {
            const Actor *actor = node.actor;
            const Mat4 &worldTransform = node.worldTransform;

            for (const auto &sceneInstance : actor->instances())
            {
                const std::string &objectRef = sceneInstance.objectRef();

                // Find the BLAS for this object
                auto it = result.objectToBlasIndex.find(objectRef);
                if (it == result.objectToBlasIndex.end())
                {
                    // Object not found, skip
                    continue;
                }

                size_t blasIndex = it->second;

                // Compute final transform (world * local instance transform)
                Mat4 finalTransform = worldTransform;
                if (sceneInstance.hasLocalTransform())
                {
                    finalTransform = worldTransform * sceneInstance.localTransform()->toMatrix();
                }

                // Create TLAS instance
                Tlas::Instance instance;
                instance.setTransform(finalTransform);
                instance.blasAddress = blasIndex;
                instance.setCustomIndex(materialIndex);
                instance.setMask(0xFF);

                result.instances.push_back(instance);
                result.instanceToMaterialIndex.push_back(materialIndex);

                // Store vertex offset for this instance (based on its BLAS)
                result.instanceVertexOffsets.push_back(blasVertexOffsets[blasIndex]);

                // Convert material and load textures
                GPUMaterial gpuMat = convertMaterial(device, result, scene, sceneInstance.material());
                result.materials.push_back(gpuMat);

                materialIndex++;
            }
        }

        if (result.instances.empty())
        {
            throw std::runtime_error("Scene has no instances to render");
        }

        // Step 3: Create TLAS
        result.tlas = std::unique_ptr<TopLevelAccelerationStructure>(
            device->createTopLevelAccelerationStructure(
                std::span<const BottomLevelAccelerationStructure *>(blasPtrs),
                std::span<const Tlas::Instance>(result.instances)));

        // Step 4: Create material buffer
        if (!result.materials.empty())
        {
            size_t materialBufferSize = result.materials.size() * sizeof(GPUMaterial);
            result.materialBuffer = std::unique_ptr<Buffer>(
                device->createBuffer(static_cast<uint32_t>(materialBufferSize), BufferUsage::StorageBuffer));

            auto *mapped = static_cast<GPUMaterial *>(result.materialBuffer->mapForWriting());
            std::copy(result.materials.begin(), result.materials.end(), mapped);
            result.materialBuffer->flush();

            std::cout << "Created material buffer with " << result.materials.size() << " materials" << std::endl;
            std::cout << "Loaded " << result.textures.size() << " textures" << std::endl;
        }

        // Step 5: Create vertex offset buffer (for correct UV indexing with multiple objects)
        if (!result.instanceVertexOffsets.empty())
        {
            size_t offsetBufferSize = result.instanceVertexOffsets.size() * sizeof(uint32_t);
            result.vertexOffsetBuffer = std::unique_ptr<Buffer>(
                device->createBuffer(static_cast<uint32_t>(offsetBufferSize), BufferUsage::StorageBuffer));

            auto *offsetMapped = static_cast<uint32_t *>(result.vertexOffsetBuffer->mapForWriting());
            std::copy(result.instanceVertexOffsets.begin(), result.instanceVertexOffsets.end(), offsetMapped);
            result.vertexOffsetBuffer->flush();

            std::cout << "Created vertex offset buffer with " << result.instanceVertexOffsets.size() << " offsets" << std::endl;
        }

        // Output BVH statistics
        std::cout << "BVH Statistics: " << result.totalNodes << " nodes, "
                  << result.totalTriangles << " triangles" << std::endl;
        if (result.totalTriangles > 0)
        {
            float nodesPerTri = static_cast<float>(result.totalNodes) / static_cast<float>(result.totalTriangles);
            std::cout << "  Nodes per triangle: " << nodesPerTri << std::endl;
        }

        return result;
    }

    void SceneCompiler::updateTransforms(Device *device, const Scene &scene, CompiledScene &existing)
    {
        // Clear existing instances
        existing.instances.clear();
        existing.instanceToMaterialIndex.clear();
        existing.instanceVertexOffsets.clear();

        // Compute BLAS vertex offsets (needed for instanceVertexOffsets)
        std::vector<uint32_t> blasVertexOffsets;
        uint32_t currentVertexOffset = 0;
        for (uint32_t vertexCount : existing.vertexCounts)
        {
            blasVertexOffsets.push_back(currentVertexOffset);
            currentVertexOffset += vertexCount;
        }

        // Flatten scene and recreate instances with new transforms
        auto sceneNodes = scene.flatten();
        uint32_t materialIndex = 0;

        for (const auto &node : sceneNodes)
        {
            const Actor *actor = node.actor;
            const Mat4 &worldTransform = node.worldTransform;

            for (const auto &sceneInstance : actor->instances())
            {
                const std::string &objectRef = sceneInstance.objectRef();

                // Find the BLAS for this object
                auto it = existing.objectToBlasIndex.find(objectRef);
                if (it == existing.objectToBlasIndex.end())
                {
                    // Object not found, skip (shouldn't happen if scene topology is same)
                    continue;
                }

                size_t blasIndex = it->second;

                // Compute final transform (world * local instance transform)
                Mat4 finalTransform = worldTransform;
                if (sceneInstance.hasLocalTransform())
                {
                    finalTransform = worldTransform * sceneInstance.localTransform()->toMatrix();
                }

                // Create TLAS instance with new transform
                Tlas::Instance instance;
                instance.setTransform(finalTransform);
                instance.blasAddress = blasIndex;
                instance.setCustomIndex(materialIndex);
                instance.setMask(0xFF);

                existing.instances.push_back(instance);
                existing.instanceToMaterialIndex.push_back(materialIndex);

                // Store vertex offset for this instance (based on its BLAS)
                existing.instanceVertexOffsets.push_back(blasVertexOffsets[blasIndex]);

                materialIndex++;
            }
        }

        // Rebuild TLAS with updated instances
        // Extract BLAS pointers from existing BLASes
        std::vector<const BottomLevelAccelerationStructure *> blasPtrs;
        blasPtrs.reserve(existing.blases.size());
        for (const auto &blas : existing.blases)
        {
            blasPtrs.push_back(blas.get());
        }

        existing.tlas = std::unique_ptr<TopLevelAccelerationStructure>(
            device->createTopLevelAccelerationStructure(
                std::span<const BottomLevelAccelerationStructure *>(blasPtrs),
                std::span<const Tlas::Instance>(existing.instances)));

        // Update vertex offset buffer
        if (!existing.instanceVertexOffsets.empty() && existing.vertexOffsetBuffer)
        {
            auto *offsetMapped = static_cast<uint32_t *>(existing.vertexOffsetBuffer->mapForWriting());
            std::copy(existing.instanceVertexOffsets.begin(), existing.instanceVertexOffsets.end(), offsetMapped);
            existing.vertexOffsetBuffer->flush();
        }

        std::cout << "Updated " << existing.instances.size() << " instance transforms (TLAS rebuild)" << std::endl;
    }
}
