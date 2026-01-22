#include "tracey_api.h"
#include "../device/device.hpp"
#include "../scene/scene.hpp"
#include "../scene/scene_compiler.hpp"
#include "../scene/gltf_loader.hpp"
#include "../rendering/path_tracer.hpp"

#include <string>
#include <cstring>
#include <stdexcept>

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

TraceyResult tracey_scene_load_gltf(
    TraceyScene* scene,
    const char* filePath)
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

        // Load GLTF into a temporary scene
        auto loadedScene = tracey::GltfLoader::loadFromFile(filePath);
        if (!loadedScene) {
            setError("Failed to load GLTF file");
            return TRACEY_ERROR_FILE_NOT_FOUND;
        }

        // Copy loaded scene into cleared scene
        // Copy all actors
        for (const auto& actor : loadedScene->actors()) {
            auto* newActor = s->createActor();
            newActor->setName(actor->name());
            newActor->setTransform(actor->transform());
            for (const auto& instance : actor->instances()) {
                newActor->addInstance(instance);
            }
        }

        // Copy all objects
        for (const auto& [name, obj] : loadedScene->objects()) {
            tracey::SceneObject objCopy = *obj;  // Make a copy
            s->addObject(name, std::move(objCopy));  // Move into scene
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
            outUids[count++] = actor->getUid();
        }

        return count;
    } catch (...) {
        return 0;
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

} // extern "C"
