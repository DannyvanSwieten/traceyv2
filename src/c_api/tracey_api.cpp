#include "tracey_api.h"
#include "../device/device.hpp"
#include "../device/gpu/vulkan_compute_device.hpp"
#include "../device/gpu/vulkan_image_2d.hpp"
#include "../scene/scene.hpp"
#include "../scene/scene_object.hpp"
#include "../scene/scene_compiler.hpp"
#include "../scene/gltf_loader.hpp"
#include "../rendering/path_tracer.hpp"
#include "../rendering/rasterizer.hpp"
#include "../gpu/vulkan_presenter.hpp"
#include "../gpu/vulkan_surface_factory.hpp"

#include <string>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <iostream>

// Thread-local error storage
thread_local std::string g_lastError;

// Helper function to set error and return failure
static void setError(const std::string& error) {
    g_lastError = error;
}

static void clearError() {
    g_lastError.clear();
}

// Helper: Convert C TraceyVec3 to C++ Vec3
static tracey::Vec3 toVec3(const TraceyVec3& v) {
    return tracey::Vec3(v.x, v.y, v.z);
}

// Helper: Convert C++ Vec3 to C TraceyVec3
static TraceyVec3 fromVec3(const tracey::Vec3& v) {
    return TraceyVec3{v.x, v.y, v.z};
}

// Helper: Convert C TraceyQuat to C++ Quaternion
static tracey::Quaternion toQuat(const TraceyQuat& q) {
    return tracey::Quaternion(q.w, q.x, q.y, q.z);
}

// Helper: Convert C++ Quaternion to C TraceyQuat
static TraceyQuat fromQuat(const tracey::Quaternion& q) {
    return TraceyQuat{q.w, q.x, q.y, q.z};
}

// Helper: Convert C TraceyTransform to C++ Transform
static tracey::Transform toTransform(const TraceyTransform& t) {
    tracey::Transform result;
    result.setPosition(toVec3(t.position));
    result.setRotation(toQuat(t.rotation));
    result.setScale(toVec3(t.scale));
    return result;
}

// Helper: Convert C++ Transform to C TraceyTransform
static TraceyTransform fromTransform(const tracey::Transform& t) {
    TraceyTransform result;
    result.position = fromVec3(t.position());
    result.rotation = fromQuat(t.rotation());
    result.scale = fromVec3(t.scale());
    return result;
}

// Helper: Convert C TraceyCamera to C++ Camera
static tracey::Camera toCamera(const TraceyCamera& c) {
    tracey::Camera cam;
    cam.setPosition(toVec3(c.position));
    cam.setRotation(toQuat(c.rotation));
    cam.setFov(c.fov);
    cam.setNearPlane(c.nearPlane);
    cam.setFarPlane(c.farPlane);
    cam.setAspectRatio(c.aspectRatio);
    return cam;
}

// Helper: Convert C++ Camera to C TraceyCamera
static TraceyCamera fromCamera(const tracey::Camera& cam) {
    TraceyCamera c;
    c.position = fromVec3(cam.position());
    c.rotation = fromQuat(cam.rotation());
    c.fov = cam.fov();
    c.nearPlane = cam.nearPlane();
    c.farPlane = cam.farPlane();
    c.aspectRatio = cam.aspectRatio();
    return c;
}

// Helper: Convert C TraceyMaterialInstance to C++ MaterialInstance
static tracey::MaterialInstance toMaterialInstance(const TraceyMaterialInstance& mat) {
    tracey::MaterialInstance result(mat.shaderId ? mat.shaderId : "");

    for (uint32_t i = 0; i < mat.propertyCount; ++i) {
        const auto& prop = mat.properties[i];
        if (!prop.name) continue;

        switch (prop.type) {
            case TRACEY_MATERIAL_PROP_FLOAT:
                result.setFloat(prop.name, prop.value.floatValue);
                break;
            case TRACEY_MATERIAL_PROP_VEC2:
                result.setVec2(prop.name, tracey::Vec2(
                    prop.value.vec2Value.x,
                    prop.value.vec2Value.y
                ));
                break;
            case TRACEY_MATERIAL_PROP_VEC3:
                result.setVec3(prop.name, toVec3(prop.value.vec3Value));
                break;
            case TRACEY_MATERIAL_PROP_VEC4:
                result.setVec4(prop.name, tracey::Vec4(
                    prop.value.vec4Value.x,
                    prop.value.vec4Value.y,
                    prop.value.vec4Value.z,
                    prop.value.vec4Value.w
                ));
                break;
            case TRACEY_MATERIAL_PROP_INT:
                result.setInt(prop.name, prop.value.intValue);
                break;
            case TRACEY_MATERIAL_PROP_TEXTURE:
                if (prop.value.textureValue) {
                    result.setTexture(prop.name, prop.value.textureValue);
                }
                break;
        }
    }

    return result;
}

// Helper: Convert C TraceySceneInstance to C++ SceneInstance
static tracey::SceneInstance toSceneInstance(const TraceySceneInstance& inst) {
    std::string objRef = inst.objectRef ? inst.objectRef : "";
    tracey::MaterialInstance mat = toMaterialInstance(inst.material);

    tracey::SceneInstance result(objRef, mat);

    if (inst.hasLocalTransform) {
        result.setLocalTransform(toTransform(inst.localTransform));
    }

    return result;
}

extern "C" {

// ============================================================================
// Device Management
// ============================================================================

TraceyDevice* tracey_create_device(
    TraceyDeviceType deviceType,
    TraceyDeviceBackend backend)
{
    try {
        clearError();

        auto type = (deviceType == TRACEY_DEVICE_GPU)
            ? tracey::DeviceType::Gpu
            : tracey::DeviceType::Cpu;

        auto backendType = tracey::DeviceBackend::None;
        if (backend == TRACEY_DEVICE_BACKEND_COMPUTE) {
            backendType = tracey::DeviceBackend::Compute;
        } else if (backend == TRACEY_DEVICE_BACKEND_RTX) {
            backendType = tracey::DeviceBackend::Rtx;
        }

        tracey::Device* device = tracey::createDevice(type, backendType);
        return reinterpret_cast<TraceyDevice*>(device);

    } catch (const std::exception& e) {
        setError(std::string("Device creation failed: ") + e.what());
        return nullptr;
    } catch (...) {
        setError("Device creation failed: unknown error");
        return nullptr;
    }
}

void tracey_destroy_device(TraceyDevice* device)
{
    if (device) {
        delete reinterpret_cast<tracey::Device*>(device);
    }
}

void tracey_device_wait_idle(TraceyDevice* device)
{
    if (device) {
        reinterpret_cast<tracey::Device*>(device)->waitIdle();
    }
}

// ============================================================================
// Scene Management
// ============================================================================

TraceyScene* tracey_scene_create(void)
{
    try {
        clearError();
        auto* scene = new tracey::Scene();
        return reinterpret_cast<TraceyScene*>(scene);
    } catch (const std::exception& e) {
        setError(std::string("Scene creation failed: ") + e.what());
        return nullptr;
    } catch (...) {
        setError("Scene creation failed: unknown error");
        return nullptr;
    }
}

void tracey_scene_destroy(TraceyScene* scene)
{
    if (scene) {
        delete reinterpret_cast<tracey::Scene*>(scene);
    }
}

void tracey_scene_clear(TraceyScene* scene)
{
    if (scene) {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        s->clear();
    }
}

uint64_t tracey_scene_create_actor(
    TraceyScene* scene,
    const char* name)
{
    if (!scene) {
        setError("Scene pointer is null");
        return UINT64_MAX;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->createActor();
        if (name) {
            actor->setName(name);
        }
        return actor->getUid();
    } catch (const std::exception& e) {
        setError(std::string("Actor creation failed: ") + e.what());
        return UINT64_MAX;
    } catch (...) {
        setError("Actor creation failed: unknown error");
        return UINT64_MAX;
    }
}

TraceyResult tracey_scene_remove_actor(
    TraceyScene* scene,
    uint64_t actorUid)
{
    if (!scene) {
        setError("Scene pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        s->removeActor(actorUid);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Actor removal failed: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Actor removal failed: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_get_actor_transform(
    TraceyScene* scene,
    uint64_t actorUid,
    TraceyTransform* outTransform)
{
    if (!scene || !outTransform) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) {
            setError("Actor not found");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        *outTransform = fromTransform(actor->transform());
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get actor transform: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to get actor transform: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_set_actor_transform(
    TraceyScene* scene,
    uint64_t actorUid,
    const TraceyTransform* transform)
{
    if (!scene || !transform) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) {
            setError("Actor not found");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        actor->setTransform(toTransform(*transform));
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set actor transform: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to set actor transform: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_set_actor_name(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name)
{
    if (!scene || !name) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) {
            setError("Actor not found");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        actor->setName(name);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set actor name: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to set actor name: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_add_instance(
    TraceyScene* scene,
    uint64_t actorUid,
    const TraceySceneInstance* instance)
{
    if (!scene || !instance) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) {
            setError("Actor not found");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        actor->addInstance(toSceneInstance(*instance));
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to add instance: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to add instance: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_set_camera(
    TraceyScene* scene,
    const TraceyCamera* camera)
{
    if (!scene || !camera) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        s->setCamera(toCamera(*camera));
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set camera: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to set camera: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_get_camera(
    TraceyScene* scene,
    TraceyCamera* outCamera)
{
    if (!scene || !outCamera) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        if (!s->hasCamera()) {
            setError("Scene has no camera");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        *outCamera = fromCamera(s->camera());
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get camera: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to get camera: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_load_gltf_with_project(
    TraceyScene* scene,
    const char* filePath,
    const char* projectRoot)
{
    if (!scene || !filePath) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);

        // Clear existing scene before loading new one
        s->clear();

        // Load GLTF into a temporary scene with project root
        tracey::GltfLoader::LoadOptions options;
        if (projectRoot && projectRoot[0] != '\0') {
            options.projectRoot = std::string(projectRoot);
        }

        auto loadedScene = tracey::GltfLoader::loadFromFile(filePath, options);
        if (!loadedScene) {
            setError("Failed to load GLTF file");
            return TRACEY_ERROR_FILE_NOT_FOUND;
        }

        // Extract base filename from path
        std::string filePathStr(filePath);
        size_t lastSlash = filePathStr.find_last_of("/\\");
        size_t lastDot = filePathStr.find_last_of('.');
        std::string baseName = filePathStr.substr(
            lastSlash == std::string::npos ? 0 : lastSlash + 1,
            lastDot == std::string::npos ? std::string::npos : lastDot - (lastSlash == std::string::npos ? 0 : lastSlash + 1)
        );

        // Copy loaded scene into cleared scene
        // First pass: Copy all actors and build UID mapping
        std::unordered_map<size_t, size_t> uidMapping; // old UID -> new UID
        for (const auto& actor : loadedScene->actors()) {
            if (!actor) continue;  // Skip null actors
            auto* newActor = s->createActor();
            newActor->setName(actor->name());
            newActor->setTransform(actor->transform());
            for (const auto& instance : actor->instances()) {
                newActor->addInstance(instance);
            }
            // Store UID mapping
            uidMapping[actor->getUid()] = newActor->getUid();
        }

        // Second pass: Copy parent-child relationships
        for (const auto& actor : loadedScene->actors()) {
            if (!actor) continue;  // Skip null actors
            size_t newActorUid = uidMapping[actor->getUid()];
            auto* newActor = s->getActor(newActorUid);
            if (!newActor) continue;

            // Copy children relationships
            for (size_t oldChildUid : actor->children()) {
                if (uidMapping.count(oldChildUid)) {
                    size_t newChildUid = uidMapping[oldChildUid];
                    auto* childActor = s->getActor(newChildUid);
                    if (childActor) {
                        newActor->addChild(childActor);
                    }
                }
            }
        }

        // If multiple top-level actors were imported, create a parent to group them
        std::vector<size_t> topLevelActors;
        for (const auto& [oldUid, newUid] : uidMapping) {
            auto* actor = s->getActor(newUid);
            if (!actor) continue;

            // Check if this actor has a parent
            bool hasParent = false;
            for (const auto& otherActor : s->actors()) {
                if (!otherActor || otherActor->getUid() == newUid) continue;
                for (size_t childUid : otherActor->children()) {
                    if (childUid == newUid) {
                        hasParent = true;
                        break;
                    }
                }
                if (hasParent) break;
            }

            if (!hasParent) {
                topLevelActors.push_back(newUid);
            }
        }

        // If we have multiple top-level actors, create a parent group
        if (topLevelActors.size() > 1) {
            auto* parentActor = s->createActor();
            parentActor->setName(baseName);  // Name it after the file

            // Make all top-level actors children of this parent
            for (size_t actorUid : topLevelActors) {
                auto* actor = s->getActor(actorUid);
                if (actor) {
                    parentActor->addChild(actor);
                }
            }
        }

        // Move all objects (SceneObject is no longer copyable)
        for (const auto& [name, obj] : loadedScene->objects()) {
            tracey::SceneObject newObj(name);
            newObj.setPositions(obj->positions());
            newObj.setIndices(obj->indices());
            newObj.setNormals(obj->normals());
            newObj.setUvs(obj->uvs());
            s->addObject(name, std::move(newObj));
        }

        // Copy camera if present
        if (loadedScene->hasCamera()) {
            s->setCamera(loadedScene->camera());
        }

        // Copy embedded textures
        for (const auto& [id, tex] : loadedScene->embeddedTextures()) {
            tracey::EmbeddedTexture texCopy = tex;
            s->addEmbeddedTexture(id, std::move(texCopy));
        }

        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("GLTF loading failed: ") + e.what());
        return TRACEY_ERROR_FILE_NOT_FOUND;
    } catch (...) {
        setError("GLTF loading failed: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_add_gltf_with_project(
    TraceyScene* scene,
    const char* filePath,
    const char* projectRoot)
{
    if (!scene || !filePath) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);

        // DON'T clear existing scene - we're adding to it
        // s->clear(); // REMOVED - this is the key difference

        // Load GLTF into a temporary scene with project root
        tracey::GltfLoader::LoadOptions options;
        if (projectRoot && projectRoot[0] != '\0') {
            options.projectRoot = std::string(projectRoot);
        }

        auto loadedScene = tracey::GltfLoader::loadFromFile(filePath, options);
        if (!loadedScene) {
            setError("Failed to load GLTF file");
            return TRACEY_ERROR_FILE_NOT_FOUND;
        }

        // Extract base filename from path for texture ID prefix
        std::string filePathStr(filePath);
        size_t lastSlash = filePathStr.find_last_of("/\\");
        size_t lastDot = filePathStr.find_last_of('.');
        std::string baseName = filePathStr.substr(
            lastSlash == std::string::npos ? 0 : lastSlash + 1,
            lastDot == std::string::npos ? std::string::npos : lastDot - (lastSlash == std::string::npos ? 0 : lastSlash + 1)
        );

        // Build texture ID mapping using file path as prefix
        std::unordered_map<std::string, std::string> textureIdMapping;

        std::cout << "=== GLTF Import ===" << std::endl;
        std::cout << "File: " << filePath << std::endl;
        std::cout << "Base name: " << baseName << std::endl;
        std::cout << "Current scene has " << s->embeddedTextures().size() << " embedded textures" << std::endl;
        std::cout << "Loading scene has " << loadedScene->embeddedTextures().size() << " embedded textures" << std::endl;

        for (const auto& [oldId, tex] : loadedScene->embeddedTextures()) {
            // Create unique ID based on filename: "avocado/textures/embedded:0"
            std::string newId = baseName + "/textures/" + oldId;
            textureIdMapping[oldId] = newId;
            std::cout << "Texture ID mapping: " << oldId << " -> " << newId << std::endl;
        }

        // Copy loaded scene into existing scene (adding to current actors)
        // First pass: Copy all actors and build UID mapping
        std::unordered_map<size_t, size_t> uidMapping; // old UID -> new UID
        for (const auto& actor : loadedScene->actors()) {
            if (!actor) continue;  // Skip null actors
            auto* newActor = s->createActor();
            newActor->setName(actor->name());
            newActor->setTransform(actor->transform());
            for (const auto& instance : actor->instances()) {
                // Make a copy of the instance
                auto instanceCopy = instance;

                // Get the material and create a modified copy with remapped texture IDs
                auto materialCopy = instance.material();

                // Remap texture references by checking each slot
                if (auto albedoTex = materialCopy.getTexture(tracey::TEXTURE_ALBEDO)) {
                    if (textureIdMapping.count(*albedoTex)) {
                        std::string newId = textureIdMapping[*albedoTex];
                        std::cout << "Remapping albedo texture: " << *albedoTex << " -> " << newId << std::endl;
                        materialCopy.setTexture(tracey::TEXTURE_ALBEDO, newId);
                    }
                }
                if (auto normalTex = materialCopy.getTexture(tracey::TEXTURE_NORMAL)) {
                    if (textureIdMapping.count(*normalTex)) {
                        std::string newId = textureIdMapping[*normalTex];
                        std::cout << "Remapping normal texture: " << *normalTex << " -> " << newId << std::endl;
                        materialCopy.setTexture(tracey::TEXTURE_NORMAL, newId);
                    }
                }
                if (auto mrTex = materialCopy.getTexture(tracey::TEXTURE_METALLIC_ROUGHNESS)) {
                    if (textureIdMapping.count(*mrTex)) {
                        std::string newId = textureIdMapping[*mrTex];
                        std::cout << "Remapping MR texture: " << *mrTex << " -> " << newId << std::endl;
                        materialCopy.setTexture(tracey::TEXTURE_METALLIC_ROUGHNESS, newId);
                    }
                }
                if (auto emissiveTex = materialCopy.getTexture(tracey::TEXTURE_EMISSIVE)) {
                    if (textureIdMapping.count(*emissiveTex)) {
                        std::string newId = textureIdMapping[*emissiveTex];
                        std::cout << "Remapping emissive texture: " << *emissiveTex << " -> " << newId << std::endl;
                        materialCopy.setTexture(tracey::TEXTURE_EMISSIVE, newId);
                    }
                }
                if (auto occlusionTex = materialCopy.getTexture(tracey::TEXTURE_OCCLUSION)) {
                    if (textureIdMapping.count(*occlusionTex)) {
                        std::string newId = textureIdMapping[*occlusionTex];
                        std::cout << "Remapping occlusion texture: " << *occlusionTex << " -> " << newId << std::endl;
                        materialCopy.setTexture(tracey::TEXTURE_OCCLUSION, newId);
                    }
                }

                // Explicitly set the modified material back on the instance
                instanceCopy.setMaterial(materialCopy);
                newActor->addInstance(instanceCopy);
            }
            // Store UID mapping
            uidMapping[actor->getUid()] = newActor->getUid();
        }

        // Second pass: Copy parent-child relationships
        for (const auto& actor : loadedScene->actors()) {
            if (!actor) continue;  // Skip null actors
            size_t newActorUid = uidMapping[actor->getUid()];
            auto* newActor = s->getActor(newActorUid);
            if (!newActor) continue;

            // Copy children relationships
            for (size_t oldChildUid : actor->children()) {
                if (uidMapping.count(oldChildUid)) {
                    size_t newChildUid = uidMapping[oldChildUid];
                    auto* childActor = s->getActor(newChildUid);
                    if (childActor) {
                        newActor->addChild(childActor);
                    }
                }
            }
        }

        // If multiple top-level actors were imported, create a parent to group them
        std::vector<size_t> topLevelActors;
        for (const auto& [oldUid, newUid] : uidMapping) {
            auto* actor = s->getActor(newUid);
            if (!actor) continue;

            // Check if this actor has a parent
            bool hasParent = false;
            for (const auto& otherActor : s->actors()) {
                if (!otherActor || otherActor->getUid() == newUid) continue;
                for (size_t childUid : otherActor->children()) {
                    if (childUid == newUid) {
                        hasParent = true;
                        break;
                    }
                }
                if (hasParent) break;
            }

            if (!hasParent) {
                topLevelActors.push_back(newUid);
            }
        }

        // If we have multiple top-level actors, create a parent group
        if (topLevelActors.size() > 1) {
            auto* parentActor = s->createActor();
            parentActor->setName(baseName);  // Name it after the file

            // Make all top-level actors children of this parent
            for (size_t actorUid : topLevelActors) {
                auto* actor = s->getActor(actorUid);
                if (actor) {
                    parentActor->addChild(actor);
                }
            }

            std::cout << "Created parent actor '" << baseName << "' for "
                      << topLevelActors.size() << " top-level actors" << std::endl;
        }

        // Move all objects (meshes, materials, textures)
        // Note: SceneObject is no longer copyable due to AttributeSet containing unique_ptrs
        // We need to reconstruct the object or implement a clone() method
        // For now, we'll create new objects with the same data
        for (const auto& [name, obj] : loadedScene->objects()) {
            // Create new object and copy the basic geometry data
            tracey::SceneObject newObj(name);
            newObj.setPositions(obj->positions());
            newObj.setIndices(obj->indices());
            newObj.setNormals(obj->normals());
            newObj.setUvs(obj->uvs());
            // TODO: Clone attributes when needed
            s->addObject(name, std::move(newObj));
        }

        // Copy camera if present (optional - might want to skip this for additive load)
        if (loadedScene->hasCamera()) {
            s->setCamera(loadedScene->camera());
        }

        // Copy embedded textures with remapped IDs to avoid collisions
        std::cout << "Adding " << loadedScene->embeddedTextures().size() << " embedded textures..." << std::endl;
        for (const auto& [id, tex] : loadedScene->embeddedTextures()) {
            std::string newId = textureIdMapping[id];  // Use remapped ID

            // Skip if texture already exists (e.g., reimporting same file)
            if (s->hasEmbeddedTexture(newId)) {
                std::cout << "Texture already exists, skipping: " << newId << std::endl;
                continue;
            }

            tracey::EmbeddedTexture texCopy = tex;
            std::cout << "Adding embedded texture: " << id << " -> " << newId << std::endl;
            s->addEmbeddedTexture(newId, std::move(texCopy));
        }
        std::cout << "Scene now has " << s->embeddedTextures().size() << " total embedded textures" << std::endl;

        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("GLTF loading failed: ") + e.what());
        return TRACEY_ERROR_FILE_NOT_FOUND;
    } catch (...) {
        setError("GLTF loading failed: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_load_gltf(
    TraceyScene* scene,
    const char* filePath)
{
    // Call the new function with null projectRoot for backward compatibility
    return tracey_scene_load_gltf_with_project(scene, filePath, nullptr);
}

uint32_t tracey_scene_get_actor_count(TraceyScene* scene)
{
    if (!scene) return 0;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        return static_cast<uint32_t>(s->actors().size());
    } catch (...) {
        return 0;
    }
}

uint32_t tracey_scene_get_actor_uids(
    TraceyScene* scene,
    uint64_t* outUids,
    uint32_t maxCount)
{
    if (!scene || !outUids || maxCount == 0) return 0;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        const auto& actors = s->actors();

        uint32_t count = 0;
        for (const auto& actor : actors) {
            if (count >= maxCount) break;
            if (actor) {  // Check for null - actors may have been removed
                outUids[count++] = actor->getUid();
            }
        }

        return count;
    } catch (...) {
        return 0;
    }
}

// ============================================================================
// Scene Query Functions
// ============================================================================

const char* tracey_scene_get_actor_name(
    TraceyScene* scene,
    uint64_t actorUid)
{
    if (!scene) return nullptr;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) return nullptr;
        return actor->name().c_str();
    } catch (...) {
        return nullptr;
    }
}

uint32_t tracey_scene_get_actor_children(
    TraceyScene* scene,
    uint64_t actorUid,
    uint64_t* outUids,
    uint32_t maxCount)
{
    if (!scene || !outUids || maxCount == 0) return 0;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) return 0;

        const auto children = actor->children();
        uint32_t count = 0;
        for (size_t childUid : children) {
            if (count >= maxCount) break;
            outUids[count++] = childUid;
        }
        return count;
    } catch (...) {
        return 0;
    }
}

uint32_t tracey_scene_get_actor_instance_count(
    TraceyScene* scene,
    uint64_t actorUid)
{
    if (!scene) return 0;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) return 0;
        return static_cast<uint32_t>(actor->instances().size());
    } catch (...) {
        return 0;
    }
}

TraceyResult tracey_scene_get_actor_instance(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    TraceyInstanceInfo* outInfo)
{
    if (!scene || !outInfo) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) {
            setError("Actor not found");
            return TRACEY_ERROR_NOT_FOUND;
        }

        const auto& instances = actor->instances();
        if (instanceIndex >= instances.size()) {
            setError("Instance index out of range");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        const auto& inst = instances[instanceIndex];
        outInfo->objectRef = inst.objectRef().c_str();
        outInfo->shaderId = inst.material().shaderId().c_str();
        outInfo->hasLocalTransform = inst.hasLocalTransform();
        if (inst.hasLocalTransform()) {
            outInfo->localTransform = fromTransform(inst.localTransform().value());
        }

        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get instance: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to get instance: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

// ============================================================================
// Material Editing Functions
// ============================================================================

uint32_t tracey_scene_get_instance_material_property_count(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex)
{
    if (!scene) return 0;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) return 0;

        const auto& instances = actor->instances();
        if (instanceIndex >= instances.size()) return 0;

        return static_cast<uint32_t>(instances[instanceIndex].material().properties().size());
    } catch (...) {
        return 0;
    }
}

TraceyResult tracey_scene_get_instance_material_property_by_name(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    const char* propertyName,
    TraceyMaterialProperty* outProperty)
{
    if (!scene || !propertyName || !outProperty) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) {
            setError("Actor not found");
            return TRACEY_ERROR_NOT_FOUND;
        }

        const auto& instances = actor->instances();
        if (instanceIndex >= instances.size()) {
            setError("Instance index out of range");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        const auto& mat = instances[instanceIndex].material();
        const auto* prop = mat.getProperty(propertyName);
        if (!prop) {
            setError("Property not found");
            return TRACEY_ERROR_NOT_FOUND;
        }

        // Static storage for property name (valid until next call)
        static thread_local std::string propNameStorage;
        propNameStorage = propertyName;
        outProperty->name = propNameStorage.c_str();

        // Convert variant to TraceyMaterialProperty
        std::visit([outProperty](const auto& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, float>) {
                outProperty->type = TRACEY_MATERIAL_PROP_FLOAT;
                outProperty->value.floatValue = val;
            } else if constexpr (std::is_same_v<T, tracey::Vec2>) {
                outProperty->type = TRACEY_MATERIAL_PROP_VEC2;
                outProperty->value.vec2Value = TraceyVec2{val.x, val.y};
            } else if constexpr (std::is_same_v<T, tracey::Vec3>) {
                outProperty->type = TRACEY_MATERIAL_PROP_VEC3;
                outProperty->value.vec3Value = TraceyVec3{val.x, val.y, val.z};
            } else if constexpr (std::is_same_v<T, tracey::Vec4>) {
                outProperty->type = TRACEY_MATERIAL_PROP_VEC4;
                outProperty->value.vec4Value = TraceyVec4{val.x, val.y, val.z, val.w};
            } else if constexpr (std::is_same_v<T, int>) {
                outProperty->type = TRACEY_MATERIAL_PROP_INT;
                outProperty->value.intValue = val;
            } else if constexpr (std::is_same_v<T, std::string>) {
                outProperty->type = TRACEY_MATERIAL_PROP_TEXTURE;
                // Store in thread-local for lifetime
                static thread_local std::string textureStorage;
                textureStorage = val;
                outProperty->value.textureValue = textureStorage.c_str();
            }
        }, *prop);

        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get material property: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to get material property: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_set_instance_material_float(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    const char* propertyName,
    float value)
{
    if (!scene || !propertyName) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) {
            setError("Actor not found");
            return TRACEY_ERROR_NOT_FOUND;
        }

        auto& instances = actor->instances();
        if (instanceIndex >= instances.size()) {
            setError("Instance index out of range");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        instances[instanceIndex].material().setFloat(propertyName, value);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set material property: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to set material property: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_set_instance_material_vec3(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    const char* propertyName,
    TraceyVec3 value)
{
    if (!scene || !propertyName) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) {
            setError("Actor not found");
            return TRACEY_ERROR_NOT_FOUND;
        }

        auto& instances = actor->instances();
        if (instanceIndex >= instances.size()) {
            setError("Instance index out of range");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        instances[instanceIndex].material().setVec3(propertyName, toVec3(value));
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set material property: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to set material property: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_set_instance_material_vec4(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    const char* propertyName,
    TraceyVec4 value)
{
    if (!scene || !propertyName) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) {
            setError("Actor not found");
            return TRACEY_ERROR_NOT_FOUND;
        }

        auto& instances = actor->instances();
        if (instanceIndex >= instances.size()) {
            setError("Instance index out of range");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        instances[instanceIndex].material().setVec4(propertyName, tracey::Vec4(value.x, value.y, value.z, value.w));
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set material property: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to set material property: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_set_instance_material_texture(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex,
    const char* propertyName,
    const char* texturePath)
{
    if (!scene || !propertyName || !texturePath) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) {
            setError("Actor not found");
            return TRACEY_ERROR_NOT_FOUND;
        }

        auto& instances = actor->instances();
        if (instanceIndex >= instances.size()) {
            setError("Instance index out of range");
            return TRACEY_ERROR_INVALID_PARAMETER;
        }

        instances[instanceIndex].material().setTexture(propertyName, texturePath);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set material texture: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to set material texture: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

const char* tracey_scene_get_instance_material_shader_id(
    TraceyScene* scene,
    uint64_t actorUid,
    uint32_t instanceIndex)
{
    if (!scene) return nullptr;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* actor = s->getActor(actorUid);
        if (!actor) return nullptr;

        const auto& instances = actor->instances();
        if (instanceIndex >= instances.size()) return nullptr;

        return instances[instanceIndex].material().shaderId().c_str();
    } catch (...) {
        return nullptr;
    }
}

uint32_t tracey_scene_get_mesh_count(TraceyScene* scene)
{
    if (!scene) return 0;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        return static_cast<uint32_t>(s->objects().size());
    } catch (...) {
        return 0;
    }
}

uint32_t tracey_scene_get_mesh_names(
    TraceyScene* scene,
    const char** outNames,
    uint32_t maxCount)
{
    if (!scene || !outNames || maxCount == 0) return 0;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        const auto& objects = s->objects();

        uint32_t count = 0;
        for (const auto& [name, obj] : objects) {
            if (count >= maxCount) break;
            outNames[count++] = name.c_str();
        }
        return count;
    } catch (...) {
        return 0;
    }
}

TraceyResult tracey_scene_get_mesh_info(
    TraceyScene* scene,
    const char* name,
    TraceyMeshInfo* outInfo)
{
    if (!scene || !name || !outInfo) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        const auto* obj = s->getObject(name);
        if (!obj) {
            setError("Mesh not found");
            return TRACEY_ERROR_NOT_FOUND;
        }

        outInfo->name = obj->name().c_str();
        outInfo->vertexCount = static_cast<uint32_t>(obj->vertexCount());
        outInfo->triangleCount = static_cast<uint32_t>(obj->triangleCount());
        outInfo->hasIndices = obj->hasIndices();
        outInfo->hasNormals = obj->hasNormals();
        outInfo->hasUvs = obj->hasUvs();

        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get mesh info: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to get mesh info: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

uint32_t tracey_scene_get_texture_count(TraceyScene* scene)
{
    if (!scene) return 0;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        return static_cast<uint32_t>(s->embeddedTextures().size());
    } catch (...) {
        return 0;
    }
}

uint32_t tracey_scene_get_texture_ids(
    TraceyScene* scene,
    const char** outIds,
    uint32_t maxCount)
{
    if (!scene || !outIds || maxCount == 0) return 0;

    try {
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        const auto& textures = s->embeddedTextures();

        uint32_t count = 0;
        for (const auto& [id, tex] : textures) {
            if (count >= maxCount) break;
            outIds[count++] = id.c_str();
        }
        return count;
    } catch (...) {
        return 0;
    }
}

TraceyResult tracey_scene_get_texture_info(
    TraceyScene* scene,
    const char* id,
    TraceyTextureInfo* outInfo)
{
    if (!scene || !id || !outInfo) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        const auto* tex = s->getEmbeddedTexture(id);
        if (!tex) {
            setError("Texture not found");
            return TRACEY_ERROR_NOT_FOUND;
        }

        // Store id in a static map to keep it alive
        static std::unordered_map<std::string, std::string> idStorage;
        idStorage[id] = id;
        outInfo->id = idStorage[id].c_str();
        outInfo->width = tex->width;
        outInfo->height = tex->height;
        outInfo->channels = tex->channels;
        outInfo->mimeType = tex->mimeType.c_str();

        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get texture info: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to get texture info: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

// ============================================================================
// Primitive Creation Functions
// ============================================================================

// Helper function to add a primitive and create an actor with instance
static uint64_t addPrimitiveToScene(tracey::Scene* scene, const char* name, tracey::SceneObject&& obj) {
    try {
        // Generate a unique object name based on provided name
        std::string objName = name ? name : "primitive";

        // Add the object to scene
        scene->addObject(objName, std::move(obj));

        // Create actor for this primitive
        auto* actor = scene->createActor();
        actor->setName(objName);

        // Create default material instance with all PBR properties
        tracey::MaterialInstance material("pbr");
        material.setAlbedo(tracey::Vec3(0.8f, 0.8f, 0.8f));
        material.setMetallic(0.0f);
        material.setRoughness(0.5f);
        material.setEmission(tracey::Vec3(0.0f, 0.0f, 0.0f));
        // Clearcoat layer (default: disabled)
        material.setClearcoat(0.0f);
        material.setClearcoatRoughness(0.1f);
        // Sheen layer (default: disabled)
        material.setSheenColor(tracey::Vec3(0.0f, 0.0f, 0.0f));
        material.setSheenRoughness(0.5f);

        // Create scene instance referencing the object
        tracey::SceneInstance instance(objName, material);
        actor->addInstance(instance);

        return actor->getUid();
    } catch (...) {
        return UINT64_MAX;
    }
}

uint64_t tracey_scene_add_cube(
    TraceyScene* scene,
    const char* name,
    float size)
{
    if (!scene) {
        setError("Scene pointer is null");
        return UINT64_MAX;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createCube(size > 0 ? size : 1.0f);
        return addPrimitiveToScene(s, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add cube: ") + e.what());
        return UINT64_MAX;
    } catch (...) {
        setError("Failed to add cube: unknown error");
        return UINT64_MAX;
    }
}

uint64_t tracey_scene_add_sphere(
    TraceyScene* scene,
    const char* name,
    float radius,
    uint32_t segments,
    uint32_t rings)
{
    if (!scene) {
        setError("Scene pointer is null");
        return UINT64_MAX;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createSphere(
            radius > 0 ? radius : 1.0f,
            segments > 0 ? segments : 16,
            rings > 0 ? rings : 16
        );
        return addPrimitiveToScene(s, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add sphere: ") + e.what());
        return UINT64_MAX;
    } catch (...) {
        setError("Failed to add sphere: unknown error");
        return UINT64_MAX;
    }
}

uint64_t tracey_scene_add_torus(
    TraceyScene* scene,
    const char* name,
    float majorRadius,
    float minorRadius,
    uint32_t majorSegments,
    uint32_t minorSegments)
{
    if (!scene) {
        setError("Scene pointer is null");
        return UINT64_MAX;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createTorus(
            majorRadius > 0 ? majorRadius : 1.0f,
            minorRadius > 0 ? minorRadius : 0.3f,
            majorSegments > 0 ? majorSegments : 32,
            minorSegments > 0 ? minorSegments : 16
        );
        return addPrimitiveToScene(s, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add torus: ") + e.what());
        return UINT64_MAX;
    } catch (...) {
        setError("Failed to add torus: unknown error");
        return UINT64_MAX;
    }
}

uint64_t tracey_scene_add_plane(
    TraceyScene* scene,
    const char* name,
    float width,
    float depth)
{
    if (!scene) {
        setError("Scene pointer is null");
        return UINT64_MAX;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createPlane(
            width > 0 ? width : 1.0f,
            depth > 0 ? depth : 1.0f
        );
        return addPrimitiveToScene(s, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add plane: ") + e.what());
        return UINT64_MAX;
    } catch (...) {
        setError("Failed to add plane: unknown error");
        return UINT64_MAX;
    }
}

uint64_t tracey_scene_add_cylinder(
    TraceyScene* scene,
    const char* name,
    float radius,
    float height,
    uint32_t segments)
{
    if (!scene) {
        setError("Scene pointer is null");
        return UINT64_MAX;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createCylinder(
            radius > 0 ? radius : 0.5f,
            height > 0 ? height : 1.0f,
            segments > 0 ? segments : 32
        );
        return addPrimitiveToScene(s, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add cylinder: ") + e.what());
        return UINT64_MAX;
    } catch (...) {
        setError("Failed to add cylinder: unknown error");
        return UINT64_MAX;
    }
}

uint64_t tracey_scene_add_cone(
    TraceyScene* scene,
    const char* name,
    float radius,
    float height,
    uint32_t segments)
{
    if (!scene) {
        setError("Scene pointer is null");
        return UINT64_MAX;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createCone(
            radius > 0 ? radius : 0.5f,
            height > 0 ? height : 1.0f,
            segments > 0 ? segments : 32
        );
        return addPrimitiveToScene(s, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add cone: ") + e.what());
        return UINT64_MAX;
    } catch (...) {
        setError("Failed to add cone: unknown error");
        return UINT64_MAX;
    }
}

// ============================================================================
// Primitive Creation with Pre-assigned Actor ID
// ============================================================================

// Helper function to add a primitive to an actor with a specific UID
// Creates the actor if it doesn't exist (for queue-based creation from Rust)
static TraceyResult addPrimitiveToActorWithId(tracey::Scene* scene, uint64_t actorUid, const char* name, tracey::SceneObject&& obj) {
    try {
        // Try to find existing actor, or create one with the specified UID
        auto* actor = scene->getActor(actorUid);
        if (!actor) {
            // Actor doesn't exist in C++ scene - create it with the specified UID
            actor = scene->createActorWithUid(actorUid);
            if (!actor) {
                setError("Failed to create actor with specified UID");
                return TRACEY_ERROR_UNKNOWN;
            }
        }

        // Generate a unique object name based on provided name
        std::string objName = name ? name : "primitive";

        // Add the object to scene
        scene->addObject(objName, std::move(obj));

        // Update actor name to match
        actor->setName(objName);

        // Create default material instance with all PBR properties
        tracey::MaterialInstance material("pbr");
        material.setAlbedo(tracey::Vec3(0.8f, 0.8f, 0.8f));
        material.setMetallic(0.0f);
        material.setRoughness(0.5f);
        material.setEmission(tracey::Vec3(0.0f, 0.0f, 0.0f));
        // Clearcoat layer (default: disabled)
        material.setClearcoat(0.0f);
        material.setClearcoatRoughness(0.1f);
        // Sheen layer (default: disabled)
        material.setSheenColor(tracey::Vec3(0.0f, 0.0f, 0.0f));
        material.setSheenRoughness(0.5f);

        // Create scene instance referencing the object
        tracey::SceneInstance instance(objName, material);
        actor->addInstance(instance);

        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to add primitive: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to add primitive: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_add_cube_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float size)
{
    if (!scene) {
        setError("Scene pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createCube(size > 0 ? size : 1.0f);
        return addPrimitiveToActorWithId(s, actorUid, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add cube: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to add cube: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_add_sphere_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float radius,
    uint32_t segments,
    uint32_t rings)
{
    if (!scene) {
        setError("Scene pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createSphere(
            radius > 0 ? radius : 1.0f,
            segments > 0 ? segments : 16,
            rings > 0 ? rings : 16
        );
        return addPrimitiveToActorWithId(s, actorUid, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add sphere: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to add sphere: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_add_torus_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float majorRadius,
    float minorRadius,
    uint32_t majorSegments,
    uint32_t minorSegments)
{
    if (!scene) {
        setError("Scene pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createTorus(
            majorRadius > 0 ? majorRadius : 1.0f,
            minorRadius > 0 ? minorRadius : 0.3f,
            majorSegments > 0 ? majorSegments : 32,
            minorSegments > 0 ? minorSegments : 16
        );
        return addPrimitiveToActorWithId(s, actorUid, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add torus: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to add torus: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_add_plane_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float width,
    float depth)
{
    if (!scene) {
        setError("Scene pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createPlane(
            width > 0 ? width : 1.0f,
            depth > 0 ? depth : 1.0f
        );
        return addPrimitiveToActorWithId(s, actorUid, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add plane: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to add plane: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_add_cylinder_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float radius,
    float height,
    uint32_t segments)
{
    if (!scene) {
        setError("Scene pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createCylinder(
            radius > 0 ? radius : 0.5f,
            height > 0 ? height : 1.0f,
            segments > 0 ? segments : 32
        );
        return addPrimitiveToActorWithId(s, actorUid, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add cylinder: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to add cylinder: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_scene_add_cone_with_id(
    TraceyScene* scene,
    uint64_t actorUid,
    const char* name,
    float radius,
    float height,
    uint32_t segments)
{
    if (!scene) {
        setError("Scene pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto obj = tracey::SceneObject::createCone(
            radius > 0 ? radius : 0.5f,
            height > 0 ? height : 1.0f,
            segments > 0 ? segments : 32
        );
        return addPrimitiveToActorWithId(s, actorUid, name, std::move(obj));
    } catch (const std::exception& e) {
        setError(std::string("Failed to add cone: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to add cone: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

// ============================================================================
// Scene Compilation
// ============================================================================

TraceyCompiledScene* tracey_compile_scene(
    TraceyDevice* device,
    TraceyScene* scene)
{
    if (!device || !scene) {
        setError("Null pointer parameter");
        return nullptr;
    }

    try {
        clearError();
        auto* dev = reinterpret_cast<tracey::Device*>(device);
        auto* s = reinterpret_cast<tracey::Scene*>(scene);

        // Compile scene
        auto compiled = new tracey::SceneCompiler::CompiledScene(
            tracey::SceneCompiler::compile(dev, *s)
        );

        return reinterpret_cast<TraceyCompiledScene*>(compiled);
    } catch (const std::exception& e) {
        setError(std::string("Scene compilation failed: ") + e.what());
        return nullptr;
    } catch (...) {
        setError("Scene compilation failed: unknown error");
        return nullptr;
    }
}

void tracey_destroy_compiled_scene(TraceyCompiledScene* compiledScene)
{
    if (compiledScene) {
        delete reinterpret_cast<tracey::SceneCompiler::CompiledScene*>(compiledScene);
    }
}

int tracey_update_scene_transforms(
    TraceyDevice* device,
    TraceyScene* scene,
    TraceyCompiledScene* compiledScene)
{
    if (!device || !scene || !compiledScene) {
        setError("Null pointer parameter");
        return -1;
    }

    try {
        clearError();
        auto* dev = reinterpret_cast<tracey::Device*>(device);
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* compiled = reinterpret_cast<tracey::SceneCompiler::CompiledScene*>(compiledScene);

        // Update transforms only
        tracey::SceneCompiler::updateTransforms(dev, *s, *compiled);
        return 0;
    } catch (const std::exception& e) {
        setError(std::string("Transform update failed: ") + e.what());
        return -1;
    } catch (...) {
        setError("Transform update failed: unknown error");
        return -1;
    }
}

int tracey_update_scene_materials(
    TraceyDevice* device,
    TraceyScene* scene,
    TraceyCompiledScene* compiledScene)
{
    if (!device || !scene || !compiledScene) {
        setError("Null pointer parameter");
        return -1;
    }

    try {
        clearError();
        auto* dev = reinterpret_cast<tracey::Device*>(device);
        auto* s = reinterpret_cast<tracey::Scene*>(scene);
        auto* compiled = reinterpret_cast<tracey::SceneCompiler::CompiledScene*>(compiledScene);

        // Update materials only
        tracey::SceneCompiler::updateMaterials(dev, *s, *compiled);
        return 0;
    } catch (const std::exception& e) {
        setError(std::string("Material update failed: ") + e.what());
        return -1;
    } catch (...) {
        setError("Material update failed: unknown error");
        return -1;
    }
}

// ============================================================================
// Path Tracer
// ============================================================================

TraceyPathTracer* tracey_path_tracer_create(
    TraceyDevice* device,
    const TraceyPathTracerConfig* config)
{
    if (!device || !config) {
        setError("Null pointer parameter");
        return nullptr;
    }

    try {
        clearError();
        auto* dev = reinterpret_cast<tracey::Device*>(device);

        tracey::PathTracerConfig cppConfig;
        cppConfig.width = config->width;
        cppConfig.height = config->height;
        cppConfig.rayGenShader = config->rayGenShaderPath ? config->rayGenShaderPath : "";
        cppConfig.hitShader = config->hitShaderPath ? config->hitShaderPath : "";
        cppConfig.missShader = config->missShaderPath ? config->missShaderPath : "";
        if (config->resolveShaderPath) {
            cppConfig.resolveShader = config->resolveShaderPath;
        }
        cppConfig.hdrOutput = config->hdrOutput;
        cppConfig.samplesPerFrame = config->samplesPerFrame > 0 ? config->samplesPerFrame : 16;
        cppConfig.maxBounces = config->maxBounces > 0 ? config->maxBounces : 8;

        auto* tracer = new tracey::PathTracer(dev, cppConfig);
        return reinterpret_cast<TraceyPathTracer*>(tracer);
    } catch (const std::exception& e) {
        setError(std::string("Path tracer creation failed: ") + e.what());
        return nullptr;
    } catch (...) {
        setError("Path tracer creation failed: unknown error");
        return nullptr;
    }
}

void tracey_path_tracer_destroy(TraceyPathTracer* pathTracer)
{
    if (pathTracer) {
        delete reinterpret_cast<tracey::PathTracer*>(pathTracer);
    }
}

double tracey_path_tracer_render(
    TraceyPathTracer* pathTracer,
    TraceyCompiledScene* compiledScene,
    const TraceyCamera* camera,
    bool clearAccumulation)
{
    if (!pathTracer || !compiledScene || !camera) {
        setError("Null pointer parameter");
        return -1.0;
    }

    try {
        clearError();
        auto* tracer = reinterpret_cast<tracey::PathTracer*>(pathTracer);
        auto* scene = reinterpret_cast<tracey::SceneCompiler::CompiledScene*>(compiledScene);

        double renderTime = tracer->render(*scene, toCamera(*camera), clearAccumulation);
        return renderTime;
    } catch (const std::exception& e) {
        setError(std::string("Rendering failed: ") + e.what());
        return -1.0;
    } catch (...) {
        setError("Rendering failed: unknown error");
        return -1.0;
    }
}

size_t tracey_path_tracer_readback(
    TraceyPathTracer* pathTracer,
    void* outBuffer,
    size_t bufferSize)
{
    if (!pathTracer || !outBuffer || bufferSize == 0) {
        setError("Invalid parameter");
        return 0;
    }

    try {
        clearError();
        auto* tracer = reinterpret_cast<tracey::PathTracer*>(pathTracer);
        return tracer->readback(outBuffer);
    } catch (const std::exception& e) {
        setError(std::string("Readback failed: ") + e.what());
        return 0;
    } catch (...) {
        setError("Readback failed: unknown error");
        return 0;
    }
}

TraceyResult tracey_path_tracer_get_resolution(
    TraceyPathTracer* pathTracer,
    uint32_t* outWidth,
    uint32_t* outHeight)
{
    if (!pathTracer || !outWidth || !outHeight) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* tracer = reinterpret_cast<tracey::PathTracer*>(pathTracer);
        *outWidth = tracer->width();
        *outHeight = tracer->height();
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get resolution: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to get resolution: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

uint32_t tracey_path_tracer_get_sample_count(TraceyPathTracer* pathTracer)
{
    if (!pathTracer) return 0;

    try {
        auto* tracer = reinterpret_cast<tracey::PathTracer*>(pathTracer);
        return tracer->sampleCount();
    } catch (...) {
        return 0;
    }
}

uint32_t tracey_path_tracer_get_samples_per_frame(TraceyPathTracer* pathTracer)
{
    if (!pathTracer) return 0;

    try {
        auto* tracer = reinterpret_cast<tracey::PathTracer*>(pathTracer);
        return tracer->samplesPerFrame();
    } catch (...) {
        return 0;
    }
}

TraceyResult tracey_path_tracer_set_samples_per_frame(TraceyPathTracer* pathTracer, uint32_t samples)
{
    if (!pathTracer) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* tracer = reinterpret_cast<tracey::PathTracer*>(pathTracer);
        tracer->setSamplesPerFrame(samples);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set samples per frame: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to set samples per frame: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

uint32_t tracey_path_tracer_get_max_bounces(TraceyPathTracer* pathTracer)
{
    if (!pathTracer) return 0;

    try {
        auto* tracer = reinterpret_cast<tracey::PathTracer*>(pathTracer);
        return tracer->maxBounces();
    } catch (...) {
        return 0;
    }
}

TraceyResult tracey_path_tracer_set_max_bounces(TraceyPathTracer* pathTracer, uint32_t bounces)
{
    if (!pathTracer) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* tracer = reinterpret_cast<tracey::PathTracer*>(pathTracer);
        tracer->setMaxBounces(bounces);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set max bounces: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to set max bounces: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

// ============================================================================
// Rasterizer
// ============================================================================

TraceyRasterizer* tracey_rasterizer_create(
    TraceyDevice* device,
    const TraceyRasterizerConfig* config)
{
    if (!device || !config) {
        setError("Null pointer parameter");
        return nullptr;
    }

    try {
        clearError();
        auto* dev = reinterpret_cast<tracey::Device*>(device);

        tracey::RasterizerConfig cppConfig;
        cppConfig.width = config->width;
        cppConfig.height = config->height;
        cppConfig.vertexShader = config->vertexShaderPath ? config->vertexShaderPath : "";
        cppConfig.fragmentShader = config->fragmentShaderPath ? config->fragmentShaderPath : "";
        cppConfig.useDepthBuffer = config->useDepthBuffer;
        cppConfig.depthTestEnable = config->depthTestEnable;
        cppConfig.cullBackFaces = config->cullBackFaces;
        cppConfig.alphaBlending = config->alphaBlending;

        auto* rasterizer = new tracey::Rasterizer(dev, cppConfig);
        return reinterpret_cast<TraceyRasterizer*>(rasterizer);
    } catch (const std::exception& e) {
        setError(std::string("Rasterizer creation failed: ") + e.what());
        return nullptr;
    } catch (...) {
        setError("Rasterizer creation failed: unknown error");
        return nullptr;
    }
}

void tracey_rasterizer_destroy(TraceyRasterizer* rasterizer)
{
    if (rasterizer) {
        delete reinterpret_cast<tracey::Rasterizer*>(rasterizer);
    }
}

double tracey_rasterizer_render(
    TraceyRasterizer* rasterizer,
    TraceyCompiledScene* compiledScene,
    const TraceyCamera* camera)
{
    if (!rasterizer || !compiledScene || !camera) {
        setError("Null pointer parameter");
        return -1.0;
    }

    try {
        clearError();
        auto* rast = reinterpret_cast<tracey::Rasterizer*>(rasterizer);
        auto* scene = reinterpret_cast<tracey::SceneCompiler::CompiledScene*>(compiledScene);

        double renderTime = rast->render(*scene, toCamera(*camera));
        return renderTime;
    } catch (const std::exception& e) {
        setError(std::string("Rendering failed: ") + e.what());
        return -1.0;
    } catch (...) {
        setError("Rendering failed: unknown error");
        return -1.0;
    }
}

size_t tracey_rasterizer_readback(
    TraceyRasterizer* rasterizer,
    void* outBuffer,
    size_t bufferSize)
{
    if (!rasterizer || !outBuffer || bufferSize == 0) {
        setError("Invalid parameter");
        return 0;
    }

    try {
        clearError();
        auto* rast = reinterpret_cast<tracey::Rasterizer*>(rasterizer);
        return rast->readback(outBuffer);
    } catch (const std::exception& e) {
        setError(std::string("Readback failed: ") + e.what());
        return 0;
    } catch (...) {
        setError("Readback failed: unknown error");
        return 0;
    }
}

TraceyResult tracey_rasterizer_get_resolution(
    TraceyRasterizer* rasterizer,
    uint32_t* outWidth,
    uint32_t* outHeight)
{
    if (!rasterizer || !outWidth || !outHeight) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* rast = reinterpret_cast<tracey::Rasterizer*>(rasterizer);
        *outWidth = rast->width();
        *outHeight = rast->height();
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get resolution: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Failed to get resolution: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

// ============================================================================
// Error Handling
// ============================================================================

const char* tracey_get_last_error(void)
{
    return g_lastError.empty() ? nullptr : g_lastError.c_str();
}

void tracey_clear_last_error(void)
{
    clearError();
}

// ============================================================================
// Utility Functions
// ============================================================================

const char* tracey_get_version(void)
{
    return "1.0.0";
}

TraceyTransform tracey_transform_identity(void)
{
    TraceyTransform t;
    t.position = TraceyVec3{0.0f, 0.0f, 0.0f};
    t.rotation = TraceyQuat{1.0f, 0.0f, 0.0f, 0.0f};  // identity quaternion
    t.scale = TraceyVec3{1.0f, 1.0f, 1.0f};
    return t;
}

TraceyCamera tracey_camera_default(void)
{
    TraceyCamera c;
    c.position = TraceyVec3{0.0f, 0.0f, 0.0f};
    c.rotation = TraceyQuat{1.0f, 0.0f, 0.0f, 0.0f};  // identity quaternion
    c.fov = 45.0f;
    c.nearPlane = 0.01f;
    c.farPlane = 1000.0f;
    c.aspectRatio = 1.0f;
    return c;
}

// ============================================================================
// Native Window Presentation
// ============================================================================

TraceyPresenter* tracey_presenter_create(
    TraceyDevice* device,
    void* nativeWindowHandle,
    void* nativeDisplayHandle,
    const TraceyPresenterConfig* config)
{
    if (!device) {
        setError("Device pointer is null");
        return nullptr;
    }

    if (!nativeWindowHandle) {
        setError("Native window handle is null");
        return nullptr;
    }

    if (!config) {
        setError("Config pointer is null");
        return nullptr;
    }

    try {
        clearError();

        // Cast device to internal type
        auto* deviceImpl = reinterpret_cast<tracey::Device*>(device);

        // Get VulkanComputeDevice (only GPU devices support presentation)
        auto* vulkanDevice = dynamic_cast<tracey::VulkanComputeDevice*>(deviceImpl);
        if (!vulkanDevice) {
            setError("Device is not a Vulkan GPU device (presentation requires GPU)");
            return nullptr;
        }

        // Get VulkanContext
        tracey::VulkanContext& context = vulkanDevice->context();

        // Create Vulkan surface from native handle
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        std::string surfaceError = tracey::VulkanSurfaceFactory::createSurface(
            context.instance(),
            nativeWindowHandle,
            nativeDisplayHandle,
            &surface
        );

        if (!surfaceError.empty()) {
            setError("Failed to create Vulkan surface: " + surfaceError);
            return nullptr;
        }

        // Create presenter config
        tracey::PresenterConfig presenterConfig;
        presenterConfig.width = config->width;
        presenterConfig.height = config->height;
        presenterConfig.enableHDR = config->enableHDR;
        presenterConfig.desiredImageCount = config->desiredImageCount;

        // Create presenter
        auto* presenter = new tracey::VulkanPresenter(context, surface, presenterConfig);
        return reinterpret_cast<TraceyPresenter*>(presenter);

    } catch (const std::exception& e) {
        setError(std::string("Presenter creation failed: ") + e.what());
        return nullptr;
    } catch (...) {
        setError("Presenter creation failed: unknown error");
        return nullptr;
    }
}

void tracey_presenter_destroy(TraceyPresenter* presenter)
{
    if (presenter) {
        try {
            delete reinterpret_cast<tracey::VulkanPresenter*>(presenter);
        } catch (const std::exception& e) {
            // Log error but don't propagate exception across FFI boundary
            setError(std::string("Presenter destruction failed: ") + e.what());
        } catch (...) {
            setError("Presenter destruction failed: unknown error");
        }
    }
}

TraceyResult tracey_presenter_present_pathtracer(
    TraceyPresenter* presenter,
    TraceyPathTracer* pathTracer)
{
    if (!presenter) {
        setError("Presenter pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    if (!pathTracer) {
        setError("PathTracer pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    try {
        clearError();

        auto* presenterImpl = reinterpret_cast<tracey::VulkanPresenter*>(presenter);
        auto* pathTracerImpl = reinterpret_cast<tracey::PathTracer*>(pathTracer);

        // Get output image from path tracer
        auto* outputImage = pathTracerImpl->outputImage();
        if (!outputImage) {
            setError("PathTracer has no output image");
            return TRACEY_ERROR_INVALID_STATE;
        }

        // Cast to VulkanImage2D (safe because GPU devices create VulkanImage2D instances)
        auto* vulkanImage = dynamic_cast<tracey::VulkanImage2D*>(outputImage);
        if (!vulkanImage) {
            setError("Output image is not a Vulkan image (presentation requires GPU device)");
            return TRACEY_ERROR_INVALID_STATE;
        }

        // Present the image
        bool success = presenterImpl->present(vulkanImage, true);
        if (!success) {
            setError("Presentation failed (swapchain may need recreation)");
            return TRACEY_ERROR_PRESENTATION_FAILED;
        }

        return TRACEY_SUCCESS;

    } catch (const std::exception& e) {
        setError(std::string("Present failed: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Present failed: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_presenter_present_rasterizer(
    TraceyPresenter* presenter,
    TraceyRasterizer* rasterizer)
{
    if (!presenter) {
        setError("Presenter pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    if (!rasterizer) {
        setError("Rasterizer pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    try {
        clearError();

        auto* presenterImpl = reinterpret_cast<tracey::VulkanPresenter*>(presenter);
        auto* rasterizerImpl = reinterpret_cast<tracey::Rasterizer*>(rasterizer);

        // Get output image from rasterizer
        auto* outputImage = rasterizerImpl->outputImage();
        if (!outputImage) {
            setError("Rasterizer has no output image");
            return TRACEY_ERROR_INVALID_STATE;
        }

        // Cast to VulkanImage2D (safe because GPU devices create VulkanImage2D instances)
        auto* vulkanImage = dynamic_cast<tracey::VulkanImage2D*>(outputImage);
        if (!vulkanImage) {
            setError("Output image is not a Vulkan image (presentation requires GPU device)");
            return TRACEY_ERROR_INVALID_STATE;
        }

        // Present the image
        bool success = presenterImpl->present(vulkanImage, true);
        if (!success) {
            setError("Presentation failed (swapchain may need recreation)");
            return TRACEY_ERROR_PRESENTATION_FAILED;
        }

        return TRACEY_SUCCESS;

    } catch (const std::exception& e) {
        setError(std::string("Present failed: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Present failed: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_presenter_present_pathtracer_to_region(
    TraceyPresenter* presenter,
    TraceyPathTracer* pathTracer,
    const TraceyViewportBounds* bounds)
{
    if (!presenter) {
        setError("Presenter pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    if (!pathTracer) {
        setError("PathTracer pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    if (!bounds) {
        setError("ViewportBounds pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    try {
        clearError();

        auto* presenterImpl = reinterpret_cast<tracey::VulkanPresenter*>(presenter);
        auto* pathTracerImpl = reinterpret_cast<tracey::PathTracer*>(pathTracer);

        // Get output image from path tracer
        auto* outputImage = pathTracerImpl->outputImage();
        if (!outputImage) {
            setError("PathTracer has no output image");
            return TRACEY_ERROR_INVALID_STATE;
        }

        // Cast to VulkanImage2D
        auto* vulkanImage = dynamic_cast<tracey::VulkanImage2D*>(outputImage);
        if (!vulkanImage) {
            setError("Output image is not a Vulkan image (presentation requires GPU device)");
            return TRACEY_ERROR_INVALID_STATE;
        }

        // Present the image to specified region
        bool success = presenterImpl->presentToRegion(
            vulkanImage,
            bounds->x, bounds->y,
            bounds->width, bounds->height,
            true
        );

        if (!success) {
            setError("Presentation failed (swapchain may need recreation)");
            return TRACEY_ERROR_PRESENTATION_FAILED;
        }

        return TRACEY_SUCCESS;

    } catch (const std::exception& e) {
        setError(std::string("Present to region failed: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Present to region failed: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_presenter_present_rasterizer_to_region(
    TraceyPresenter* presenter,
    TraceyRasterizer* rasterizer,
    const TraceyViewportBounds* bounds)
{
    if (!presenter) {
        setError("Presenter pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    if (!rasterizer) {
        setError("Rasterizer pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    if (!bounds) {
        setError("ViewportBounds pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    try {
        clearError();

        auto* presenterImpl = reinterpret_cast<tracey::VulkanPresenter*>(presenter);
        auto* rasterizerImpl = reinterpret_cast<tracey::Rasterizer*>(rasterizer);

        // Get output image from rasterizer
        auto* outputImage = rasterizerImpl->outputImage();
        if (!outputImage) {
            setError("Rasterizer has no output image");
            return TRACEY_ERROR_INVALID_STATE;
        }

        // Cast to VulkanImage2D
        auto* vulkanImage = dynamic_cast<tracey::VulkanImage2D*>(outputImage);
        if (!vulkanImage) {
            setError("Output image is not a Vulkan image (presentation requires GPU device)");
            return TRACEY_ERROR_INVALID_STATE;
        }

        // Present the image to specified region
        bool success = presenterImpl->presentToRegion(
            vulkanImage,
            bounds->x, bounds->y,
            bounds->width, bounds->height,
            true
        );

        if (!success) {
            setError("Presentation failed (swapchain may need recreation)");
            return TRACEY_ERROR_PRESENTATION_FAILED;
        }

        return TRACEY_SUCCESS;

    } catch (const std::exception& e) {
        setError(std::string("Present to region failed: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Present to region failed: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_presenter_resize(
    TraceyPresenter* presenter,
    uint32_t newWidth,
    uint32_t newHeight)
{
    if (!presenter) {
        setError("Presenter pointer is null");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    }

    try {
        clearError();

        auto* presenterImpl = reinterpret_cast<tracey::VulkanPresenter*>(presenter);
        presenterImpl->resize(newWidth, newHeight);

        return TRACEY_SUCCESS;

    } catch (const std::exception& e) {
        setError(std::string("Resize failed: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    } catch (...) {
        setError("Resize failed: unknown error");
        return TRACEY_ERROR_UNKNOWN;
    }
}

void tracey_presenter_wait_idle(TraceyPresenter* presenter)
{
    if (presenter) {
        auto* presenterImpl = reinterpret_cast<tracey::VulkanPresenter*>(presenter);
        presenterImpl->waitIdle();
    }
}


} // extern "C"
// ============================================================================
// Procedural Node System Implementation (Phase 1)
// ============================================================================

#include "../scene/procedural/node_graph.hpp"
#include "../scene/procedural/nodes/actor_node.hpp"
#include "../scene/procedural/nodes/primitive_node.hpp"
extern "C" {

TraceyNodeGraph* tracey_scene_get_node_graph(TraceyScene* scene)
{
    if (!scene) {
        setError("Scene pointer is null");
        return nullptr;
    }

    try {
        clearError();
        auto* sceneImpl = reinterpret_cast<tracey::Scene*>(scene);
        return reinterpret_cast<TraceyNodeGraph*>(&sceneImpl->nodeGraph());
    } catch (const std::exception& e) {
        setError(std::string("Failed to get node graph: ") + e.what());
        return nullptr;
    }
}

uint64_t tracey_node_graph_create_node(
    TraceyNodeGraph* graph,
    TraceyNodeType type,
    const char* name)
{
    if (!graph) {
        setError("NodeGraph pointer is null");
        return UINT64_MAX;
    }

    try {
        clearError();
        auto* graphImpl = reinterpret_cast<tracey::NodeGraph*>(graph);

        std::string nodeName = name ? name : "Node";
        size_t uid = graphImpl->generateNodeUid();

        // Create the appropriate node type
        std::unique_ptr<tracey::ProceduralNode> node;

        switch (type) {
            case TRACEY_NODE_ACTOR:
                node = std::make_unique<tracey::ActorNode>(uid, nodeName);
                break;
            case TRACEY_NODE_PRIMITIVE_CUBE:
                node = std::make_unique<tracey::CubeNode>(uid, nodeName);
                break;
            case TRACEY_NODE_PRIMITIVE_SPHERE:
                node = std::make_unique<tracey::SphereNode>(uid, nodeName);
                break;
            case TRACEY_NODE_PRIMITIVE_TORUS:
                node = std::make_unique<tracey::TorusNode>(uid, nodeName);
                break;
            case TRACEY_NODE_PRIMITIVE_PLANE:
                node = std::make_unique<tracey::PlaneNode>(uid, nodeName);
                break;
            case TRACEY_NODE_PRIMITIVE_CYLINDER:
                node = std::make_unique<tracey::CylinderNode>(uid, nodeName);
                break;
            case TRACEY_NODE_PRIMITIVE_CONE:
                node = std::make_unique<tracey::ConeNode>(uid, nodeName);
                break;
            default:
                setError("Unsupported node type");
                return UINT64_MAX;
        }

        // Store the node in the graph - using const_cast workaround for Phase 1
        const_cast<std::unordered_map<size_t, std::unique_ptr<tracey::ProceduralNode>>&>(
            graphImpl->nodes()
        )[uid] = std::move(node);

        return uid;
    } catch (const std::exception& e) {
        setError(std::string("Failed to create node: ") + e.what());
        return UINT64_MAX;
    }
}

TraceyNode* tracey_node_graph_get_node(TraceyNodeGraph* graph, uint64_t nodeUid)
{
    if (!graph) {
        setError("NodeGraph pointer is null");
        return nullptr;
    }

    try {
        clearError();
        auto* graphImpl = reinterpret_cast<tracey::NodeGraph*>(graph);
        auto* node = graphImpl->getNode(nodeUid);
        return reinterpret_cast<TraceyNode*>(node);
    } catch (const std::exception& e) {
        setError(std::string("Failed to get node: ") + e.what());
        return nullptr;
    }
}

TraceyResult tracey_node_graph_remove_node(TraceyNodeGraph* graph, uint64_t nodeUid)
{
    if (!graph) {
        setError("NodeGraph pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* graphImpl = reinterpret_cast<tracey::NodeGraph*>(graph);
        bool removed = graphImpl->removeNode(nodeUid);
        return removed ? TRACEY_SUCCESS : TRACEY_ERROR_NOT_FOUND;
    } catch (const std::exception& e) {
        setError(std::string("Failed to remove node: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyNodeType tracey_node_get_type(TraceyNode* node)
{
    if (!node) {
        return TRACEY_NODE_ACTOR;
    }

    auto* nodeImpl = reinterpret_cast<tracey::ProceduralNode*>(node);
    switch (nodeImpl->nodeType()) {
        case tracey::NodeType::Actor: return TRACEY_NODE_ACTOR;
        case tracey::NodeType::GeometryPrimitive: {
            auto* primNode = dynamic_cast<tracey::PrimitiveNode*>(nodeImpl);
            if (primNode) {
                switch (primNode->primitiveType()) {
                    case tracey::PrimitiveType::Cube: return TRACEY_NODE_PRIMITIVE_CUBE;
                    case tracey::PrimitiveType::Sphere: return TRACEY_NODE_PRIMITIVE_SPHERE;
                    case tracey::PrimitiveType::Torus: return TRACEY_NODE_PRIMITIVE_TORUS;
                    case tracey::PrimitiveType::Plane: return TRACEY_NODE_PRIMITIVE_PLANE;
                    case tracey::PrimitiveType::Cylinder: return TRACEY_NODE_PRIMITIVE_CYLINDER;
                    case tracey::PrimitiveType::Cone: return TRACEY_NODE_PRIMITIVE_CONE;
                }
            }
            return TRACEY_NODE_PRIMITIVE_CUBE;
        }
        default: return TRACEY_NODE_ACTOR;
    }
}

const char* tracey_node_get_name(TraceyNode* node)
{
    if (!node) return "";
    auto* nodeImpl = reinterpret_cast<tracey::ProceduralNode*>(node);
    return nodeImpl->name().c_str();
}

TraceyResult tracey_node_set_name(TraceyNode* node, const char* name)
{
    if (!node || !name) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* nodeImpl = reinterpret_cast<tracey::ProceduralNode*>(node);
        nodeImpl->setName(name);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set node name: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    }
}

uint64_t tracey_node_get_uid(TraceyNode* node)
{
    if (!node) return UINT64_MAX;
    auto* nodeImpl = reinterpret_cast<tracey::ProceduralNode*>(node);
    return nodeImpl->uid();
}

TraceyParameter* tracey_node_get_parameter(TraceyNode* node, const char* paramName)
{
    if (!node || !paramName) return nullptr;
    auto* nodeImpl = reinterpret_cast<tracey::ProceduralNode*>(node);
    auto* param = nodeImpl->getParameter(paramName);
    return reinterpret_cast<TraceyParameter*>(param);
}

uint32_t tracey_node_get_parameter_count(TraceyNode* node)
{
    if (!node) return 0;
    auto* nodeImpl = reinterpret_cast<tracey::ProceduralNode*>(node);
    return static_cast<uint32_t>(nodeImpl->parameters().size());
}

TraceyParameterType tracey_parameter_get_type(TraceyParameter* param)
{
    if (!param) return TRACEY_PARAM_FLOAT;
    auto* paramImpl = reinterpret_cast<tracey::Parameter*>(param);
    switch (paramImpl->type()) {
        case tracey::ParameterType::Float: return TRACEY_PARAM_FLOAT;
        case tracey::ParameterType::Vec2: return TRACEY_PARAM_VEC2;
        case tracey::ParameterType::Vec3: return TRACEY_PARAM_VEC3;
        case tracey::ParameterType::Vec4: return TRACEY_PARAM_VEC4;
        case tracey::ParameterType::Int: return TRACEY_PARAM_INT;
        case tracey::ParameterType::Bool: return TRACEY_PARAM_BOOL;
        case tracey::ParameterType::String: return TRACEY_PARAM_STRING;
        case tracey::ParameterType::Color: return TRACEY_PARAM_COLOR;
        case tracey::ParameterType::Texture: return TRACEY_PARAM_TEXTURE;
        default: return TRACEY_PARAM_FLOAT;
    }
}

const char* tracey_parameter_get_name(TraceyParameter* param)
{
    if (!param) return "";
    auto* paramImpl = reinterpret_cast<tracey::Parameter*>(param);
    return paramImpl->name().c_str();
}

TraceyResult tracey_parameter_set_float(TraceyParameter* param, float value)
{
    if (!param) {
        setError("Parameter pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* paramImpl = reinterpret_cast<tracey::Parameter*>(param);
        paramImpl->setValue(value);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set float parameter: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_parameter_set_vec3(TraceyParameter* param, TraceyVec3 value)
{
    if (!param) {
        setError("Parameter pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* paramImpl = reinterpret_cast<tracey::Parameter*>(param);
        tracey::Vec3 vec3(value.x, value.y, value.z);
        paramImpl->setValue(vec3);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set vec3 parameter: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_parameter_set_int(TraceyParameter* param, int value)
{
    if (!param) {
        setError("Parameter pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* paramImpl = reinterpret_cast<tracey::Parameter*>(param);
        paramImpl->setValue(value);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set int parameter: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_parameter_set_bool(TraceyParameter* param, int value)
{
    if (!param) {
        setError("Parameter pointer is null");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* paramImpl = reinterpret_cast<tracey::Parameter*>(param);
        paramImpl->setValue(value != 0);
        return TRACEY_SUCCESS;
    } catch (const std::exception& e) {
        setError(std::string("Failed to set bool parameter: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_parameter_get_float(TraceyParameter* param, float* outValue)
{
    if (!param || !outValue) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* paramImpl = reinterpret_cast<tracey::Parameter*>(param);
        if (auto* floatVal = tracey::getValuePtr<float>(paramImpl->value())) {
            *outValue = *floatVal;
            return TRACEY_SUCCESS;
        }
        setError("Parameter is not a float type");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get float parameter: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_parameter_get_vec3(TraceyParameter* param, TraceyVec3* outValue)
{
    if (!param || !outValue) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* paramImpl = reinterpret_cast<tracey::Parameter*>(param);
        if (auto* vec3Val = tracey::getValuePtr<tracey::Vec3>(paramImpl->value())) {
            outValue->x = vec3Val->x;
            outValue->y = vec3Val->y;
            outValue->z = vec3Val->z;
            return TRACEY_SUCCESS;
        }
        setError("Parameter is not a vec3 type");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get vec3 parameter: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_parameter_get_int(TraceyParameter* param, int* outValue)
{
    if (!param || !outValue) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* paramImpl = reinterpret_cast<tracey::Parameter*>(param);
        if (auto* intVal = tracey::getValuePtr<int>(paramImpl->value())) {
            *outValue = *intVal;
            return TRACEY_SUCCESS;
        }
        setError("Parameter is not an int type");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get int parameter: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    }
}

TraceyResult tracey_parameter_get_bool(TraceyParameter* param, int* outValue)
{
    if (!param || !outValue) {
        setError("Null pointer parameter");
        return TRACEY_ERROR_NULL_POINTER;
    }

    try {
        clearError();
        auto* paramImpl = reinterpret_cast<tracey::Parameter*>(param);
        if (auto* boolVal = tracey::getValuePtr<bool>(paramImpl->value())) {
            *outValue = *boolVal ? 1 : 0;
            return TRACEY_SUCCESS;
        }
        setError("Parameter is not a bool type");
        return TRACEY_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& e) {
        setError(std::string("Failed to get bool parameter: ") + e.what());
        return TRACEY_ERROR_UNKNOWN;
    }
}

} // extern "C"
