#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "gltf_loader.hpp"
#include "camera.hpp"
#include <tiny_gltf.h>
#include <iostream>
#include <unordered_map>
#include <cmath>

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

        // Helper to get texture URI from GLTF texture index
        std::string getTextureUri(const tinygltf::Model &model, int textureIndex, const std::string &baseDir)
        {
            if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
                return "";

            const auto &texture = model.textures[textureIndex];
            if (texture.source < 0 || texture.source >= static_cast<int>(model.images.size()))
                return "";

            const auto &image = model.images[texture.source];

            // If the image has a URI, return the full path
            if (!image.uri.empty())
            {
                // Check if it's a data URI (embedded)
                if (image.uri.find("data:") == 0)
                {
                    // Embedded image - return a special identifier
                    return "embedded:" + std::to_string(texture.source);
                }
                // External file - combine with base directory
                return baseDir + "/" + image.uri;
            }

            // Image is embedded via bufferView - return special identifier
            return "embedded:" + std::to_string(texture.source);
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
                    std::cout << "  Occlusion texture: " << texPath << std::endl;
                }
            }

            return material;
        }

        // Convert GLTF node transform to our Transform
        Transform convertNodeTransform(const tinygltf::Node &node)
        {
            Transform transform;

            if (!node.matrix.empty())
            {
                // Node has a matrix - decompose it
                // For simplicity, we'll extract TRS if available, otherwise use identity
                // Full matrix decomposition would be more complex
            }

            if (!node.translation.empty())
            {
                transform.setPosition(Vec3(
                    static_cast<float>(node.translation[0]),
                    static_cast<float>(node.translation[1]),
                    static_cast<float>(node.translation[2])));
            }

            if (!node.rotation.empty())
            {
                // GLTF uses quaternions (x, y, z, w)
                // Convert quaternion to euler angles (degrees)
                float x = static_cast<float>(node.rotation[0]);
                float y = static_cast<float>(node.rotation[1]);
                float z = static_cast<float>(node.rotation[2]);
                float w = static_cast<float>(node.rotation[3]);

                // Convert quaternion to euler angles
                glm::quat q(w, x, y, z);
                Vec3 euler = glm::degrees(glm::eulerAngles(q));
                transform.setRotation(euler);
            }

            if (!node.scale.empty())
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

            // If we have indices, expand to triangle list
            if (!indices.empty())
            {
                std::vector<Vec3> expandedPositions;
                std::vector<Vec3> expandedNormals;
                std::vector<Vec2> expandedTexCoords;

                expandedPositions.reserve(indices.size());
                if (!normals.empty())
                    expandedNormals.reserve(indices.size());
                if (!texCoords.empty())
                    expandedTexCoords.reserve(indices.size());

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
                }

                obj.setPositions(std::move(expandedPositions));
                if (!expandedNormals.empty())
                    obj.setNormals(std::move(expandedNormals));
                if (!expandedTexCoords.empty())
                    obj.setUvs(std::move(expandedTexCoords));
            }
            else
            {
                obj.setPositions(std::move(positions));
                if (!normals.empty())
                    obj.setNormals(std::move(normals));
                if (!texCoords.empty())
                    obj.setUvs(std::move(texCoords));
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
}
