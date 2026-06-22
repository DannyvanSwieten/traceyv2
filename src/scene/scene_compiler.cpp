#include "scene_compiler.hpp"
#include "blas_cache.hpp"
#include "material_instance.hpp"
#include "../graph/graphs/shader_graph/compiler.hpp"
#include "../graph/graphs/shader_graph/nodes.hpp"
#include "../graph/graphs/shader_graph/serialization.hpp"
#include "../graph/graphs/shader_graph/shader_graph.hpp"
#include "../shading/material_program/opcodes.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unordered_map>

// stb_image header (implementation is in gltf_loader.cpp)
#include <stb_image.h>

namespace tracey
{
    namespace
    {
        // Best-effort viewport-preview color extraction from a compiled
        // shader graph. The rasterizer can't run the full bytecode VM that
        // the path tracer uses, so it needs a single representative
        // baseColor per actor — the equivalent of Houdini's "display
        // color" hint. We look for the Albedo input on the graph's
        // MaterialOutput terminal and try to resolve it: a literal
        // ConstantNode upstream, or an `input_defaults[Albedo]` set on
        // the output via the inspector. Anything more complex (an
        // attribute lookup, a math chain, a noise texture) returns no
        // preview — the rasterizer keeps using the SceneObject
        // material's albedo (or default white).
        std::optional<Vec3> extractGraphPreviewAlbedo(const ShaderGraph &graph)
        {
            // Port index of "Albedo" on MaterialOutput — must match
            // materialOutputPorts()[0] in nodes.hpp.
            constexpr size_t kAlbedoPort = 0;

            const MaterialOutputNode *out = nullptr;
            size_t outUid = 0;
            for (const auto &nodePtr : graph.nodes())
            {
                const auto *node = dynamic_cast<const ShaderGraphNode *>(nodePtr.get());
                if (!node) continue;
                if (node->kind() != ShaderNodeKind::MaterialOutput) continue;
                out = static_cast<const MaterialOutputNode *>(node);
                outUid = out->uid();
                break;
            }
            if (!out) return std::nullopt;

            // Wired upstream?
            for (const auto &c : graph.connections())
            {
                if (c.toNode != outUid || c.toPort != kAlbedoPort) continue;
                const ShaderGraphNode *src = nullptr;
                for (const auto &nodePtr : graph.nodes())
                {
                    const auto *n = dynamic_cast<const ShaderGraphNode *>(nodePtr.get());
                    if (n && n->uid() == c.fromNode) { src = n; break; }
                }
                if (!src) continue;
                if (src->kind() == ShaderNodeKind::Constant)
                {
                    const Vec4 &v = static_cast<const ConstantNode *>(src)->value();
                    return Vec3(v.x, v.y, v.z);
                }
                // Anything else (Parameter, math op, attribute lookup):
                // can't statically resolve, bail.
                return std::nullopt;
            }

            // No upstream wire — fall back to the per-port inspector
            // default (the "drag out MaterialOutput and set Albedo" case).
            const auto def = out->inputDefault(kAlbedoPort);
            if (def.has_value()) return Vec3(def->x, def->y, def->z);
            return std::nullopt;
        }
    }  // anon

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

        // Retain the decoded pixels for path tracer backends that own their
        // textures (Metal / CPU) — see CompiledScene::textureSources. Copied
        // before the frees below; index-parallel with result.textures.
        CompiledScene::TextureSource source;
        source.width = static_cast<uint32_t>(width);
        source.height = static_cast<uint32_t>(height);
        source.srgb = isColorData;
        source.rgba8.assign(data, data + dataSize);

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
        result.textureSources.push_back(std::move(source));
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

        // Transparency / emission scalars. Stored as plain float properties
        // (set by glTF import or the material UI); absent → struct defaults
        // (transmission 0, ior 1.5, opacity 1, emissionStrength 1).
        if (auto t = material.getFloat("transmission")) gpuMat.transmissionFactor = *t;
        if (auto i = material.getFloat("ior")) gpuMat.iorFactor = *i;
        if (auto o = material.getFloat("opacity")) gpuMat.baseColorA = *o;
        if (auto es = material.getFloat("emissionStrength")) gpuMat.emissiveStrength = *es;

        // R3 advanced-BSDF lobe factors (0 = off → unchanged look).
        if (auto v = material.getFloat("clearcoat")) gpuMat.clearcoatFactor = *v;
        if (auto v = material.getFloat("clearcoatRoughness")) gpuMat.clearcoatRoughnessFactor = *v;
        if (auto v = material.getFloat("sheen")) gpuMat.sheenFactor = *v;
        if (auto v = material.getFloat("anisotropy")) gpuMat.anisotropyFactor = *v;
        if (auto v = material.getFloat("subsurface")) gpuMat.subsurfaceFactor = *v;
        if (auto c = material.getVec3("subsurfaceColor"))
        {
            gpuMat.subsurfaceColorR = c->x;
            gpuMat.subsurfaceColorG = c->y;
            gpuMat.subsurfaceColorB = c->z;
        }

        return gpuMat;
    }
    SceneCompiler::ObjectData SceneCompiler::compileObject(Device *device, const SceneObject &obj,
                                                            const BVHConfig &bvhConfig,
                                                            bool buildAccelerationStructures)
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

        // BLAS construction is the most expensive piece of per-object
        // compile. When the caller isn't going to ray-trace this scene
        // (live viewport with the PT inset preview disabled) we skip the
        // BVH build and leave data.blas / data.nodeCount at their
        // defaults — vertex / color / uv / normal data above is still
        // populated because the rasterizer needs all of it.
        if (buildAccelerationStructures)
        {
            data.blas = std::unique_ptr<BottomLevelAccelerationStructure>(
                device->createBottomLevelAccelerationStructure(
                    data.vertexBuffer.get(),
                    static_cast<uint32_t>(data.vertexCount),
                    sizeof(Vec3),
                    nullptr, 0,
                    bvhConfig));

            data.nodeCount = data.blas->nodeCount();
        }

        return data;
    }

    Mat4 SceneCompiler::computeWorldTransform(const Scene & /*scene*/, const Actor &actor)
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
        return compile(device, scene, bvhConfig, cache, /*buildAccelerationStructures=*/true);
    }

    uint64_t SceneCompiler::nextSceneRevision()
    {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }

    SceneCompiler::CompiledScene SceneCompiler::compile(Device *device, const Scene &scene,
                                                        const BVHConfig &bvhConfig, BlasCache *cache,
                                                        bool buildAccelerationStructures)
    {
        CompiledScene result;
        result.revision = nextSceneRevision();

        // Per-compile config + stats logs fired once per compile_scene()
        // call; with particle sims cooking 60×/sec the stderr flood
        // drowns out anything actually worth surfacing. Re-enable for a
        // single run with TRACEY_VERBOSE_COMPILE=1 when triaging BLAS /
        // material-buffer issues.
        static const bool verboseCompile =
            std::getenv("TRACEY_VERBOSE_COMPILE") != nullptr;
        if (verboseCompile)
        {
            std::cout << "BVH Configuration: leafThreshold=" << bvhConfig.leafThreshold
                      << ", traversalCost=" << bvhConfig.traversalCost
                      << ", intersectionCost=" << bvhConfig.intersectionCost << std::endl;
        }

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

        // Positions concatenated parallel to UVs/normals (Vec3 → Vec4 for the
        // 16-byte std430 stride). Consumed by the CPU path tracer to rebuild a
        // hit triangle's object-space vertices for UV-aligned tangents.
        std::vector<Vec4> allPositions;

        for (const auto &[name, objPtr] : objects)
        {
            if (objPtr->vertexCount() == 0)
            {
                continue;
            }

            const uint64_t hash = objPtr->contentHash();
            BlasCache::Entry *entry = nullptr;
            // The BlasCache contract is "cache hit ⇒ entry->blas non-null".
            // When skipping AS we'd be inserting null-blas entries that a
            // later PT-on recompile would happily reuse without building
            // BVHs — so bypass the cache entirely in raster-only mode.
            // Vertex buffers get re-uploaded each cook in that mode, which
            // is cheap relative to the BVH cost we just elided.
            if (cache && buildAccelerationStructures)
            {
                entry = cache->lookup(name, hash);
                // Cache contentHash only covers positions + indices —
                // a Cd / N / UV edit (e.g. an attribute_vop writing
                // geo_output.Cd) doesn't change topology and shouldn't
                // invalidate the BLAS. Refresh the entry's shading
                // data in place so the rasterizer / hit shader picks
                // up the new values without paying for a BVH rebuild.
                if (entry)
                {
                    // colorBuffer (per-vertex Cd, always allocated; default
                    // white when the SceneObject has no colors).
                    const size_t vCount = entry->vertexCount;
                    if (entry->colorBuffer && vCount > 0)
                    {
                        auto *colorMapped = static_cast<Vec3 *>(
                            entry->colorBuffer->mapForWriting());
                        if (objPtr->hasColors())
                        {
                            const auto &colors = objPtr->colors();
                            const size_t n = std::min(colors.size(), vCount);
                            std::copy(colors.begin(), colors.begin() + n, colorMapped);
                            for (size_t i = n; i < vCount; ++i)
                                colorMapped[i] = Vec3(1.0f);
                        }
                        else
                        {
                            for (size_t i = 0; i < vCount; ++i)
                                colorMapped[i] = Vec3(1.0f);
                        }
                        entry->colorBuffer->flush();
                    }
                    // uvs / normals live as CPU vectors on the entry
                    // and get concatenated into the global uv /
                    // normal buffers below — replace them with fresh
                    // copies from the SceneObject so VOP-written
                    // values flow through.
                    if (objPtr->hasUvs())
                    {
                        entry->uvs = objPtr->uvs();
                        if (entry->uvs.size() < vCount)
                            entry->uvs.resize(vCount, Vec2(0.0f));
                    }
                    entry->hasUvs = objPtr->hasUvs();
                    if (objPtr->hasNormals())
                    {
                        entry->normals = objPtr->normals();
                        if (entry->normals.size() < vCount)
                            entry->normals.resize(vCount, Vec3(0.0f));
                    }
                    else
                    {
                        // Object lost its normals between cooks; zero
                        // out so the hit shader's "all-zero ⇒ face
                        // normal" fallback still kicks in cleanly.
                        std::fill(entry->normals.begin(), entry->normals.end(), Vec3(0.0f));
                    }
                    entry->hasNormals = objPtr->hasNormals();
                }
            }

            if (!entry)
            {
                ObjectData objData =
                    compileObject(device, *objPtr, bvhConfig,
                                  buildAccelerationStructures);
                // Empty SceneObjects produce vertexCount==0 + no blas; non-
                // empty objects with buildAS=false also have no blas but
                // are valid. Use vertexCount as the validity check.
                if (objData.vertexCount == 0) continue;

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
                if (cache && buildAccelerationStructures)
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

            // Positions concatenated parallel to UVs/normals. Read from the
            // BLAS cache's packed-Vec3 vertex buffer; pad with zeros if absent
            // so the per-instance base offset stays valid for every backend.
            if (entry->vertexBuffer && entry->vertexCount > 0)
            {
                const Vec3 *src =
                    static_cast<const Vec3 *>(entry->vertexBuffer->mapForReading());
                allPositions.reserve(allPositions.size() + entry->vertexCount);
                for (size_t k = 0; k < entry->vertexCount; ++k)
                    allPositions.emplace_back(src[k].x, src[k].y, src[k].z, 0.0f);
                entry->vertexBuffer->unmap();
            }
            else
            {
                allPositions.insert(allPositions.end(), entry->vertexCount, Vec4(0.0f));
            }
        }

        // In raster-only mode result.blases is empty by design (entries are
        // pushed as nullptr to keep the parallel-index contract with
        // vertexBuffers / colorBuffers, but we don't bother growing the
        // vector). Use vertexBuffers as the "has geometry" check instead.
        if (result.vertexBuffers.empty())
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
            if (verboseCompile) std::cout << "Created UV buffer with " << allUVs.size() << " entries" << std::endl;
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

        // Position buffer parallel to UVs/normals — feeds the CPU backend's
        // UV-tangent derivation. Created whenever any geometry exists.
        if (!allPositions.empty())
        {
            const size_t bytes = allPositions.size() * sizeof(Vec4);
            result.positionBuffer = std::unique_ptr<Buffer>(
                device->createBuffer(static_cast<uint32_t>(bytes), BufferUsage::StorageBuffer));
            auto *mapped = static_cast<Vec4 *>(result.positionBuffer->mapForWriting());
            std::copy(allPositions.begin(), allPositions.end(), mapped);
            result.positionBuffer->flush();
        }

        // Step 2: Flatten scene and create instances with materials
        auto sceneNodes = scene.flatten();
        uint32_t materialIndex = 0;

        // Material program aggregation: program 0 is always the passthrough
        // (used by any actor without an attached graph). Subsequent programs
        // are added on first encounter of a unique graph JSON. The lookup
        // table keyed by JSON string lets us dedupe across actors.
        result.materialPrograms.addProgram(makePassthroughProgram());
        // Each entry pairs the programId (consumed by the path tracer)
        // with an optional viewport preview color extracted from the
        // graph's WriteAlbedo output. The rasterizer can't run the full
        // material program, so the preview color is its fallback when
        // the SceneObject's own material doesn't carry an albedo. See
        // extractGraphPreviewAlbedo above for the resolution rules.
        struct ActorMaterialEntry {
            uint32_t programId = 0;
            std::optional<Vec3> previewAlbedo;
        };
        std::unordered_map<std::string, ActorMaterialEntry> graphJsonToEntry;

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
            std::optional<Vec3> actorPreviewAlbedo;
            const std::string &graphJson = actor->materialGraphJson();
            if (!graphJson.empty())
            {
                auto cached = graphJsonToEntry.find(graphJson);
                if (cached != graphJsonToEntry.end())
                {
                    actorProgramId = cached->second.programId;
                    actorPreviewAlbedo = cached->second.previewAlbedo;
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
                            actorPreviewAlbedo = extractGraphPreviewAlbedo(*graph);
                            graphJsonToEntry.emplace(graphJson,
                                ActorMaterialEntry{actorProgramId, actorPreviewAlbedo});
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
                // Viewport preview color: when an actor's material graph
                // statically resolves to a constant baseColor, that
                // wins over whatever the SceneObject material carried.
                // The path tracer already treats the graph as
                // authoritative when present (it runs the program
                // bytecode at hit time), so the rasterizer doing the
                // same just brings the preview in line with the final
                // render. The graph-assignment handler upstream leaves
                // a default 0.8-gray albedo on the SceneObject when a
                // graph is attached — that's a fallback for the
                // no-graph case, not a value the rasterizer should
                // prefer over the user's actual material choice.
                if (actorPreviewAlbedo)
                {
                    gpuMat.baseColorR = actorPreviewAlbedo->x;
                    gpuMat.baseColorG = actorPreviewAlbedo->y;
                    gpuMat.baseColorB = actorPreviewAlbedo->z;
                }
                result.materials.push_back(gpuMat);

                // Collect emissive triangles (world-space) for path-tracer NEE.
                // Only when this instance's material actually emits; the common
                // non-emissive case costs one cheap add + compare here.
                const Vec3 emission(gpuMat.emissiveR, gpuMat.emissiveG, gpuMat.emissiveB);
                const float emiss = (emission.x + emission.y + emission.z) * gpuMat.emissiveStrength;
                constexpr size_t kMaxEmitterTris = 4096;
                if (emiss > 1e-4f && result.emitters.size() < kMaxEmitterTris)
                {
                    if (const SceneObject *obj = scene.getObject(objectRef))
                    {
                        const auto &P = obj->positions();
                        const auto &idx = obj->indices();
                        const Vec3 emRGB = emission * gpuMat.emissiveStrength;
                        const size_t triCount = idx.empty() ? P.size() / 3 : idx.size() / 3;
                        for (size_t t = 0; t < triCount &&
                                            result.emitters.size() < kMaxEmitterTris; ++t)
                        {
                            const uint32_t i0 = idx.empty() ? uint32_t(t * 3 + 0) : idx[t * 3 + 0];
                            const uint32_t i1 = idx.empty() ? uint32_t(t * 3 + 1) : idx[t * 3 + 1];
                            const uint32_t i2 = idx.empty() ? uint32_t(t * 3 + 2) : idx[t * 3 + 2];
                            if (i0 >= P.size() || i1 >= P.size() || i2 >= P.size()) continue;
                            CompiledScene::EmissiveTri e;
                            e.p0 = transformPoint(finalTransform, P[i0]);
                            e.p1 = transformPoint(finalTransform, P[i1]);
                            e.p2 = transformPoint(finalTransform, P[i2]);
                            e.area = 0.5f * glm::length(glm::cross(e.p1 - e.p0, e.p2 - e.p0));
                            if (e.area <= 1e-9f) continue; // skip degenerate
                            e.emission = emRGB;
                            result.emitters.push_back(e);
                        }
                    }
                }

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

            // Slot 2 packs the colour multiplier alongside one of the
            // per-type scalars in .w: Point→radius, Area→sizeX, otherwise
            // zero. Reading code branches on positionAndType.w (LightType).
            gpu.colorAndExtraX[0] = l->color.x;
            gpu.colorAndExtraX[1] = l->color.y;
            gpu.colorAndExtraX[2] = l->color.z;
            gpu.colorAndExtraX[3] =
                (l->type == LightType::Point) ? l->radius :
                (l->type == LightType::Area)  ? l->size.x : 0.0f;

            // Dome gradient (skyColor / horizonColor / groundColor). For
            // non-Dome lights these slots stay zero and the shader's Dome
            // pass skips them via positionAndType.w; the cost is one branch
            // per fragment, dwarfed by the BRDF.
            gpu.skyColorAndExtraY[0] = l->skyColor.x;
            gpu.skyColorAndExtraY[1] = l->skyColor.y;
            gpu.skyColorAndExtraY[2] = l->skyColor.z;
            gpu.skyColorAndExtraY[3] =
                (l->type == LightType::Area) ? l->size.y : 0.0f;

            gpu.horizonColorAndFlags[0] = l->horizonColor.x;
            gpu.horizonColorAndFlags[1] = l->horizonColor.y;
            gpu.horizonColorAndFlags[2] = l->horizonColor.z;
            gpu.horizonColorAndFlags[3] = 0.0f;  // reserved flag bits

            gpu.groundColorAndPad[0] = l->groundColor.x;
            gpu.groundColorAndPad[1] = l->groundColor.y;
            gpu.groundColorAndPad[2] = l->groundColor.z;
            gpu.groundColorAndPad[3] = 0.0f;

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

        // Step 3: Create TLAS — gated on PT mode. blasPtrs contains nullptrs
        // when buildAccelerationStructures was false, so we'd build a TLAS
        // over invalid handles otherwise. The path tracer is the only
        // consumer of result.tlas; the rasterizer drives instancing from
        // result.instances directly.
        if (buildAccelerationStructures)
        {
            result.tlas = std::unique_ptr<TopLevelAccelerationStructure>(
                device->createTopLevelAccelerationStructure(
                    std::span<const BottomLevelAccelerationStructure *>(blasPtrs),
                    std::span<const Tlas::Instance>(result.instances)));
        }

        // Step 4: Create material buffer
        if (!result.materials.empty())
        {
            size_t materialBufferSize = result.materials.size() * sizeof(GPUMaterial);
            result.materialBuffer = std::unique_ptr<Buffer>(
                device->createBuffer(static_cast<uint32_t>(materialBufferSize), BufferUsage::StorageBuffer));

            auto *mapped = static_cast<GPUMaterial *>(result.materialBuffer->mapForWriting());
            std::copy(result.materials.begin(), result.materials.end(), mapped);
            result.materialBuffer->flush();

            if (verboseCompile)
            {
                std::cout << "Created material buffer with " << result.materials.size() << " materials" << std::endl;
                std::cout << "Loaded " << result.textures.size() << " textures" << std::endl;
            }
        }

        // Step 5: Pack the two per-instance lookup tables (programId,
        // uvOffset) into a single uvec2[] SSBO. We used to upload them as
        // two separate buffers (instanceProgramIndexBuffer /
        // instanceUvOffsetBuffer) but the wavefront compute pipeline was
        // hitting maxPerStageDescriptorStorageBuffers=31 on MoltenVK, so
        // merging is the cheapest way to claw back a binding without
        // restructuring the rest of the layout. Shader: `instanceData.data[i].x`
        // is programId, `.y` is uvOffset.
        if (!result.instanceProgramIndex.empty() || !result.instanceUvOffset.empty())
        {
            const size_t count = std::max(result.instanceProgramIndex.size(),
                                          result.instanceUvOffset.size());
            struct Pair { uint32_t programId; uint32_t uvOffset; };
            std::vector<Pair> packed(count, Pair{0u, 0u});
            for (size_t i = 0; i < result.instanceProgramIndex.size(); ++i)
                packed[i].programId = result.instanceProgramIndex[i];
            for (size_t i = 0; i < result.instanceUvOffset.size(); ++i)
                packed[i].uvOffset = result.instanceUvOffset[i];

            const size_t bytes = packed.size() * sizeof(Pair);
            result.instanceDataBuffer = std::unique_ptr<Buffer>(
                device->createBuffer(static_cast<uint32_t>(bytes), BufferUsage::StorageBuffer));
            std::memcpy(result.instanceDataBuffer->mapForWriting(),
                        packed.data(), bytes);
            result.instanceDataBuffer->flush();
            if (verboseCompile)
            {
                std::cout << "Created instanceData buffer for "
                          << count << " instances across "
                          << result.materialPrograms.headers().size() << " program(s)"
                          << std::endl;
            }
        }

        if (verboseCompile)
        {
            std::cout << "BVH Statistics: " << result.totalNodes << " nodes, "
                      << result.totalTriangles << " triangles" << std::endl;
            if (result.totalTriangles > 0)
            {
                float nodesPerTri = static_cast<float>(result.totalNodes) / static_cast<float>(result.totalTriangles);
                std::cout << "  Nodes per triangle: " << nodesPerTri << std::endl;
            }
        }

        // R4 motion blur: default the shutter-close poses to the shutter-open
        // ones (no motion). The sequence renderer overwrites instancesEnd +
        // sets hasMotion when it recompiles at t + shutter.
        result.instancesEnd = result.instances;

        return result;
    }
}
