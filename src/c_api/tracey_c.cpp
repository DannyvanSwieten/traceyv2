// tracey_c.cpp — implementation of the Tracey C embedding API.
//
// Thin wrapper over the native C++ engine: Device + Scene + SceneCompiler +
// PathTracer. Everything C++-specific (STL, glm, engine headers) stays in this
// translation unit; the public header (tracey_c.h) is pure C. This is the
// embedding boundary — heavy optional modules (USD/MaterialX/OIDN) are NOT
// referenced here, keeping the ABI small and dependency-free.

#include "tracey_c.h"

#include "device/device.hpp"
#include "scene/scene.hpp"
#include "scene/scene_object.hpp"
#include "scene/scene_instance.hpp"
#include "scene/actor.hpp"
#include "scene/material_instance.hpp"
#include "scene/light.hpp"
#include "scene/camera.hpp"
#include "scene/transform.hpp"
#include "scene/scene_compiler.hpp"
#include "scene/blas_cache.hpp"
#include "shading/material_program/material_program.hpp"
#include "path_tracer/api/path_tracer.hpp"
#include "path_tracer/api/path_tracer_backend.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include <memory>
#include <new>
#include <string>

namespace
{
    // Most-recent-error string for tracey_last_error(). Not thread-safe by
    // design (documented); a diagnostics aid, not a control-flow channel.
    std::string g_lastError;
    void setError(const std::string &msg) { g_lastError = msg; }
    void clearError() { g_lastError.clear(); }

    tracey::Transform transformFromColumnMajor(const float *m16)
    {
        tracey::Transform t;
        if (!m16) return t;
        const glm::mat4 m = glm::make_mat4(m16); // glm is column-major: direct
        glm::vec3 scale, translation, skew;
        glm::vec4 perspective;
        glm::quat rotation;
        if (glm::decompose(m, scale, rotation, translation, skew, perspective))
        {
            t.setPosition(translation);
            t.setRotation(rotation);
            t.setScale(scale);
        }
        return t;
    }

    tracey::MaterialInstance toMaterialInstance(const tracey_material &m)
    {
        tracey::MaterialInstance mat("pbr");
        mat.setAlbedo(glm::vec3(m.base_color[0], m.base_color[1], m.base_color[2]));
        mat.setMetallic(m.metallic);
        mat.setRoughness(m.roughness);
        mat.setFloat("ior", m.ior);
        mat.setFloat("opacity", m.opacity);
        mat.setFloat("transmission", m.transmission);
        mat.setEmission(glm::vec3(m.emission[0], m.emission[1], m.emission[2]));
        mat.setFloat("emissionStrength", m.emission_strength);
        mat.setFloat("clearcoat", m.clearcoat);
        mat.setFloat("clearcoatRoughness", m.clearcoat_roughness);
        mat.setFloat("sheen", m.sheen);
        mat.setFloat("subsurface", m.subsurface);
        mat.setVec3("subsurfaceColor",
                    glm::vec3(m.subsurface_color[0], m.subsurface_color[1], m.subsurface_color[2]));
        mat.setFloat("anisotropy", m.anisotropy);
        return mat;
    }

    tracey::PathTracerBackendKind toBackendKind(tracey_backend b)
    {
        switch (b)
        {
        case TRACEY_BACKEND_METAL:  return tracey::PathTracerBackendKind::MetalRT;
        case TRACEY_BACKEND_VULKAN: return tracey::PathTracerBackendKind::VulkanRT;
        case TRACEY_BACKEND_CPU:    return tracey::PathTracerBackendKind::Cpu;
        case TRACEY_BACKEND_AUTO:
        default:                    return tracey::PathTracerBackendKind::Auto;
        }
    }
}

// ── Wrapper structs the opaque handles point to ─────────────────────────────

struct tracey_device_t
{
    std::unique_ptr<tracey::Device> device;
};

struct tracey_scene_t
{
    tracey::Scene scene;
};

struct tracey_renderer_t
{
    tracey::Device *device = nullptr; // borrowed (owned by tracey_device_t)
    tracey::PathTracerConfig config;
    std::unique_ptr<tracey::PathTracer> tracer;
    // The renderer owns the BLAS cache so the GPU buffers it holds are freed
    // when the renderer is destroyed — which the API contract requires to
    // happen BEFORE the device. (Compiling without a cache leaks entries into
    // a process-lifetime static that is finalized after the device is gone,
    // crashing in ~VulkanBuffer. This is the same pattern render_engine uses.)
    tracey::BlasCache blasCache;
};

// ── Defaults ────────────────────────────────────────────────────────────────

extern "C" tracey_material tracey_material_default(void)
{
    tracey_material m;
    m.base_color[0] = 0.8f; m.base_color[1] = 0.8f; m.base_color[2] = 0.8f;
    m.metallic = 0.0f;
    m.roughness = 0.5f;
    m.ior = 1.5f;
    m.opacity = 1.0f;
    m.transmission = 0.0f;
    m.emission[0] = 0.0f; m.emission[1] = 0.0f; m.emission[2] = 0.0f;
    m.emission_strength = 1.0f;
    m.clearcoat = 0.0f;
    m.clearcoat_roughness = 0.0f;
    m.sheen = 0.0f;
    m.subsurface = 0.0f;
    m.subsurface_color[0] = 1.0f; m.subsurface_color[1] = 1.0f; m.subsurface_color[2] = 1.0f;
    m.anisotropy = 0.0f;
    return m;
}

extern "C" tracey_camera tracey_camera_default(void)
{
    tracey_camera c;
    c.position[0] = 0.0f; c.position[1] = 0.0f; c.position[2] = 5.0f;
    c.target[0] = 0.0f; c.target[1] = 0.0f; c.target[2] = 0.0f;
    c.up[0] = 0.0f; c.up[1] = 1.0f; c.up[2] = 0.0f;
    c.fov_degrees = 45.0f;
    c.aperture = 0.0f;
    c.focal_distance = 0.0f;
    return c;
}

extern "C" tracey_render_config tracey_render_config_default(void)
{
    tracey_render_config c;
    c.width = 512;
    c.height = 512;
    c.samples_per_frame = 1;
    c.max_bounces = 8;
    c.hdr_output = 1;
    c.enable_aovs = 0;
    c.backend = TRACEY_BACKEND_AUTO;
    return c;
}

// ── Library ──────────────────────────────────────────────────────────────────

extern "C" const char *tracey_version(void) { return "0.1.0"; }

extern "C" const char *tracey_last_error(void) { return g_lastError.c_str(); }

// ── Device ────────────────────────────────────────────────────────────────────

extern "C" tracey_device tracey_device_create(tracey_backend backend)
{
    clearError();
    // The CPU path tracer backend still runs on a GPU "compute" device façade
    // for resource management; the backend kind (chosen at renderer creation)
    // selects CPU vs Metal execution. Use a GPU compute device, matching the
    // reference harness.
    (void)backend;
    tracey::Device *dev =
        tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute);
    if (!dev)
    {
        setError("createDevice failed");
        return nullptr;
    }
    auto *wrap = new (std::nothrow) tracey_device_t;
    if (!wrap)
    {
        delete dev;
        setError("out of memory");
        return nullptr;
    }
    wrap->device.reset(dev);
    return wrap;
}

extern "C" void tracey_device_destroy(tracey_device device) { delete device; }

// ── Scene ─────────────────────────────────────────────────────────────────────

extern "C" tracey_scene tracey_scene_create(void)
{
    clearError();
    auto *wrap = new (std::nothrow) tracey_scene_t;
    if (!wrap) setError("out of memory");
    return wrap;
}

extern "C" void tracey_scene_destroy(tracey_scene scene) { delete scene; }

extern "C" int tracey_scene_add_mesh(tracey_scene scene, const char *name,
                                     const float *positions, uint32_t vertex_count,
                                     const float *normals, const float *uvs,
                                     const uint32_t *indices, uint32_t index_count)
{
    clearError();
    if (!scene || !name || !positions || !indices || vertex_count == 0 || index_count == 0)
    {
        setError("add_mesh: null/empty argument");
        return -1;
    }
    if (scene->scene.hasObject(name))
    {
        setError(std::string("add_mesh: duplicate mesh name '") + name + "'");
        return -1;
    }

    auto obj = std::make_unique<tracey::SceneObject>();
    obj->setName(name);

    std::vector<tracey::Vec3> pos(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i)
        pos[i] = tracey::Vec3(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]);
    obj->setPositions(std::move(pos));

    std::vector<uint32_t> idx(indices, indices + index_count);
    obj->setIndices(std::move(idx));

    if (normals)
    {
        std::vector<tracey::Vec3> n(vertex_count);
        for (uint32_t i = 0; i < vertex_count; ++i)
            n[i] = tracey::Vec3(normals[i * 3 + 0], normals[i * 3 + 1], normals[i * 3 + 2]);
        obj->setNormals(std::move(n));
    }
    if (uvs)
    {
        std::vector<tracey::Vec2> u(vertex_count);
        for (uint32_t i = 0; i < vertex_count; ++i)
            u[i] = tracey::Vec2(uvs[i * 2 + 0], uvs[i * 2 + 1]);
        obj->setUvs(std::move(u));
    }

    scene->scene.addObject(name, std::move(obj));
    return 0;
}

extern "C" int tracey_scene_add_instance(tracey_scene scene, const char *mesh_name,
                                         const tracey_material *material,
                                         const float *transform4x4)
{
    clearError();
    if (!scene || !mesh_name)
    {
        setError("add_instance: null argument");
        return -1;
    }
    if (!scene->scene.hasObject(mesh_name))
    {
        setError(std::string("add_instance: unknown mesh '") + mesh_name + "'");
        return -1;
    }

    tracey::Actor *actor = scene->scene.createActor();
    actor->setName(mesh_name);
    actor->setTransform(transformFromColumnMajor(transform4x4));

    tracey::SceneInstance inst(mesh_name);
    if (material) inst.setMaterial(toMaterialInstance(*material));
    actor->addInstance(std::move(inst));
    return 0;
}

extern "C" int tracey_scene_add_light(tracey_scene scene, const tracey_light *light,
                                      const float *transform4x4)
{
    clearError();
    if (!scene || !light)
    {
        setError("add_light: null argument");
        return -1;
    }

    tracey::Light l;
    l.type = static_cast<tracey::LightType>(light->type);
    l.color = tracey::Vec3(light->color[0], light->color[1], light->color[2]);
    l.intensity = light->intensity;
    l.radius = light->radius;
    l.size = tracey::Vec2(light->size[0], light->size[1]);
    if (light->hdri_path) l.hdriPath = light->hdri_path;

    tracey::Actor *actor = scene->scene.createActor();
    actor->setName("light");
    actor->setTransform(transformFromColumnMajor(transform4x4));
    actor->setLight(l);
    return 0;
}

extern "C" void tracey_scene_set_camera(tracey_scene scene, const tracey_camera *camera)
{
    clearError();
    if (!scene || !camera) { setError("set_camera: null argument"); return; }

    const glm::vec3 eye(camera->position[0], camera->position[1], camera->position[2]);
    const glm::vec3 target(camera->target[0], camera->target[1], camera->target[2]);
    glm::vec3 up(camera->up[0], camera->up[1], camera->up[2]);
    if (glm::length(up) < 1e-6f) up = glm::vec3(0.0f, 1.0f, 0.0f);

    glm::vec3 fwd = target - eye;
    const float dist = glm::length(fwd);
    fwd = (dist > 1e-6f) ? fwd / dist : glm::vec3(0.0f, 0.0f, -1.0f);

    tracey::Camera cam;
    cam.setPosition(eye);
    cam.setRotation(glm::quatLookAt(fwd, up)); // default camera looks down -Z
    cam.setFov(camera->fov_degrees > 0.0f ? camera->fov_degrees : 45.0f);
    if (camera->aperture > 0.0f)
    {
        cam.setAperture(camera->aperture);
        cam.setFocalDistance(camera->focal_distance > 0.0f ? camera->focal_distance
                                                           : (dist > 1e-6f ? dist : 1.0f));
    }
    scene->scene.setCamera(cam);
}

// ── Renderer ────────────────────────────────────────────────────────────────

extern "C" tracey_renderer tracey_renderer_create(tracey_device device,
                                                  const tracey_render_config *config)
{
    clearError();
    if (!device || !config)
    {
        setError("renderer_create: null argument");
        return nullptr;
    }

    tracey::PathTracerConfig cfg;
    cfg.width = config->width ? config->width : 512;
    cfg.height = config->height ? config->height : 512;
    cfg.hdrOutput = config->hdr_output != 0;
    cfg.samplesPerFrame = config->samples_per_frame ? config->samples_per_frame : 1;
    cfg.maxBounces = config->max_bounces ? config->max_bounces : 8;
    cfg.enableAovs = config->enable_aovs != 0;
    cfg.useMaterialPrograms = true;
    cfg.backend = toBackendKind(config->backend);

    auto *wrap = new (std::nothrow) tracey_renderer_t;
    if (!wrap) { setError("out of memory"); return nullptr; }
    wrap->device = device->device.get();
    wrap->config = cfg;

    try
    {
        wrap->tracer = std::make_unique<tracey::PathTracer>(wrap->device, cfg);
        // Passthrough program: copies the compiled GPUMaterial slots verbatim.
        // One program covers every instance (instanceProgramIndex defaults 0).
        tracey::MaterialProgramBuffer programs;
        programs.addProgram(tracey::makePassthroughProgram());
        wrap->tracer->setMaterialPrograms(programs);
    }
    catch (const std::exception &e)
    {
        setError(std::string("renderer_create: ") + e.what());
        delete wrap;
        return nullptr;
    }
    return wrap;
}

extern "C" void tracey_renderer_destroy(tracey_renderer renderer) { delete renderer; }

extern "C" int tracey_render(tracey_renderer renderer, tracey_scene scene,
                             uint32_t sample_count)
{
    clearError();
    if (!renderer || !scene || !renderer->tracer)
    {
        setError("render: null argument");
        return -1;
    }
    if (!scene->scene.hasCamera())
    {
        setError("render: scene has no camera (call tracey_scene_set_camera)");
        return -1;
    }
    if (sample_count == 0) sample_count = 1;

    try
    {
        tracey::SceneCompiler::CompiledScene compiled =
            tracey::SceneCompiler::compile(renderer->device, scene->scene, &renderer->blasCache);
        const tracey::Camera camera = scene->scene.camera();

        for (uint32_t s = 0; s < sample_count; ++s)
        {
            const bool clear = (s == 0);
            const bool want = (s == sample_count - 1);
            renderer->tracer->render(compiled, camera, clear, want);
        }
    }
    catch (const std::exception &e)
    {
        setError(std::string("render: ") + e.what());
        return -1;
    }
    return 0;
}

extern "C" size_t tracey_readback_beauty(tracey_renderer renderer, void *out)
{
    clearError();
    if (!renderer || !renderer->tracer || !out)
    {
        setError("readback_beauty: null argument");
        return 0;
    }
    return renderer->tracer->readback(out);
}

extern "C" size_t tracey_readback_aov(tracey_renderer renderer, tracey_aov aov, void *out)
{
    clearError();
    if (!renderer || !renderer->tracer || !out)
    {
        setError("readback_aov: null argument");
        return 0;
    }
    if (static_cast<int>(aov) < 0 || static_cast<int>(aov) >= static_cast<int>(TRACEY_AOV_COUNT))
    {
        setError("readback_aov: invalid AOV");
        return 0;
    }
    return renderer->tracer->readbackAOV(static_cast<tracey::AovKind>(aov), out);
}
