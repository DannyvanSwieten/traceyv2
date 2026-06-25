#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "gltf_loader.hpp"
#include "camera.hpp"
#include "skeleton.hpp"
#include "scene_object.hpp"
// tinygltf pulls in stb_image_write here; its aggregate initialisers trip
// -Wmissing-field-initializers. Silence that vendored-header noise locally.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include <tiny_gltf.h>
#pragma clang diagnostic pop
#include <glm/glm.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <iostream>
#include <unordered_map>
#include <cmath>
#include <memory>
#include <mutex>

namespace tracey
{
    namespace
    {
        // Helper to extract Vec3 from GLTF accessor
        std::vector<Vec3> extractVec3Accessor(const tinygltf::Model &model, int accessorIndex)
        {
            std::vector<Vec3> result;
            if (accessorIndex < 0)
                return result;

            const auto &accessor = model.accessors[accessorIndex];

            // Validate accessor type
            if (accessor.type != TINYGLTF_TYPE_VEC3)
            {
                std::cerr << "Warning: Expected VEC3 accessor, got type " << accessor.type << std::endl;
                return result;
            }

            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
            {
                std::cerr << "Warning: Expected FLOAT component type, got " << accessor.componentType << std::endl;
                return result;
            }

            // Handle sparse accessors or missing buffer view
            if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
            {
                std::cerr << "Warning: Invalid bufferView index " << accessor.bufferView << std::endl;
                return result;
            }

            const auto &bufferView = model.bufferViews[accessor.bufferView];
            const auto &buffer = model.buffers[bufferView.buffer];

            const size_t byteStride = bufferView.byteStride ? bufferView.byteStride : sizeof(float) * 3;
            const unsigned char *data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;

            result.reserve(accessor.count);
            for (size_t i = 0; i < accessor.count; ++i)
            {
                const float *ptr = reinterpret_cast<const float *>(data + i * byteStride);
                result.emplace_back(ptr[0], ptr[1], ptr[2]);
            }

            return result;
        }

        // Helper to extract Vec2 from GLTF accessor
        std::vector<Vec2> extractVec2Accessor(const tinygltf::Model &model, int accessorIndex)
        {
            std::vector<Vec2> result;
            if (accessorIndex < 0)
                return result;

            const auto &accessor = model.accessors[accessorIndex];
            const auto &bufferView = model.bufferViews[accessor.bufferView];
            const auto &buffer = model.buffers[bufferView.buffer];

            const size_t byteStride = bufferView.byteStride ? bufferView.byteStride : sizeof(float) * 2;
            const unsigned char *data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;

            result.reserve(accessor.count);
            for (size_t i = 0; i < accessor.count; ++i)
            {
                const float *ptr = reinterpret_cast<const float *>(data + i * byteStride);
                result.emplace_back(ptr[0], ptr[1]);
            }
            return result;
        }

        // Helper to extract indices from GLTF accessor
        std::vector<uint32_t> extractIndicesAccessor(const tinygltf::Model &model, int accessorIndex)
        {
            std::vector<uint32_t> result;
            if (accessorIndex < 0)
                return result;

            const auto &accessor = model.accessors[accessorIndex];
            const auto &bufferView = model.bufferViews[accessor.bufferView];
            const auto &buffer = model.buffers[bufferView.buffer];

            const unsigned char *data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;

            result.reserve(accessor.count);
            for (size_t i = 0; i < accessor.count; ++i)
            {
                uint32_t index = 0;
                switch (accessor.componentType)
                {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    index = data[i];
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    index = reinterpret_cast<const uint16_t *>(data)[i];
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    index = reinterpret_cast<const uint32_t *>(data)[i];
                    break;
                default:
                    break;
                }
                result.push_back(index);
            }
            return result;
        }

        // ── Skinning / animation accessor helpers ───────────────────────────

        // FLOAT VEC4 accessor — WEIGHTS_0 and animation rotation output.
        std::vector<Vec4> extractVec4Accessor(const tinygltf::Model &model, int accessorIndex)
        {
            std::vector<Vec4> result;
            if (accessorIndex < 0)
                return result;
            const auto &accessor = model.accessors[accessorIndex];
            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || accessor.bufferView < 0)
                return result; // v1: float weights/rotations only
            const auto &bufferView = model.bufferViews[accessor.bufferView];
            const auto &buffer = model.buffers[bufferView.buffer];
            const size_t byteStride = bufferView.byteStride ? bufferView.byteStride : sizeof(float) * 4;
            const unsigned char *data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
            result.reserve(accessor.count);
            for (size_t i = 0; i < accessor.count; ++i)
            {
                const float *p = reinterpret_cast<const float *>(data + i * byteStride);
                result.emplace_back(p[0], p[1], p[2], p[3]);
            }
            return result;
        }

        // JOINTS_0 (VEC4 of unsigned byte/short/int) → Vec4 of float-packed ids.
        std::vector<Vec4> extractJointsVec4(const tinygltf::Model &model, int accessorIndex)
        {
            std::vector<Vec4> result;
            if (accessorIndex < 0)
                return result;
            const auto &accessor = model.accessors[accessorIndex];
            if (accessor.bufferView < 0)
                return result;
            const auto &bufferView = model.bufferViews[accessor.bufferView];
            const auto &buffer = model.buffers[bufferView.buffer];
            const size_t compSize =
                (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)  ? 1
                : (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) ? 2
                                                                                     : 4;
            const size_t byteStride = bufferView.byteStride ? bufferView.byteStride : compSize * 4;
            const unsigned char *data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
            result.reserve(accessor.count);
            for (size_t i = 0; i < accessor.count; ++i)
            {
                const unsigned char *p = data + i * byteStride;
                Vec4 j(0.0f);
                for (int c = 0; c < 4; ++c)
                {
                    uint32_t idx = 0;
                    switch (accessor.componentType)
                    {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                        idx = p[c];
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                        idx = reinterpret_cast<const uint16_t *>(p)[c];
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                        idx = reinterpret_cast<const uint32_t *>(p)[c];
                        break;
                    default:
                        break;
                    }
                    j[c] = static_cast<float>(idx);
                }
                result.push_back(j);
            }
            return result;
        }

        // FLOAT MAT4 accessor — inverseBindMatrices (column-major, like glm).
        std::vector<Mat4> extractMat4Accessor(const tinygltf::Model &model, int accessorIndex)
        {
            std::vector<Mat4> result;
            if (accessorIndex < 0)
                return result;
            const auto &accessor = model.accessors[accessorIndex];
            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || accessor.bufferView < 0)
                return result;
            const auto &bufferView = model.bufferViews[accessor.bufferView];
            const auto &buffer = model.buffers[bufferView.buffer];
            const size_t byteStride = bufferView.byteStride ? bufferView.byteStride : sizeof(float) * 16;
            const unsigned char *data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
            result.reserve(accessor.count);
            for (size_t i = 0; i < accessor.count; ++i)
            {
                const float *p = reinterpret_cast<const float *>(data + i * byteStride);
                Mat4 m(1.0f);
                for (int col = 0; col < 4; ++col)
                    for (int row = 0; row < 4; ++row)
                        m[col][row] = p[col * 4 + row];
                result.push_back(m);
            }
            return result;
        }

        // FLOAT SCALAR accessor — animation sampler input (keyframe times).
        std::vector<float> extractScalarFloat(const tinygltf::Model &model, int accessorIndex)
        {
            std::vector<float> result;
            if (accessorIndex < 0)
                return result;
            const auto &accessor = model.accessors[accessorIndex];
            if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || accessor.bufferView < 0)
                return result;
            const auto &bufferView = model.bufferViews[accessor.bufferView];
            const auto &buffer = model.buffers[bufferView.buffer];
            const size_t byteStride = bufferView.byteStride ? bufferView.byteStride : sizeof(float);
            const unsigned char *data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
            result.reserve(accessor.count);
            for (size_t i = 0; i < accessor.count; ++i)
                result.push_back(*reinterpret_cast<const float *>(data + i * byteStride));
            return result;
        }

        // A node's bind-pose local transform as TRS (glTF stores either a
        // matrix or T/R/S components; rotation quat is xyzw → glm wxyz).
        void nodeLocalTRS(const tinygltf::Node &node, Vec3 &t, glm::quat &r, Vec3 &s)
        {
            t = Vec3(0.0f);
            r = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            s = Vec3(1.0f);
            if (node.matrix.size() == 16)
            {
                Mat4 m(1.0f);
                for (int col = 0; col < 4; ++col)
                    for (int row = 0; row < 4; ++row)
                        m[col][row] = static_cast<float>(node.matrix[col * 4 + row]);
                Vec3 skew;
                Vec4 persp;
                glm::quat q;
                glm::decompose(m, s, q, t, skew, persp);
                r = q;
                return;
            }
            if (node.translation.size() == 3)
                t = Vec3(static_cast<float>(node.translation[0]),
                         static_cast<float>(node.translation[1]),
                         static_cast<float>(node.translation[2]));
            if (node.scale.size() == 3)
                s = Vec3(static_cast<float>(node.scale[0]),
                         static_cast<float>(node.scale[1]),
                         static_cast<float>(node.scale[2]));
            if (node.rotation.size() == 4)
                r = glm::quat(static_cast<float>(node.rotation[3]),  // w
                              static_cast<float>(node.rotation[0]),  // x
                              static_cast<float>(node.rotation[1]),  // y
                              static_cast<float>(node.rotation[2])); // z
        }

        // Build one Skeleton per glTF skin. All skeletons share the full node
        // hierarchy (bind-pose local TRS + parents) and the converted animation
        // clips; each carries its own joint list + inverse-bind matrices.
        std::unordered_map<int, std::shared_ptr<const Skeleton>>
        buildSkeletons(const tinygltf::Model &model)
        {
            std::unordered_map<int, std::shared_ptr<const Skeleton>> out;
            if (model.skins.empty())
                return out;

            const size_t nodeCount = model.nodes.size();
            std::vector<SkelNode> nodes(nodeCount);
            for (size_t i = 0; i < nodeCount; ++i)
                nodeLocalTRS(model.nodes[i], nodes[i].t, nodes[i].r, nodes[i].s);
            for (size_t i = 0; i < nodeCount; ++i)
                for (int child : model.nodes[i].children)
                    if (child >= 0 && static_cast<size_t>(child) < nodeCount)
                        nodes[child].parent = static_cast<int>(i);

            // Convert animations once (channels target node indices, shared
            // across skins). CUBICSPLINE samplers carry 3× values per key;
            // the size mismatch below drops them (v1 supports LINEAR/STEP).
            std::vector<AnimationClip> clips;
            for (const auto &anim : model.animations)
            {
                AnimationClip clip;
                clip.name = anim.name;
                for (const auto &chan : anim.channels)
                {
                    if (chan.sampler < 0 || chan.sampler >= static_cast<int>(anim.samplers.size()))
                        continue;
                    if (chan.target_node < 0)
                        continue;
                    AnimChannel ac;
                    ac.node = chan.target_node;
                    if (chan.target_path == "translation")
                        ac.path = AnimChannel::Path::Translation;
                    else if (chan.target_path == "rotation")
                        ac.path = AnimChannel::Path::Rotation;
                    else if (chan.target_path == "scale")
                        ac.path = AnimChannel::Path::Scale;
                    else
                        continue; // skip "weights" (morph) for v1
                    const auto &samp = anim.samplers[chan.sampler];
                    ac.times = extractScalarFloat(model, samp.input);
                    if (ac.path == AnimChannel::Path::Rotation)
                    {
                        ac.values = extractVec4Accessor(model, samp.output);
                    }
                    else
                    {
                        const auto v3 = extractVec3Accessor(model, samp.output);
                        ac.values.reserve(v3.size());
                        for (const auto &v : v3)
                            ac.values.emplace_back(v.x, v.y, v.z, 0.0f);
                    }
                    ac.step = (samp.interpolation == "STEP");
                    if (ac.times.empty() || ac.times.size() != ac.values.size())
                        continue;
                    clip.duration = std::max(clip.duration, ac.times.back());
                    clip.channels.push_back(std::move(ac));
                }
                if (!clip.channels.empty())
                    clips.push_back(std::move(clip));
            }

            for (size_t si = 0; si < model.skins.size(); ++si)
            {
                const auto &skin = model.skins[si];
                auto skel = std::make_shared<Skeleton>();
                skel->nodes = nodes;
                skel->clips = clips;
                skel->joints.assign(skin.joints.begin(), skin.joints.end());
                skel->inverseBind = extractMat4Accessor(model, skin.inverseBindMatrices);
                // Inverse-bind matrices are optional in glTF (omitted → identity).
                if (skel->inverseBind.size() < skel->joints.size())
                    skel->inverseBind.resize(skel->joints.size(), Mat4(1.0f));
                out[static_cast<int>(si)] = std::move(skel);
            }
            return out;
        }

        // Helper to get texture URI from GLTF texture index
        std::string getTextureUri(const tinygltf::Model &model, int textureIndex, const std::string &baseDir)
        {
            if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
                return "";

            const auto &texture = model.textures[textureIndex];
            if (texture.source < 0 || texture.source >= static_cast<int>(model.images.size()))
                return "";

            const auto &image = model.images[texture.source];

            // If tinygltf decoded the image (embedded via bufferView, data URI, or loaded from file),
            // use embedded identifier to match extractEmbeddedTextures
            if (!image.image.empty())
            {
                return "embedded:" + std::to_string(texture.source);
            }

            // External file that wasn't loaded by tinygltf - return file path
            if (!image.uri.empty())
            {
                return baseDir + "/" + image.uri;
            }

            return "";
        }

        // Pick a SamplerKind from a glTF Texture's sampler reference. glTF's
        // sampler matrix is bigger than ours (3 wrap modes × 6 minFilters ×
        // 2 magFilters), so we collapse:
        //   - MIRRORED_REPEAT folds into Repeat (no shader-side mirror yet).
        //   - Mipmap variants of minFilter are ignored — we don't generate
        //     mips, so only the base filter classification matters.
        //   - If the texture omits `sampler` (-1), the glTF default is
        //     LINEAR/REPEAT, which maps to LinearRepeat.
        SamplerKind samplerKindForTexture(const tinygltf::Model &model, int textureIndex)
        {
            if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
                return SamplerKind::LinearRepeat;
            const int samplerIdx = model.textures[textureIndex].sampler;
            if (samplerIdx < 0 || samplerIdx >= static_cast<int>(model.samplers.size()))
                return SamplerKind::LinearRepeat;
            const auto &s = model.samplers[samplerIdx];

            // Treat unset filters as LINEAR (the spec-compatible "nice" default).
            const int mag = s.magFilter;
            const int minF = s.minFilter;
            const bool nearestMag = (mag == TINYGLTF_TEXTURE_FILTER_NEAREST);
            const bool nearestMin = (minF == TINYGLTF_TEXTURE_FILTER_NEAREST ||
                                     minF == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST ||
                                     minF == TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR);
            const bool isNearest = nearestMag && nearestMin;

            // wrap: if EITHER axis says clamp, the asset clearly wants clamp
            // (a partial clamp like clamp-S/repeat-T is rare; this errs on
            // the side of matching the artist's intent for the constrained axis).
            const bool isClamp = (s.wrapS == TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE) ||
                                 (s.wrapT == TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE);

            if (isNearest && isClamp)  return SamplerKind::NearestClamp;
            if (isNearest)             return SamplerKind::NearestRepeat;
            if (isClamp)               return SamplerKind::LinearClamp;
            return SamplerKind::LinearRepeat;
        }

        // Extract embedded texture data from tinygltf image
        void extractEmbeddedTextures(const tinygltf::Model &model, Scene &scene)
        {
            for (size_t i = 0; i < model.images.size(); ++i)
            {
                const auto &image = model.images[i];

                // Check if this is an embedded image (loaded by tinygltf)
                // tinygltf already decodes embedded images into the image.image vector
                if (!image.image.empty())
                {
                    std::string embeddedId = "embedded:" + std::to_string(i);

                    EmbeddedTexture tex;
                    tex.data = image.image; // Copy decoded pixel data
                    tex.width = image.width;
                    tex.height = image.height;
                    tex.channels = image.component;
                    tex.mimeType = image.mimeType;

                    scene.addEmbeddedTexture(embeddedId, std::move(tex));

                    std::cout << "Extracted embedded texture " << embeddedId
                              << " (" << image.width << "x" << image.height
                              << ", " << image.component << " channels)" << std::endl;
                }
            }
        }

        // Convert GLTF material to our MaterialInstance
        MaterialInstance convertMaterial(const tinygltf::Model &model, int materialIndex, const std::string &baseDir)
        {
            MaterialInstance material("pbr");

            if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size()))
            {
                // Default material
                material.setAlbedo(Vec3(0.8f, 0.8f, 0.8f));
                material.setMetallic(0.0f);
                material.setRoughness(0.5f);
                return material;
            }

            const auto &gltfMat = model.materials[materialIndex];
            const auto &pbr = gltfMat.pbrMetallicRoughness;

            // Base color factor
            material.setAlbedo(Vec3(
                static_cast<float>(pbr.baseColorFactor[0]),
                static_cast<float>(pbr.baseColorFactor[1]),
                static_cast<float>(pbr.baseColorFactor[2])));

            // Base color texture (albedo)
            if (pbr.baseColorTexture.index >= 0)
            {
                std::string texPath = getTextureUri(model, pbr.baseColorTexture.index, baseDir);
                if (!texPath.empty())
                {
                    material.setTexture(TEXTURE_ALBEDO, texPath);
                    material.setTextureSampler(TEXTURE_ALBEDO,
                        samplerKindForTexture(model, pbr.baseColorTexture.index));
                    std::cout << "  Albedo texture: " << texPath << std::endl;
                }
            }

            // Metallic-roughness factor
            material.setMetallic(static_cast<float>(pbr.metallicFactor));
            material.setRoughness(static_cast<float>(pbr.roughnessFactor));

            // Metallic-roughness texture
            if (pbr.metallicRoughnessTexture.index >= 0)
            {
                std::string texPath = getTextureUri(model, pbr.metallicRoughnessTexture.index, baseDir);
                if (!texPath.empty())
                {
                    material.setTexture(TEXTURE_METALLIC_ROUGHNESS, texPath);
                    material.setTextureSampler(TEXTURE_METALLIC_ROUGHNESS,
                        samplerKindForTexture(model, pbr.metallicRoughnessTexture.index));
                    std::cout << "  Metallic/Roughness texture: " << texPath << std::endl;
                }
            }

            // Normal texture
            if (gltfMat.normalTexture.index >= 0)
            {
                std::string texPath = getTextureUri(model, gltfMat.normalTexture.index, baseDir);
                if (!texPath.empty())
                {
                    material.setTexture(TEXTURE_NORMAL, texPath);
                    material.setTextureSampler(TEXTURE_NORMAL,
                        samplerKindForTexture(model, gltfMat.normalTexture.index));
                    std::cout << "  Normal texture: " << texPath << std::endl;
                }
            }

            // Emissive factor
            if (gltfMat.emissiveFactor.size() >= 3)
            {
                material.setEmission(Vec3(
                    static_cast<float>(gltfMat.emissiveFactor[0]),
                    static_cast<float>(gltfMat.emissiveFactor[1]),
                    static_cast<float>(gltfMat.emissiveFactor[2])));
            }

            // Emissive texture
            if (gltfMat.emissiveTexture.index >= 0)
            {
                std::string texPath = getTextureUri(model, gltfMat.emissiveTexture.index, baseDir);
                if (!texPath.empty())
                {
                    material.setTexture(TEXTURE_EMISSIVE, texPath);
                    material.setTextureSampler(TEXTURE_EMISSIVE,
                        samplerKindForTexture(model, gltfMat.emissiveTexture.index));
                    std::cout << "  Emissive texture: " << texPath << std::endl;
                }
            }

            // Occlusion texture
            if (gltfMat.occlusionTexture.index >= 0)
            {
                std::string texPath = getTextureUri(model, gltfMat.occlusionTexture.index, baseDir);
                if (!texPath.empty())
                {
                    material.setTexture(TEXTURE_OCCLUSION, texPath);
                    material.setTextureSampler(TEXTURE_OCCLUSION,
                        samplerKindForTexture(model, gltfMat.occlusionTexture.index));
                    std::cout << "  Occlusion texture: " << texPath << std::endl;
                }
            }

            // Transparency / emission extensions (tinygltf exposes unknown
            // extensions as a generic value map). These map onto the plain
            // float material properties convertMaterial packs into GPUMaterial.
            const auto getExtFloat = [&](const char *ext, const char *key,
                                         float &out) -> bool {
                auto it = gltfMat.extensions.find(ext);
                if (it == gltfMat.extensions.end() || !it->second.Has(key)) return false;
                out = static_cast<float>(it->second.Get(key).GetNumberAsDouble());
                return true;
            };
            float v = 0.0f;
            if (getExtFloat("KHR_materials_transmission", "transmissionFactor", v))
                material.setFloat("transmission", v);
            if (getExtFloat("KHR_materials_ior", "ior", v))
                material.setFloat("ior", v);
            if (getExtFloat("KHR_materials_emissive_strength", "emissiveStrength", v))
                material.setFloat("emissionStrength", v);
            if (getExtFloat("KHR_materials_clearcoat", "clearcoatFactor", v))
                material.setFloat("clearcoat", v);
            if (getExtFloat("KHR_materials_clearcoat", "clearcoatRoughnessFactor", v))
                material.setFloat("clearcoatRoughness", v);
            // KHR_materials_anisotropy: map strength → our signed [-1,1] scalar.
            // anisotropyRotation is dropped (our tangent is UV-fixed; honoring
            // it would need a separate tangent-rotation parameter).
            if (getExtFloat("KHR_materials_anisotropy", "anisotropyStrength", v))
                material.setFloat("anisotropy", v);
            // KHR_materials_sheen: sheenColorFactor is an RGB array; our sheen
            // lobe is a scalar (white) weight, so collapse to the max component.
            {
                auto it = gltfMat.extensions.find("KHR_materials_sheen");
                if (it != gltfMat.extensions.end() && it->second.Has("sheenColorFactor"))
                {
                    const auto &arr = it->second.Get("sheenColorFactor");
                    float sheen = 0.0f;
                    for (int i = 0; i < arr.ArrayLen(); ++i)
                        sheen = std::max(sheen,
                                         static_cast<float>(arr.Get(i).GetNumberAsDouble()));
                    if (sheen > 0.0f) material.setFloat("sheen", sheen);
                }
            }
            // alphaMode BLEND → use baseColor alpha as opacity (MASK is a hard
            // cutout we approximate the same way; OPAQUE leaves opacity at 1).
            if (gltfMat.alphaMode == "BLEND" || gltfMat.alphaMode == "MASK")
            {
                const float a = pbr.baseColorFactor.size() >= 4
                                    ? static_cast<float>(pbr.baseColorFactor[3])
                                    : 1.0f;
                material.setFloat("opacity", a);
            }

            return material;
        }

        // Convert GLTF node transform to our Transform. A node specifies its
        // local transform EITHER as a full 4x4 matrix OR as TRS components
        // (the glTF spec forbids mixing the two). Both are handled here:
        //   - The matrix form is decomposed into TRS. Previously it was
        //     ignored entirely, which silently placed every matrix-based node
        //     at the origin (many exporters, e.g. Blender, emit `matrix`).
        //   - The quaternion is applied directly. The old path round-tripped
        //     it through euler *degrees* and handed that Vec3 to
        //     setRotation(Quaternion), which reinterpreted the degree values
        //     as radians — so any non-trivial TRS rotation came in wrong.
        Transform convertNodeTransform(const tinygltf::Node &node)
        {
            Transform transform;

            if (node.matrix.size() == 16)
            {
                // glTF matrices are stored column-major, matching glm::mat4.
                glm::mat4 m(1.0f);
                for (int col = 0; col < 4; ++col)
                    for (int row = 0; row < 4; ++row)
                        m[col][row] = static_cast<float>(node.matrix[col * 4 + row]);

                glm::vec3 scale, translation, skew;
                glm::vec4 perspective;
                glm::quat rotation;
                if (glm::decompose(m, scale, rotation, translation, skew, perspective))
                {
                    transform.setPosition(translation);
                    transform.setRotation(rotation);
                    transform.setScale(scale);
                }
                return transform;
            }

            // TRS form.
            if (node.translation.size() == 3)
            {
                transform.setPosition(Vec3(
                    static_cast<float>(node.translation[0]),
                    static_cast<float>(node.translation[1]),
                    static_cast<float>(node.translation[2])));
            }

            if (node.rotation.size() == 4)
            {
                // glTF stores quaternions as (x, y, z, w); glm::quat takes (w, x, y, z).
                transform.setRotation(glm::quat(
                    static_cast<float>(node.rotation[3]),
                    static_cast<float>(node.rotation[0]),
                    static_cast<float>(node.rotation[1]),
                    static_cast<float>(node.rotation[2])));
            }

            if (node.scale.size() == 3)
            {
                transform.setScale(Vec3(
                    static_cast<float>(node.scale[0]),
                    static_cast<float>(node.scale[1]),
                    static_cast<float>(node.scale[2])));
            }

            return transform;
        }

        // Process a GLTF mesh primitive and add it to the scene
        SceneObject processPrimitive(const tinygltf::Model &model, const tinygltf::Primitive &primitive,
                                     const GltfLoader::LoadOptions &options)
        {
            SceneObject obj;

            // Get position accessor
            auto posIt = primitive.attributes.find("POSITION");
            if (posIt == primitive.attributes.end())
            {
                return obj; // No positions, skip
            }

            std::vector<Vec3> positions = extractVec3Accessor(model, posIt->second);

            // Apply scale factor
            if (options.scaleFactor != 1.0f)
            {
                for (auto &pos : positions)
                {
                    pos *= options.scaleFactor;
                }
            }

            // Get indices
            std::vector<uint32_t> indices;
            if (primitive.indices >= 0)
            {
                indices = extractIndicesAccessor(model, primitive.indices);
            }

            // Get normals
            std::vector<Vec3> normals;
            if (options.loadNormals)
            {
                auto normIt = primitive.attributes.find("NORMAL");
                if (normIt != primitive.attributes.end())
                {
                    normals = extractVec3Accessor(model, normIt->second);
                }
            }

            // Get texture coordinates
            std::vector<Vec2> texCoords;
            if (options.loadTexCoords)
            {
                auto texIt = primitive.attributes.find("TEXCOORD_0");
                if (texIt != primitive.attributes.end())
                {
                    texCoords = extractVec2Accessor(model, texIt->second);
                }
            }

            // Skinning attributes (the skeleton itself is attached later in
            // loadFromFile, once skins are parsed). Kept index-parallel to
            // positions through the same expansion below.
            std::vector<Vec4> joints, weights;
            {
                auto jIt = primitive.attributes.find("JOINTS_0");
                if (jIt != primitive.attributes.end())
                    joints = extractJointsVec4(model, jIt->second);
                auto wIt = primitive.attributes.find("WEIGHTS_0");
                if (wIt != primitive.attributes.end())
                    weights = extractVec4Accessor(model, wIt->second);
            }

            // If we have indices, expand to triangle list
            if (!indices.empty())
            {
                std::vector<Vec3> expandedPositions;
                std::vector<Vec3> expandedNormals;
                std::vector<Vec2> expandedTexCoords;
                std::vector<Vec4> expandedJoints;
                std::vector<Vec4> expandedWeights;

                expandedPositions.reserve(indices.size());
                if (!normals.empty())
                    expandedNormals.reserve(indices.size());
                if (!texCoords.empty())
                    expandedTexCoords.reserve(indices.size());
                if (!joints.empty())
                    expandedJoints.reserve(indices.size());
                if (!weights.empty())
                    expandedWeights.reserve(indices.size());

                for (uint32_t idx : indices)
                {
                    if (idx < positions.size())
                    {
                        expandedPositions.push_back(positions[idx]);
                    }
                    if (!normals.empty() && idx < normals.size())
                    {
                        expandedNormals.push_back(normals[idx]);
                    }
                    if (!texCoords.empty() && idx < texCoords.size())
                    {
                        expandedTexCoords.push_back(texCoords[idx]);
                    }
                    if (!joints.empty() && idx < joints.size())
                    {
                        expandedJoints.push_back(joints[idx]);
                    }
                    if (!weights.empty() && idx < weights.size())
                    {
                        expandedWeights.push_back(weights[idx]);
                    }
                }

                obj.setPositions(std::move(expandedPositions));
                if (!expandedNormals.empty())
                    obj.setNormals(std::move(expandedNormals));
                if (!expandedTexCoords.empty())
                    obj.setUvs(std::move(expandedTexCoords));
                if (!expandedJoints.empty())
                    obj.setJointIndices(std::move(expandedJoints));
                if (!expandedWeights.empty())
                    obj.setJointWeights(std::move(expandedWeights));
            }
            else
            {
                obj.setPositions(std::move(positions));
                if (!normals.empty())
                    obj.setNormals(std::move(normals));
                if (!texCoords.empty())
                    obj.setUvs(std::move(texCoords));
                if (!joints.empty())
                    obj.setJointIndices(std::move(joints));
                if (!weights.empty())
                    obj.setJointWeights(std::move(weights));
            }

            return obj;
        }

        // Recursively process GLTF nodes
        void processNode(const tinygltf::Model &model, int nodeIndex, Scene &scene,
                         const std::unordered_map<int, std::string> &meshToObjectName,
                         const GltfLoader::LoadOptions &options,
                         const std::string &baseDir,
                         Actor *parentActor = nullptr)
        {
            if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
                return;

            const auto &node = model.nodes[nodeIndex];

            // Create actor for this node
            Actor *actor = scene.createActor();
            actor->setName(node.name.empty() ? ("node_" + std::to_string(nodeIndex)) : node.name);
            actor->setTransform(convertNodeTransform(node));

            // If parent exists, add as child
            if (parentActor)
            {
                parentActor->addChild(actor);
            }

            // If this node has a mesh, create instances
            if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size()))
            {
                const auto &mesh = model.meshes[node.mesh];
                for (size_t primIdx = 0; primIdx < mesh.primitives.size(); ++primIdx)
                {
                    const auto &primitive = mesh.primitives[primIdx];

                    // Find the object name for this primitive
                    int primKey = node.mesh * 1000 + static_cast<int>(primIdx);
                    auto objIt = meshToObjectName.find(primKey);
                    if (objIt != meshToObjectName.end())
                    {
                        SceneInstance instance(objIt->second);

                        // Set material
                        if (options.loadMaterials)
                        {
                            instance.setMaterial(convertMaterial(model, primitive.material, baseDir));
                        }

                        actor->addInstance(std::move(instance));
                    }
                }
            }

            // Process children
            for (int childIndex : node.children)
            {
                processNode(model, childIndex, scene, meshToObjectName, options, baseDir, actor);
            }
        }
    }

    std::unique_ptr<Scene> GltfLoader::loadFromFile(const std::string &path)
    {
        return loadFromFile(path, LoadOptions{});
    }

    namespace
    {
        std::mutex &gltfCacheMutex()
        {
            static std::mutex m;
            return m;
        }
        std::unordered_map<std::string, std::shared_ptr<const Scene>> &gltfCache()
        {
            static std::unordered_map<std::string, std::shared_ptr<const Scene>> c;
            return c;
        }
    }

    std::shared_ptr<const Scene> GltfLoader::loadFromFileCached(const std::string &path)
    {
        // Fast path: hit the cache first under the lock.
        {
            std::lock_guard<std::mutex> lk(gltfCacheMutex());
            auto it = gltfCache().find(path);
            if (it != gltfCache().end()) return it->second;
        }
        // Slow path: parse outside the lock so concurrent readers of other
        // paths don't block on this one.
        std::shared_ptr<const Scene> sh;
        try
        {
            auto loaded = loadFromFile(path);
            sh = std::shared_ptr<const Scene>(loaded.release());
        }
        catch (...)
        {
            // Cache a null result so a broken file doesn't get re-parsed on
            // every cook; the SOP cook code treats null as "skip this prim".
            // Callers that want a fresh re-try should call invalidateCache.
            std::lock_guard<std::mutex> lk(gltfCacheMutex());
            gltfCache()[path] = nullptr;
            throw;
        }
        std::lock_guard<std::mutex> lk(gltfCacheMutex());
        // Re-check after re-acquiring — another worker may have raced us.
        auto &cache = gltfCache();
        auto it = cache.find(path);
        if (it != cache.end() && it->second) return it->second;
        cache[path] = sh;
        return sh;
    }

    void GltfLoader::invalidateCache(const std::string &path)
    {
        std::lock_guard<std::mutex> lk(gltfCacheMutex());
        gltfCache().erase(path);
    }

    std::unique_ptr<Scene> GltfLoader::loadFromFile(const std::string &path, const LoadOptions &options)
    {
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;
        std::string err;
        std::string warn;

        bool success = false;
        if (path.ends_with(".glb"))
        {
            success = loader.LoadBinaryFromFile(&model, &err, &warn, path);
        }
        else
        {
            success = loader.LoadASCIIFromFile(&model, &err, &warn, path);
        }

        if (!warn.empty())
        {
            std::cerr << "GLTF Warning: " << warn << std::endl;
        }

        if (!err.empty())
        {
            std::cerr << "GLTF Error: " << err << std::endl;
        }

        if (!success)
        {
            throw std::runtime_error("Failed to load GLTF file: " + path);
        }

        // Extract base directory for texture paths
        std::string baseDir;
        size_t lastSlash = path.find_last_of("/\\");
        if (lastSlash != std::string::npos)
        {
            baseDir = path.substr(0, lastSlash);
        }
        else
        {
            baseDir = ".";
        }

        auto scene = std::make_unique<Scene>();

        // Extract embedded textures from GLTF model before processing
        extractEmbeddedTextures(model, *scene);

        // First pass: Create SceneObjects for all mesh primitives
        std::unordered_map<int, std::string> meshToObjectName;

        for (size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx)
        {
            const auto &mesh = model.meshes[meshIdx];
            for (size_t primIdx = 0; primIdx < mesh.primitives.size(); ++primIdx)
            {
                const auto &primitive = mesh.primitives[primIdx];

                // Only support triangles
                if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1)
                {
                    std::cerr << "Skipping non-triangle primitive in mesh " << meshIdx << std::endl;
                    continue;
                }

                SceneObject obj = processPrimitive(model, primitive, options);
                if (obj.vertexCount() == 0)
                {
                    continue;
                }

                std::string objectName = mesh.name.empty()
                                             ? ("mesh_" + std::to_string(meshIdx) + "_prim_" + std::to_string(primIdx))
                                             : (mesh.name + "_prim_" + std::to_string(primIdx));

                int primKey = static_cast<int>(meshIdx) * 1000 + static_cast<int>(primIdx);
                meshToObjectName[primKey] = objectName;

                scene->addObject(objectName, std::move(obj));
            }
        }

        std::cout << "Loaded " << scene->objects().size() << " mesh primitives from GLTF" << std::endl;

        // Skinning pass: build one Skeleton per skin and attach it to the
        // SceneObjects of every skinned mesh (node.skin + node.mesh). The
        // skinning deformer downstream reads (joints, weights, skeleton, time)
        // straight off the SceneObject. Skipped entirely when the asset has no
        // skins — non-character imports pay nothing.
        if (!model.skins.empty())
        {
            auto skeletons = buildSkeletons(model);
            for (size_t ni = 0; ni < model.nodes.size(); ++ni)
            {
                const auto &node = model.nodes[ni];
                if (node.skin < 0 || node.mesh < 0 ||
                    node.mesh >= static_cast<int>(model.meshes.size()))
                    continue;
                auto sit = skeletons.find(node.skin);
                if (sit == skeletons.end())
                    continue;
                // Express skinning in this mesh node's local space — the engine
                // actor for the node already carries its world transform, so
                // baking world-space positions would double-transform it.
                const Mat4 shift = glm::inverse(sit->second->nodeBindWorld(static_cast<int>(ni)));
                const auto &mesh = model.meshes[node.mesh];
                for (size_t primIdx = 0; primIdx < mesh.primitives.size(); ++primIdx)
                {
                    const int primKey = node.mesh * 1000 + static_cast<int>(primIdx);
                    auto it = meshToObjectName.find(primKey);
                    if (it == meshToObjectName.end())
                        continue;
                    if (auto *obj = scene->getObject(it->second))
                    {
                        obj->setSkeleton(sit->second);
                        obj->setSkinBindShift(shift);
                    }
                }
            }
        }

        // Second pass: Process scene hierarchy
        if (!model.scenes.empty())
        {
            int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
            const auto &gltfScene = model.scenes[sceneIndex];

            for (int nodeIndex : gltfScene.nodes)
            {
                processNode(model, nodeIndex, *scene, meshToObjectName, options, baseDir, nullptr);
            }
        }
        else if (!model.nodes.empty())
        {
            // No scenes defined, process all root nodes
            for (size_t i = 0; i < model.nodes.size(); ++i)
            {
                // Check if this node is a root (not a child of any other node)
                bool isRoot = true;
                for (const auto &node : model.nodes)
                {
                    for (int child : node.children)
                    {
                        if (child == static_cast<int>(i))
                        {
                            isRoot = false;
                            break;
                        }
                    }
                    if (!isRoot)
                        break;
                }

                if (isRoot)
                {
                    processNode(model, static_cast<int>(i), *scene, meshToObjectName, options, baseDir, nullptr);
                }
            }
        }

        std::cout << "Created " << scene->actors().size() << " actors from GLTF nodes" << std::endl;

        // Load camera if present
        if (!model.cameras.empty())
        {
            // Find a node that references a camera to get its transform
            for (size_t nodeIdx = 0; nodeIdx < model.nodes.size(); ++nodeIdx)
            {
                const auto &node = model.nodes[nodeIdx];
                if (node.camera >= 0 && node.camera < static_cast<int>(model.cameras.size()))
                {
                    const auto &gltfCamera = model.cameras[node.camera];
                    Camera camera;

                    // Build world transform for camera node (walk up parent chain)
                    Mat4 worldTransform(1.0f);

                    // Helper lambda to get node's local transform
                    auto getNodeLocalTransform = [&model](int idx) -> Mat4
                    {
                        const auto &n = model.nodes[idx];
                        Mat4 local(1.0f);

                        if (!n.matrix.empty())
                        {
                            // Use matrix directly (column-major in GLTF)
                            for (int i = 0; i < 16; ++i)
                            {
                                local[i / 4][i % 4] = static_cast<float>(n.matrix[i]);
                            }
                        }
                        else
                        {
                            // Build from TRS
                            Mat4 T(1.0f), R(1.0f), S(1.0f);
                            if (!n.translation.empty())
                            {
                                T = glm::translate(Mat4(1.0f), Vec3(
                                                                   static_cast<float>(n.translation[0]),
                                                                   static_cast<float>(n.translation[1]),
                                                                   static_cast<float>(n.translation[2])));
                            }
                            if (!n.rotation.empty())
                            {
                                glm::quat q(
                                    static_cast<float>(n.rotation[3]), // w
                                    static_cast<float>(n.rotation[0]), // x
                                    static_cast<float>(n.rotation[1]), // y
                                    static_cast<float>(n.rotation[2])  // z
                                );
                                R = glm::mat4_cast(q);
                            }
                            if (!n.scale.empty())
                            {
                                S = glm::scale(Mat4(1.0f), Vec3(
                                                               static_cast<float>(n.scale[0]),
                                                               static_cast<float>(n.scale[1]),
                                                               static_cast<float>(n.scale[2])));
                            }
                            local = T * R * S;
                        }
                        return local;
                    };

                    // Find parent chain for camera node
                    std::vector<int> parentChain;
                    parentChain.push_back(static_cast<int>(nodeIdx));

                    // Build parent map
                    std::vector<int> parentOf(model.nodes.size(), -1);
                    for (size_t i = 0; i < model.nodes.size(); ++i)
                    {
                        for (int childIdx : model.nodes[i].children)
                        {
                            parentOf[childIdx] = static_cast<int>(i);
                        }
                    }

                    // Walk up the parent chain
                    int currentIdx = static_cast<int>(nodeIdx);
                    while (parentOf[currentIdx] >= 0)
                    {
                        currentIdx = parentOf[currentIdx];
                        parentChain.push_back(currentIdx);
                    }

                    // Compute world transform from root to camera
                    for (auto it = parentChain.rbegin(); it != parentChain.rend(); ++it)
                    {
                        worldTransform = worldTransform * getNodeLocalTransform(*it);
                    }

                    // Extract position from world transform
                    Vec3 position = Vec3(worldTransform[3]);
                    camera.setPosition(position);

                    // Extract rotation from world transform
                    // Normalize the columns to remove scale influence
                    Mat3 rotMat = Mat3(worldTransform);
                    rotMat[0] = glm::normalize(rotMat[0]);
                    rotMat[1] = glm::normalize(rotMat[1]);
                    rotMat[2] = glm::normalize(rotMat[2]);
                    glm::quat rotation = glm::quat_cast(rotMat);
                    camera.setRotation(rotation);

                    // Get camera parameters
                    if (gltfCamera.type == "perspective")
                    {
                        // GLTF stores yfov in radians
                        float fovDegrees = glm::degrees(static_cast<float>(gltfCamera.perspective.yfov));
                        camera.setFov(fovDegrees);
                        camera.setNearPlane(static_cast<float>(gltfCamera.perspective.znear));
                        if (gltfCamera.perspective.zfar > 0)
                        {
                            camera.setFarPlane(static_cast<float>(gltfCamera.perspective.zfar));
                        }
                        if (gltfCamera.perspective.aspectRatio > 0)
                        {
                            camera.setAspectRatio(static_cast<float>(gltfCamera.perspective.aspectRatio));
                        }
                    }

                    scene->setCamera(camera);
                    std::cout << "Loaded camera from GLTF: position=("
                              << camera.position().x << ", "
                              << camera.position().y << ", "
                              << camera.position().z << "), fov="
                              << camera.fov() << " degrees" << std::endl;
                    std::cout << "  Camera forward: (" << camera.forward().x << ", "
                              << camera.forward().y << ", " << camera.forward().z << ")" << std::endl;
                    break; // Use first camera found
                }
            }
        }

        return scene;
    }

    // ── Hierarchy peek ─────────────────────────────────────────────────────
    namespace
    {
        // Convert a quaternion to ZYX-intrinsic euler-degrees, matching the
        // forward conversion used by transform_sop.cpp / sop_graph.cpp's
        // eulerDegToQuatWxyz / set_actor_rotation_euler. Gimbal-locked near
        // ±90° pitch (Houdini lives with this).
        Vec3 quatToEulerDegZYX(const glm::quat &q)
        {
            // glm matrices are column-major: m[col][row]. Extract the ZYX
            // intrinsic decomposition from m where R = Rz * Ry * Rx.
            const glm::mat3 m = glm::mat3_cast(q);
            const float r02 = m[0][2];   // R[2][0] in row-major → -sin(ry)
            const float r00 = m[0][0];   // R[0][0]
            const float r01 = m[0][1];   // R[1][0]
            const float r12 = m[1][2];   // R[2][1]
            const float r22 = m[2][2];   // R[2][2]

            float sy = -r02;
            float cy = std::sqrt(r00 * r00 + r01 * r01);

            float rx, ry, rz;
            if (cy > 1e-6f)
            {
                rx = std::atan2(r12, r22);
                ry = std::atan2(sy, cy);
                rz = std::atan2(r01, r00);
            }
            else
            {
                // Gimbal lock fallback: roll absorbed into yaw, pitch at ±π/2.
                const float r11 = m[1][1];   // R[1][1]
                const float r21 = m[2][1];   // R[1][2]
                rx = std::atan2(-r21, r11);
                ry = std::atan2(sy, cy);
                rz = 0.0f;
            }

            constexpr float kRad2Deg = 180.0f / 3.1415926535f;
            return Vec3(rx * kRad2Deg, ry * kRad2Deg, rz * kRad2Deg);
        }

        // Generate the SceneObject names that GltfLoader::loadFromFile assigns
        // to every primitive of `meshIdx`. Naming must match loadFromFile's
        // `<mesh.name>_prim_<primIdx>` scheme exactly so the runtime
        // `gltf_import` lookup hits.
        //
        // Critically, we mirror loadFromFile's *gating* too: that loop skips
        // non-triangle primitives and primitives without a POSITION accessor,
        // so those `_prim_<i>` names never actually get registered as scene
        // objects. If we emitted them here anyway, the frontend would build a
        // gltf_import → object_output chain for a name the cook can't resolve,
        // and the resulting subnet (often the leaf of a multi-primitive mesh)
        // would render with no geometry. Matching the skip keeps peek and
        // load in lockstep.
        std::vector<std::string> sceneObjectNamesForMesh(const tinygltf::Model &model, int meshIdx)
        {
            std::vector<std::string> out;
            if (meshIdx < 0 || meshIdx >= static_cast<int>(model.meshes.size()))
                return out;
            const auto &mesh = model.meshes[meshIdx];
            const std::string base = mesh.name.empty()
                ? ("mesh_" + std::to_string(meshIdx))
                : mesh.name;
            out.reserve(mesh.primitives.size());
            for (size_t p = 0; p < mesh.primitives.size(); ++p)
            {
                const auto &prim = mesh.primitives[p];
                // Same gate as processPrimitive's loop in loadFromFile.
                if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1)
                    continue;
                if (prim.attributes.find("POSITION") == prim.attributes.end())
                    continue;
                out.push_back(base + "_prim_" + std::to_string(p));
            }
            return out;
        }

        GltfLoader::HierarchyNode buildPeekNode(const tinygltf::Model &model, int nodeIndex)
        {
            GltfLoader::HierarchyNode out;
            if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) return out;
            const auto &node = model.nodes[nodeIndex];

            out.name = node.name.empty() ? ("node_" + std::to_string(nodeIndex)) : node.name;

            // glTF nodes may carry either a 4x4 matrix OR a TRS triple. When
            // the matrix is present, decompose to TRS — peek throws away the
            // last-row perspective (always 0,0,0,1 for glTF nodes).
            glm::quat q(1, 0, 0, 0);
            if (!node.matrix.empty())
            {
                glm::mat4 m(1.0f);
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        m[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
                Vec3 t(m[3][0], m[3][1], m[3][2]);
                Vec3 s(glm::length(glm::vec3(m[0])),
                       glm::length(glm::vec3(m[1])),
                       glm::length(glm::vec3(m[2])));
                glm::mat3 rotM(
                    glm::vec3(m[0]) / std::max(s.x, 1e-8f),
                    glm::vec3(m[1]) / std::max(s.y, 1e-8f),
                    glm::vec3(m[2]) / std::max(s.z, 1e-8f));
                q = glm::quat_cast(rotM);
                out.translate = t;
                out.scale = s;
            }
            else
            {
                if (node.translation.size() == 3)
                    out.translate = Vec3(static_cast<float>(node.translation[0]),
                                         static_cast<float>(node.translation[1]),
                                         static_cast<float>(node.translation[2]));
                if (node.rotation.size() == 4)
                {
                    // glTF stores quaternion as (x, y, z, w); glm::quat is (w, x, y, z).
                    q = glm::quat(static_cast<float>(node.rotation[3]),
                                  static_cast<float>(node.rotation[0]),
                                  static_cast<float>(node.rotation[1]),
                                  static_cast<float>(node.rotation[2]));
                }
                if (node.scale.size() == 3)
                    out.scale = Vec3(static_cast<float>(node.scale[0]),
                                     static_cast<float>(node.scale[1]),
                                     static_cast<float>(node.scale[2]));
            }
            out.rotateEulerDeg = quatToEulerDegZYX(q);

            if (node.mesh >= 0)
                out.meshObjectNames = sceneObjectNamesForMesh(model, node.mesh);

            out.children.reserve(node.children.size());
            for (int childIdx : node.children)
                out.children.push_back(buildPeekNode(model, childIdx));

            return out;
        }
    } // anon

    std::vector<GltfLoader::HierarchyNode> GltfLoader::peekHierarchy(const std::string &path)
    {
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;
        std::string err, warn;

        // Bypass image decoding for the peek. tinygltf's default loader runs
        // stb_image on every embedded texture, which on a Sponza-scale file
        // (69× 1024² RGBA) takes seconds on whatever thread parses the JSON
        // — and the WKWebView message handler runs on the main thread, so
        // the editor beachballs while peekGltf returns. Returning true from
        // the no-op signals "image accepted" without writing pixel data;
        // the peek only walks nodes/meshes/primitives, so empty Image
        // entries are harmless. Buffer loading still happens (needed for
        // mesh references) but the dominant cost was the image decode.
        loader.SetImageLoader(
            [](tinygltf::Image* /*image*/, const int /*image_idx*/,
               std::string* /*err*/, std::string* /*warn*/,
               int /*req_width*/, int /*req_height*/,
               const unsigned char* /*bytes*/, int /*size*/,
               void* /*user_data*/) -> bool {
                return true;
            },
            nullptr);

        bool success;
        if (path.ends_with(".glb"))
            success = loader.LoadBinaryFromFile(&model, &err, &warn, path);
        else
            success = loader.LoadASCIIFromFile(&model, &err, &warn, path);

        if (!success)
            throw std::runtime_error("Failed to peek glTF: " + path + (err.empty() ? "" : " — " + err));

        std::vector<HierarchyNode> roots;
        if (!model.scenes.empty())
        {
            const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
            const auto &gltfScene = model.scenes[sceneIndex];
            roots.reserve(gltfScene.nodes.size());
            for (int idx : gltfScene.nodes)
                roots.push_back(buildPeekNode(model, idx));
        }
        else
        {
            // No scene declaration — find nodes that aren't anyone's child.
            std::vector<bool> isChild(model.nodes.size(), false);
            for (const auto &n : model.nodes)
                for (int c : n.children)
                    if (c >= 0 && c < static_cast<int>(model.nodes.size()))
                        isChild[c] = true;
            for (size_t i = 0; i < model.nodes.size(); ++i)
                if (!isChild[i])
                    roots.push_back(buildPeekNode(model, static_cast<int>(i)));
        }
        return roots;
    }
}
