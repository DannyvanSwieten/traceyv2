#include "scene_compiler.hpp"
#include "blas_cache.hpp"
#include "material_instance.hpp"
#include "../graph/graphs/shader_graph/compiler.hpp"
#include "../graph/graphs/shader_graph/serialization.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

// stb_image header (implementation is in gltf_loader.cpp)
#include <stb_image.h>

namespace tracey
{
    int32_t SceneCompiler::loadTexture(Device *device, CompiledScene &result, const Scene &scene, const std::string &texturePath, bool isColorData)
    {
        // Cache key folds in the format hint so the same source file can
        // produce both a sRGB albedo upload and a Unorm normal/MR upload
        // without one stomping the other. Suffix is opaque — just keeps the
        // two paths distinguishable in the map.
        const std::string cacheKey = texturePath + (isColorData ? "|srgb" : "|linear");
        // Check if texture is already loaded
        auto it = result.texturePathToIndex.find(cacheKey);
        if (it != result.texturePathToIndex.end())
        {
            return static_cast<int32_t>(it->second);
        }

        int width, height;
        const unsigned char *data = nullptr;
        size_t dataSize = 0;
        bool needsStbiFree = false;
        bool needsPlainFree = false;

        // Handle embedded textures
        if (texturePath.find("embedded:") == 0)
        {
            const EmbeddedTexture *embedded = scene.getEmbeddedTexture(texturePath);
            if (!embedded)
            {
                std::cerr << "Warning: Embedded texture not found: " << texturePath << std::endl;
                return -1;
            }

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

        // Create GPU texture. Color-data textures (albedo, emissive) use
        // sRGB so the hardware decodes gamma on sample; data textures
        // (normal, metallic-roughness, occlusion) use Unorm so the byte
        // values reach the shader unmodified.
        const ImageFormat texFormat = isColorData ? ImageFormat::R8G8B8A8Srgb
                                                  : ImageFormat::R8G8B8A8Unorm;
        Image2D *texture = device->createImage2DWithData(
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            texFormat,
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
        result.texturePathToIndex[cacheKey] = static_cast<size_t>(index);

        return index;
    }

    GPUMaterial SceneCompiler::convertMaterial(Device *device, CompiledScene &result, const Scene &scene, const MaterialInstance &material)
    {
        GPUMaterial gpuMat;

        // Pack per-texture sampler choices into samplerBits (2 bits per slot).
        // The hit shader reads back via samplerBits >> (slot * 2) & 3.
        auto packSampler = [&](SamplerKind k, uint32_t slot) {
            gpuMat.samplerBits |= (static_cast<uint32_t>(k) & 0x3u) << (slot * 2u);
        };

        // Load textures. Albedo/emissive carry color data and must be loaded
        // as sRGB; normal/MR/occlusion are raw data and need Unorm. Loading
        // a normal map as sRGB silently gamma-decodes the byte triplets and
        // produces wrong shading.
        auto albedoPath = material.getTexture(TEXTURE_ALBEDO);
        if (albedoPath)
        {
            gpuMat.albedoTexIndex = loadTexture(device, result, scene, *albedoPath, /*isColorData=*/true);
            packSampler(material.getTextureSampler(TEXTURE_ALBEDO), 0u);
        }

        auto normalPath = material.getTexture(TEXTURE_NORMAL);
        if (normalPath)
        {
            gpuMat.normalTexIndex = loadTexture(device, result, scene, *normalPath, /*isColorData=*/false);
            packSampler(material.getTextureSampler(TEXTURE_NORMAL), 1u);
        }

        auto mrPath = material.getTexture(TEXTURE_METALLIC_ROUGHNESS);
        if (mrPath)
        {
            gpuMat.metallicRoughnessTexIndex = loadTexture(device, result, scene, *mrPath, /*isColorData=*/false);
            packSampler(material.getTextureSampler(TEXTURE_METALLIC_ROUGHNESS), 2u);
        }

        auto emissivePath = material.getTexture(TEXTURE_EMISSIVE);
        if (emissivePath)
        {
            gpuMat.emissiveTexIndex = loadTexture(device, result, scene, *emissivePath, /*isColorData=*/true);
            packSampler(material.getTextureSampler(TEXTURE_EMISSIVE), 3u);
        }

        auto occlusionPath = material.getTexture(TEXTURE_OCCLUSION);
        if (occlusionPath)
        {
            gpuMat.occlusionTexIndex = loadTexture(device, result, scene, *occlusionPath, /*isColorData=*/false);
            packSampler(material.getTextureSampler(TEXTURE_OCCLUSION), 4u);
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

        // Create vertex buffer
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

        // Per-vertex color buffer. Always allocated, even when the object
        // carries no Cd, so the rasterizer's vertex input (binding 1) always
        // has a valid buffer to bind. Missing → white per-vertex.
        data.colorBuffer = std::unique_ptr<Buffer>(
            device->createBuffer(bufferSize, BufferUsage::VertexBuffer));
        auto *colorMapped = static_cast<Vec3 *>(data.colorBuffer->mapForWriting());
        if (obj.hasColors())
        {
            const auto &colors = obj.colors();
            // Defensive: copy min(positions, colors) and pad the rest with
            // white so a malformed SceneObject can't read past the buffer.
            const size_t n = std::min(colors.size(), positions.size());
            std::copy(colors.begin(), colors.begin() + n, colorMapped);
            for (size_t i = n; i < positions.size(); ++i) colorMapped[i] = Vec3(1.0f);
        }
        else
        {
            for (size_t i = 0; i < positions.size(); ++i) colorMapped[i] = Vec3(1.0f);
        }
        data.colorBuffer->flush();

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

        // Store per-vertex normals if available. Always sized to vertexCount
        // (zero-padded when the source SceneObject had no N) so the
        // per-instance offset arithmetic stays valid even across mixed
        // scenes with some normal-bearing objects and some without.
        data.hasNormals = obj.hasNormals();
        if (data.hasNormals)
        {
            data.normals = obj.normals();
            // Pad in case the source vector is shorter than the vertex count
            // (defensive — shouldn't happen with the existing converters).
            if (data.normals.size() < data.vertexCount)
                data.normals.resize(data.vertexCount, Vec3(0.0f));
        }
        else
        {
            data.normals.resize(data.vertexCount, Vec3(0.0f));
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
        return compile(device, scene, BVHConfig{}, nullptr);
    }

    SceneCompiler::CompiledScene SceneCompiler::compile(Device *device, const Scene &scene, BlasCache *cache)
    {
        return compile(device, scene, BVHConfig{}, cache);
    }

    SceneCompiler::CompiledScene SceneCompiler::compile(Device *device, const Scene &scene, const BVHConfig &bvhConfig)
    {
        return compile(device, scene, bvhConfig, nullptr);
    }

    SceneCompiler::CompiledScene SceneCompiler::compile(Device *device, const Scene &scene,
                                                        const BVHConfig &bvhConfig, BlasCache *cache)
    {
        CompiledScene result;

        std::cout << "BVH Configuration: leafThreshold=" << bvhConfig.leafThreshold
                  << ", traversalCost=" << bvhConfig.traversalCost
                  << ", intersectionCost=" << bvhConfig.intersectionCost << std::endl;

        // Step 1: Compile all unique objects to BLAS — or pull them from the
        // cache when the object's positions + indices fingerprint matches a
        // previous compile's entry. Cache hits skip the BLAS build AND the
        // GPU vertex-buffer upload, which is the bulk of the expensive work.
        const auto &objects = scene.objects();
        std::vector<const BottomLevelAccelerationStructure *> blasPtrs;
        std::vector<Vec2> allUVs; // Collect all UVs across all objects
        bool anyObjectHasUVs = false;
        // Per-BLAS start offset into allUVs, in per-vertex slot counts.
        // The hit shader's triangleIndex is BLAS-local, so each instance
        // needs to translate it through this offset to read the right
        // slice of the global uvBuffer. Pads every object (even those
        // without UVs) with allUVs.size() to keep parallel indexing.
        std::vector<uint32_t> blasUvStart;
        // Same shape for per-vertex normals. We reuse the same offset
        // array (UV start == normal start because both store one entry
        // per vertex), so only one buffer of offsets is uploaded; the
        // hit shader uses it for both UV and normal lookup. Stored as
        // Vec4 (xyz = normal, w unused) because std430 arrays of vec3
        // have a 16-byte stride that wouldn't match a packed
        // std::vector<Vec3> — the resulting reads would scramble
        // adjacent entries' components into "normals" that look like
        // flat shading.
        std::vector<Vec4> allNormals;
        bool anyObjectHasNormals = false;

        for (const auto &[name, objPtr] : objects)
        {
            if (objPtr->vertexCount() == 0)
            {
                continue;
            }

            const uint64_t hash = objPtr->contentHash();
            const BlasCache::Entry *entry = nullptr;
            if (cache)
            {
                entry = cache->lookup(name, hash);
            }

            if (!entry)
            {
                ObjectData objData = compileObject(device, *objPtr, bvhConfig);
                if (!objData.blas) continue;

                BlasCache::Entry fresh;
                fresh.blas = std::move(objData.blas);
                fresh.vertexBuffer = std::move(objData.vertexBuffer);
                fresh.colorBuffer = std::move(objData.colorBuffer);
                fresh.vertexCount = objData.vertexCount;
                fresh.uvs = std::move(objData.uvs);
                fresh.hasUvs = objPtr->hasUvs();
                fresh.normals = std::move(objData.normals);
                fresh.hasNormals = objData.hasNormals;
                fresh.contentHash = hash;
                if (cache)
                {
                    entry = cache->insert(name, std::move(fresh));
                }
                else
                {
                    // No cache → the CompiledScene needs to own the BLAS, but
                    // we just changed it to observer pointers. The headless
                    // path (smoke tests, scene_renderer example) always
                    // passes a cache so this branch is for unit-test paths
                    // that build a Scene directly and call compile(); they
                    // discard the result right after rendering, so we leak
                    // into a static holder for the duration of the call.
                    // Simpler than introducing two compile() flavors.
                    static thread_local std::vector<BlasCache::Entry> oneShot;
                    oneShot.push_back(std::move(fresh));
                    entry = &oneShot.back();
                }
            }

            result.objectToBlasIndex[name] = result.blases.size();
            blasPtrs.push_back(entry->blas.get());
            result.blases.push_back(entry->blas.get());
            result.vertexBuffers.push_back(entry->vertexBuffer.get());
            result.colorBuffers.push_back(entry->colorBuffer.get());
            result.vertexCounts.push_back(static_cast<uint32_t>(entry->vertexCount));
            result.totalTriangles += entry->vertexCount / 3;
            if (entry->blas) result.totalNodes += entry->blas->nodeCount();

            // Snapshot the offset *before* appending — that's where this
            // BLAS's UVs begin in allUVs.
            blasUvStart.push_back(static_cast<uint32_t>(allUVs.size()));
            anyObjectHasUVs = anyObjectHasUVs || entry->hasUvs;
            allUVs.insert(allUVs.end(), entry->uvs.begin(), entry->uvs.end());

            // Normals concatenated parallel to UVs (one entry per vertex,
            // same per-BLAS offset). Packed Vec3 → Vec4 here so the
            // upload matches std430's 16-byte array stride.
            anyObjectHasNormals = anyObjectHasNormals || entry->hasNormals;
            if (entry->normals.size() == entry->vertexCount)
            {
                allNormals.reserve(allNormals.size() + entry->normals.size());
                for (const auto &n : entry->normals)
                    allNormals.emplace_back(n.x, n.y, n.z, 0.0f);
            }
            else
            {
                // Defensive zero-padding when the cache entry didn't
                // populate normals (older entry from before the field
                // existed). Keeps per-instance offset arithmetic valid.
                allNormals.insert(allNormals.end(), entry->vertexCount, Vec4(0.0f));
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

        // Create the normal buffer even when no object carries N. The
        // wavefront descriptor set always expects a bound SSBO at the
        // `normalBuffer` slot (declared unconditionally in the pipeline
        // layout); leaving it null would produce undefined reads on
        // some MoltenVK builds. When `hasNormals` is false the slice is
        // all zeros and the hit shader's getHitNormal detects that and
        // falls back to the BLAS face normal.
        if (!allNormals.empty())
        {
            const size_t bytes = allNormals.size() * sizeof(Vec4);
            result.normalBuffer = std::unique_ptr<Buffer>(
                device->createBuffer(static_cast<uint32_t>(bytes), BufferUsage::StorageBuffer));
            auto *mapped = static_cast<Vec4 *>(result.normalBuffer->mapForWriting());
            std::copy(allNormals.begin(), allNormals.end(), mapped);
            result.normalBuffer->flush();
            result.hasNormals = anyObjectHasNormals;
        }

        // Step 2: Flatten scene and create instances with materials
        auto sceneNodes = scene.flatten();
        uint32_t materialIndex = 0;

        // Material program aggregation: program 0 is always the passthrough
        // (used by any actor without an attached graph). Subsequent programs
        // are added on first encounter of a unique graph JSON. The lookup
        // table keyed by JSON string lets us dedupe across actors.
        result.materialPrograms.addProgram(makePassthroughProgram());
        std::unordered_map<std::string, uint32_t> graphJsonToProgramId;

        for (const auto &node : sceneNodes)
        {
            const Actor *actor = node.actor;
            const Mat4 &worldTransform = node.worldTransform;

            // Hidden actors contribute no instances to the path tracer's TLAS
            // or the rasterizer's draw list. Children are still walked because
            // Scene::flatten emits one SceneNode per actor with its world
            // transform already composed, so skipping a parent does NOT
            // suppress its children — that matches Houdini-style "display
            // flag" semantics (hide a subnet, its children remain visible).
            if (!actor->visible()) continue;

            // Resolve this actor's program id once -- all instances under the
            // actor share it. Empty graph -> passthrough at index 0.
            uint32_t actorProgramId = 0;
            const std::string &graphJson = actor->materialGraphJson();
            if (!graphJson.empty())
            {
                auto cached = graphJsonToProgramId.find(graphJson);
                if (cached != graphJsonToProgramId.end())
                {
                    actorProgramId = cached->second;
                }
                else
                {
                    try
                    {
                        auto graph = deserializeShaderGraph(graphJson);
                        if (graph)
                        {
                            MaterialProgram program = compileShaderGraph(*graph);
                            actorProgramId = result.materialPrograms.addProgram(program);
                            graphJsonToProgramId.emplace(graphJson, actorProgramId);
                        }
                    }
                    catch (const std::exception &e)
                    {
                        std::cerr << "SceneCompiler: failed to compile actor graph: "
                                  << e.what() << " -- using passthrough" << std::endl;
                        actorProgramId = 0;
                    }
                }
            }

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
                result.instanceProgramIndex.push_back(actorProgramId);
                result.instanceUvOffset.push_back(blasUvStart[blasIndex]);

                // Convert material and load textures
                GPUMaterial gpuMat = convertMaterial(device, result, scene, sceneInstance.material());
                result.materials.push_back(gpuMat);

                materialIndex++;
            }
        }

        // Light gather: walk the same SceneNode list so light actors inherit
        // parent transforms identically to geometry actors. Hidden actors
        // still emit lights — the Houdini display-flag analogue is geometry
        // visibility, not emission. The lightBuffer is always allocated
        // (at least one zero entry) so the wavefront descriptor set always
        // has a valid SSBO to bind; lightCount = 0 gates the NEE loop.
        for (const auto &node : sceneNodes)
        {
            const Actor *actor = node.actor;
            if (!actor->hasLight()) continue;

            const Light *l = actor->light();
            const Mat4 &xform = node.worldTransform;

            GPULight gpu;
            const Vec3 pos = transformPoint(xform, Vec3(0.0f, 0.0f, 0.0f));
            const Vec3 dir = transformVector(xform, Vec3(0.0f, 0.0f, -1.0f));

            gpu.positionAndType[0] = pos.x;
            gpu.positionAndType[1] = pos.y;
            gpu.positionAndType[2] = pos.z;
            gpu.positionAndType[3] = static_cast<float>(static_cast<int>(l->type));

            gpu.directionAndIntensity[0] = dir.x;
            gpu.directionAndIntensity[1] = dir.y;
            gpu.directionAndIntensity[2] = dir.z;
            gpu.directionAndIntensity[3] = l->intensity;

            gpu.colorAndPad[0] = l->color.x;
            gpu.colorAndPad[1] = l->color.y;
            gpu.colorAndPad[2] = l->color.z;
            gpu.colorAndPad[3] = 0.0f;

            result.lights.push_back(gpu);
        }

        result.lightCount = static_cast<uint32_t>(result.lights.size());
        {
            // Always upload at least one light slot — Vulkan binds need a
            // non-null SSBO; the shader skips iteration when lightCount == 0.
            const size_t lightSlots = result.lights.empty() ? 1 : result.lights.size();
            const size_t bytes = lightSlots * sizeof(GPULight);
            result.lightBuffer = std::unique_ptr<Buffer>(
                device->createBuffer(static_cast<uint32_t>(bytes), BufferUsage::StorageBuffer));
            auto *mapped = static_cast<GPULight *>(result.lightBuffer->mapForWriting());
            if (result.lights.empty())
            {
                GPULight dummy{};
                std::memcpy(mapped, &dummy, sizeof(GPULight));
            }
            else
            {
                std::memcpy(mapped, result.lights.data(), bytes);
            }
            result.lightBuffer->flush();
        }

        // Empty scene is a valid editor state — every actor may be hidden,
        // a graph may emit only subnet markers / lights, or a fresh project
        // may have no object_output yet. Returning a CompiledScene with no
        // TLAS lets the renderer skip the path-tracer dispatch instead of
        // crashing the process; the caller gates on
        // `has_renderable_geometry()` before sampling.
        if (result.instances.empty())
        {
            return result;
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

        // Step 5: Create instance->program SSBO so the hit shader and the
        // material-ID sort kernel can resolve programId from instanceIndex.
        if (!result.instanceProgramIndex.empty())
        {
            const size_t bytes = result.instanceProgramIndex.size() * sizeof(uint32_t);
            result.instanceProgramIndexBuffer = std::unique_ptr<Buffer>(
                device->createBuffer(static_cast<uint32_t>(bytes), BufferUsage::StorageBuffer));
            std::memcpy(result.instanceProgramIndexBuffer->mapForWriting(),
                        result.instanceProgramIndex.data(), bytes);
            result.instanceProgramIndexBuffer->flush();
            std::cout << "Created instanceProgramIndex buffer for "
                      << result.instanceProgramIndex.size() << " instances across "
                      << result.materialPrograms.headers().size() << " program(s)"
                      << std::endl;
        }

        // Step 5b: Per-instance UV start offset SSBO. Parallel to
        // instanceProgramIndex — same length, same indexing scheme. Shader
        // does `uvBuffer.uvs[instanceUvOffset[instanceIndex] + triIdx*3 + i]`.
        if (!result.instanceUvOffset.empty())
        {
            const size_t bytes = result.instanceUvOffset.size() * sizeof(uint32_t);
            result.instanceUvOffsetBuffer = std::unique_ptr<Buffer>(
                device->createBuffer(static_cast<uint32_t>(bytes), BufferUsage::StorageBuffer));
            std::memcpy(result.instanceUvOffsetBuffer->mapForWriting(),
                        result.instanceUvOffset.data(), bytes);
            result.instanceUvOffsetBuffer->flush();
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
}
