#include "render_engine.hpp"

#include "path_tracer/api/backend_registry.hpp"

#include "core/parallel.hpp"
#include "scene/scene_object.hpp"
#include "graph/graphs/shader_graph/compiler.hpp"
#include "graph/graphs/shader_graph/serialization.hpp"

#include <stdexcept>

namespace tracey_editor {

RenderEngine::RenderEngine(RenderConfig config) : m_config(std::move(config)) {
    // Present-capable device so the editor can construct a swapchain against
    // the same VulkanContext used by the path tracer.
    auto* dev = tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute,
                                     /*enablePresentation=*/true);
    if (!dev)
        throw std::runtime_error("Failed to create rendering device");
    m_device.reset(dev);
    m_scene = std::make_unique<tracey::Scene>();
    m_blas_cache = std::make_unique<tracey::BlasCache>();
}

RenderEngine::~RenderEngine() = default;

void RenderEngine::initialize_path_tracer() {
    // Initial PT size mirrors the configured viewport size — render_engine
    // owners (EditorServer) call set_resolutions() with the inset rect once
    // the viewport reports its real pixel dimensions.
    if (m_pt_width == 0) m_pt_width = m_config.width;
    if (m_pt_height == 0) m_pt_height = m_config.height;

    tracey::PathTracerConfig pt_config;
    pt_config.width = m_pt_width;
    pt_config.height = m_pt_height;
    // EXR-export mode needs a float output to hold linear radiance + emits AOVs.
    pt_config.hdrOutput = m_config.hdr_output || m_config.export_aovs;
    pt_config.enableAovs = m_config.export_aovs;
    pt_config.linearOutput = m_config.export_aovs;
    // Live interactive denoise (CPU backend honours it). Rebuilt from m_config
    // here so a PT recreation (resolution / backend switch) preserves the state.
    pt_config.denoisePreview = m_config.denoise_preview;
    // One sample per render_tick. Accumulation stops once max_samples is
    // reached (enforced in EditorServer::render_tick).
    pt_config.samplesPerFrame = 1;
    pt_config.maxBounces = m_config.max_bounces;
    pt_config.useMaterialPrograms = true;
    // Backend choice from config; the TRACEY_PT_BACKEND env var override
    // is applied inside createPathTracerBackend so it covers examples too.
    pt_config.backend = tracey::pathTracerBackendKindFromString(m_config.pt_backend);

    m_path_tracer = std::make_unique<tracey::PathTracer>(m_device.get(), pt_config);

    // Seed the editor with a passthrough graph so the very first
    // get_material_graph from the frontend returns something useful.
    //
    // One MaterialInput, one MaterialOutput, wired port-to-port for the
    // five PBR slots the editor exposes (Albedo, Metallic, Roughness,
    // Emission, Normal). Port indices match materialInputPorts() and
    // materialOutputPorts() in src/graph/graphs/shader_graph/nodes.hpp:
    //   MaterialInput  outputs: P, N, T, V, uv0, uv1, InstanceID,
    //                            Albedo(7), Metallic(8), Roughness(9),
    //                            Emission(10), InNormal(11)
    //   MaterialOutput inputs:  Albedo(0), Metallic(1), Roughness(2),
    //                            Emission(3), Normal(4), Alpha(5),
    //                            IOR(6), Transmission(7)
    if (m_current_graph_json.empty()) {
        m_current_graph_json = R"({
    "version": 1,
    "uid": 0,
    "nodes": [
        {"uid": 1, "kind": "MaterialInput",  "position": [ 80, 60]},
        {"uid": 2, "kind": "MaterialOutput", "position": [420, 60]}
    ],
    "connections": [
        {"from_node": 1, "from_port":  7, "to_node": 2, "to_port": 0},
        {"from_node": 1, "from_port":  8, "to_node": 2, "to_port": 1},
        {"from_node": 1, "from_port":  9, "to_node": 2, "to_port": 2},
        {"from_node": 1, "from_port": 10, "to_node": 2, "to_port": 3},
        {"from_node": 1, "from_port": 11, "to_node": 2, "to_port": 4}
    ]
})";
    }
    // Material programs are now driven by SceneCompiler from per-actor
    // graphs; the seeded passthrough lives inside the compiled scene buffer.
}

void RenderEngine::initialize_rasterizer() {
    tracey::RasterizerConfig cfg;
    cfg.width = m_config.width;
    cfg.height = m_config.height;
    cfg.vertexShader   = m_config.shader_dir / "rasterizer" / "position_only.vert.spv";
    cfg.fragmentShader = m_config.shader_dir / "rasterizer" / "position_only.frag.spv";
    cfg.pointsVertexShader   = m_config.shader_dir / "rasterizer" / "points.vert.spv";
    cfg.pointsFragmentShader = m_config.shader_dir / "rasterizer" / "points.frag.spv";
    cfg.linesVertexShader    = m_config.shader_dir / "rasterizer" / "lines.vert.spv";
    cfg.linesFragmentShader  = m_config.shader_dir / "rasterizer" / "lines.frag.spv";
    cfg.groundVertexShader   = m_config.shader_dir / "rasterizer" / "ground.vert.spv";
    cfg.groundFragmentShader = m_config.shader_dir / "rasterizer" / "ground.frag.spv";
    cfg.gizmoVertexShader    = m_config.shader_dir / "rasterizer" / "gizmo.vert.spv";
    cfg.gizmoFragmentShader  = m_config.shader_dir / "rasterizer" / "gizmo.frag.spv";
    cfg.useDepthBuffer = true;
    cfg.depthTestEnable = true;
    cfg.cullBackFaces = false;  // SOP-cooked geometry can have either winding
    cfg.alphaBlending = false;
    cfg.colorFormat = tracey::ImageFormat::R8G8B8A8Unorm;
    m_rasterizer = std::make_unique<tracey::Rasterizer>(m_device.get(), cfg);
    m_rasterizer->setShowPoints(m_show_points);
    m_rasterizer->setShowEdges(m_show_edges);
    m_rasterizer->setShowGround(m_show_ground);
    // Apply the persisted background colour. The rasterizer's
    // ctor-default matches m_bg_* so this is a no-op on first launch;
    // when the user has set a custom colour and then we recreate the
    // rasterizer (e.g. on viewport resize), it carries through.
    m_rasterizer->setBackgroundColor(m_bg_r, m_bg_g, m_bg_b, m_bg_a);
}

void RenderEngine::set_material_graph_json(const std::string& graph_json) {
    if (!m_path_tracer)
        throw std::runtime_error("Path tracer not initialized");
    // Scratchpad only -- the modal authors a graph that the user later saves
    // to the library and assigns to actors. Live preview during edit would
    // require a separate "preview slot" concept; not in scope yet.
    m_current_graph_json = graph_json;
}

void RenderEngine::set_material_parameter(uint32_t program_id, uint32_t param_idx,
                                          float x, float y, float z, float w) {
    if (!m_path_tracer)
        throw std::runtime_error("Path tracer not initialized");
    m_path_tracer->setMaterialParameter(program_id, param_idx, tracey::Vec4(x, y, z, w));
}

void RenderEngine::compile_scene() {
    // Unique lock: rebuilding GPU buffers/BLAS must exclude both render workers
    // (rasterizer + PT) for its whole duration — see m_gpu_mutex.
    std::unique_lock<std::shared_mutex> gpu_lk(m_gpu_mutex);
    // Advance the scene generation: this compile may evictUntouched() buffers
    // that outstanding render snapshots still point at. Bumped under the unique
    // lock so a worker that later checks it (under the shared lock) reliably
    // sees the new value and skips its now-stale snapshot. release-store pairs
    // with the workers' acquire-load.
    m_scene_generation.fetch_add(1, std::memory_order_release);
    // Empty scene (e.g. SOP graph has no ObjectOutput wired yet, or fresh
    // launch before the first import): SceneCompiler::compile would throw on
    // no-geometry, so we install an empty CompiledScene instead of resetting
    // to null. That lets the rasterizer keep drawing the reference ground
    // grid (which doesn't need any scene instances) while we wait for the
    // user to add geometry. The path tracer side checks `instances.empty()`
    // and skips dispatch on its own.
    bool has_geometry = false;
    for (const auto& [_name, obj] : m_scene->objects()) {
        if (obj && obj->vertexCount() > 0) { has_geometry = true; break; }
    }
    if (!has_geometry) {
        m_compiled_scene =
            std::make_shared<tracey::SceneCompiler::CompiledScene>();
        return;
    }

    // Mark all cache entries untouched before the compile; any entry whose
    // SceneObject doesn't appear (by name + matching content hash) in this
    // compile will stay untouched and get evicted below. The cache itself
    // is bypassed when raster-only (SceneCompiler::compile ignores it in
    // that mode) so the markAll/evictUntouched bookkeeping is harmless —
    // the next PT-mode compile will repopulate everything from scratch.
    m_blas_cache->markAllUntouched();
    auto compiled = tracey::SceneCompiler::compile(
        m_device.get(), *m_scene, tracey::BVHConfig{}, m_blas_cache.get(),
        /*buildAccelerationStructures=*/m_build_acceleration_structures);
    m_compiled_scene =
        std::make_shared<tracey::SceneCompiler::CompiledScene>(std::move(compiled));
    m_blas_cache->evictUntouched();

    // Per-actor graphs were aggregated into materialPrograms during compile;
    // push them to the GPU so the hit shader's runMaterialProgram(programId, ...)
    // sees the right code at each programId slot. Skipped in raster-only
    // mode — the rasterizer doesn't run material programs, and the next
    // PT-on compile will upload a fresh program buffer anyway.
    if (m_build_acceleration_structures &&
        m_path_tracer && !m_compiled_scene->materialPrograms.headers().empty()) {
        m_path_tracer->setMaterialPrograms(m_compiled_scene->materialPrograms);
    }
}

void RenderEngine::set_motion_end_instances(
    std::vector<tracey::Tlas::Instance> endInstances) {
    if (!m_compiled_scene || endInstances.empty() ||
        endInstances.size() != m_compiled_scene->instances.size()) {
        return;
    }
    m_compiled_scene->instancesEnd = std::move(endInstances);
    m_compiled_scene->hasMotion = true;
    m_compiled_scene->revision = tracey::SceneCompiler::nextSceneRevision();
}

void RenderEngine::set_show_points(bool v) {
    m_show_points = v;
    if (m_rasterizer) m_rasterizer->setShowPoints(v);
}

void RenderEngine::set_show_edges(bool v) {
    m_show_edges = v;
    if (m_rasterizer) m_rasterizer->setShowEdges(v);
}

void RenderEngine::set_show_ground(bool v) {
    m_show_ground = v;
    if (m_rasterizer) m_rasterizer->setShowGround(v);
}

void RenderEngine::set_gizmo_visible(bool v) {
    if (m_rasterizer) m_rasterizer->setGizmoVisible(v);
}

void RenderEngine::set_gizmo_anchor(float x, float y, float z, float length) {
    if (m_rasterizer) {
        m_rasterizer->setGizmoAnchor(tracey::Vec3(x, y, z));
        m_rasterizer->setGizmoLength(length);
    }
}

void RenderEngine::set_background_color(float r, float g, float b, float a) {
    m_bg_r = r; m_bg_g = g; m_bg_b = b; m_bg_a = a;
    if (m_rasterizer) m_rasterizer->setBackgroundColor(r, g, b, a);
}

void RenderEngine::render_rasterizer() {
    if (!m_rasterizer || !m_compiled_scene || !m_scene->hasCamera()) return;
    m_rasterizer->render(*m_compiled_scene, m_scene->camera());
}

double RenderEngine::render_rasterizer_with(
    const tracey::SceneCompiler::CompiledScene &scene,
    const tracey::Camera &camera,
    uint64_t expectedGeneration)
{
    // Shared lock: rasterizer + PT renders read resources concurrently; only
    // set_resolutions / compile_scene / set_pt_backend (unique) exclude us.
    std::shared_lock<std::shared_mutex> gpu_lk(m_gpu_mutex);
    // Stale-snapshot guard: a compile_scene() since the snapshot was taken may
    // have evicted the BlasCache buffers `scene` still points at. Checked under
    // the shared lock — mutually exclusive with the compile's generation bump.
    if (m_scene_generation.load(std::memory_order_acquire) != expectedGeneration)
        return 0.0;
    if (!m_rasterizer) return 0.0;
    return m_rasterizer->render(scene, camera);
}

double RenderEngine::render_path_tracer_with(
    const tracey::SceneCompiler::CompiledScene &scene,
    const tracey::Camera &camera,
    bool clear,
    uint64_t expectedGeneration)
{
    // Shared lock: see render_rasterizer_with. Excluded only by GPU-resource
    // recreation (set_resolutions / set_pt_backend) and scene rebuilds
    // (compile_scene), so the PT can't trace against freed buffers/BLAS.
    std::shared_lock<std::shared_mutex> gpu_lk(m_gpu_mutex);
    // Same stale-snapshot guard as the rasterizer path.
    if (m_scene_generation.load(std::memory_order_acquire) != expectedGeneration)
        return 0.0;
    if (!m_path_tracer || !m_compiled_scene) return 0.0;
    // Nothing to trace — the rasterizer still shows the ground/overlays.
    if (m_compiled_scene->instances.empty()) return 0.0;
    return m_path_tracer->render(scene, camera, clear, /*want_pixels=*/false);
}

bool RenderEngine::refresh_tlas_only() {
    if (!m_compiled_scene) return false;
    // Note: tlas may legitimately be null when PT preview is off
    // (compile_scene skips BLAS/TLAS build via buildAccelerationStructures
    // = false). In that case we still update m_compiled_scene->instances
    // so the rasterizer picks up the new per-instance transforms — we
    // just skip the TLAS create at the end. Without this, the fast TRS
    // path would always bail with PT off and apply_emitted would fall
    // through to the full compile_scene, doing material-buffer rebuilds
    // and 40K per-instance walks every cook. That was eating ~9 ms of
    // rebuild_ms at high particle counts.

    // Walk the scene the same way SceneCompiler does (flatten() then per-actor
    // instances in declaration order). The instance index across this walk
    // must match the previous compile_scene's index 1:1 — that's how the
    // per-instance material indices, program ids, and UV offsets stay
    // correct without rebuilding their buffers.
    //
    // Two-pass structure so the per-instance Mat4 work parallelises cleanly:
    //   1. Serially build per-actor descriptors (visibility, world xform,
    //      blas index — one hash lookup per actor instead of per instance)
    //      and lay out the output index ranges.
    //   2. parallel_for_chunks over each actor's SceneInstances, writing
    //      Tlas::Instance entries by index into newInstances. At 400k
    //      particles the Mat4 multiplies (~10–15 ms serially) drop to a
    //      few ms across worker threads.
    auto sceneNodes = m_scene->flatten();

    struct ActorChunk {
        const tracey::Actor *actor;
        tracey::Mat4 worldTransform;
        size_t blasIndex;
        size_t outputStart;
        size_t instanceCount;
    };
    std::vector<ActorChunk> chunks;
    chunks.reserve(sceneNodes.size());

    size_t total = 0;
    for (const auto &node : sceneNodes) {
        const tracey::Actor *actor = node.actor;
        if (!actor || !actor->visible()) continue;
        const auto &slots = actor->instances();
        if (slots.empty()) continue;
        // For now assume every SceneInstance under one Actor shares the
        // same objectRef — true for the instance-group + single-mesh
        // emit paths, which together cover everything that hits this
        // function. If a future caller mixes objectRefs per Actor we'd
        // need to split chunks; that's a one-loop refactor.
        const std::string &objectRef = slots.front().objectRef();
        auto it = m_compiled_scene->objectToBlasIndex.find(objectRef);
        if (it == m_compiled_scene->objectToBlasIndex.end()) continue;
        chunks.push_back({actor, node.worldTransform, it->second, total, slots.size()});
        total += slots.size();
    }

    std::vector<tracey::Tlas::Instance> newInstances(total);

    // Cache for "preserve previous bookkeeping bits when the same slot
    // existed before" — captured by value into worker lambdas. Reading
    // m_compiled_scene->instances is safe (no concurrent writes); we
    // only read indices < .size().
    const auto &prevInstances = m_compiled_scene->instances;
    const size_t prevN = prevInstances.size();

    for (const auto &chunk : chunks) {
        const tracey::Mat4 &worldTransform = chunk.worldTransform;
        const auto &slots = chunk.actor->instances();
        const size_t outputStart = chunk.outputStart;
        const uint64_t blasIndex = chunk.blasIndex;
        tracey::parallel_for_chunks(slots.size(),
            [&slots, &newInstances, &prevInstances, &worldTransform,
             outputStart, blasIndex, prevN](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    const auto &sceneInstance = slots[i];
                    tracey::Mat4 finalTransform = worldTransform;
                    if (sceneInstance.hasLocalTransform()) {
                        finalTransform = worldTransform *
                                         sceneInstance.localTransform()->toMatrix();
                    }
                    tracey::Tlas::Instance instance;
                    instance.setTransform(finalTransform);
                    instance.blasAddress = blasIndex;
                    const size_t outIdx = outputStart + i;
                    if (outIdx < prevN) {
                        const auto &prev = prevInstances[outIdx];
                        instance.setCustomIndex(prev.instanceCustomIndex());
                        instance.setMask(prev.instanceMask());
                        instance.setSbtRecordOffset(prev.sbtRecordOffset());
                        instance.setInstanceFlags(prev.instanceFlags());
                    } else {
                        instance.setMask(0xFF);
                    }
                    newInstances[outIdx] = instance;
                }
            });
    }

    // Re-collect BLAS pointers from the (still-owned by BlasCache) entries.
    std::vector<const tracey::BottomLevelAccelerationStructure *> blasPtrs;
    blasPtrs.reserve(m_compiled_scene->blases.size());
    for (const auto *b : m_compiled_scene->blases) blasPtrs.push_back(b);

    // Allow per-cook instance-count drift (instance groups grow/shrink
    // with particle birth/death). The parallel scene arrays grow or
    // shrink in lockstep — new entries clone the LAST surviving entry
    // so they pick up the same material/program/UV setup, which is the
    // common case for particle systems where every instance under a
    // group shares the base material. The previous strict equality
    // check forced apply_emitted to the slow path (full compile_scene)
    // on every single particle spawn or death.
    const size_t newN = newInstances.size();
    auto extend = [newN](auto &v) {
        if (v.size() < newN) {
            using V = typename std::decay_t<decltype(v)>::value_type;
            const V fill = v.empty() ? V{} : v.back();
            v.resize(newN, fill);
        } else if (v.size() > newN) {
            v.resize(newN);
        }
    };
    // Commit the rebuilt instances/TLAS under the unique GPU lock: the async
    // render workers read m_compiled_scene (the SAME object — this is an
    // in-place mutation, not a fresh CompiledScene) under the shared lock, so
    // without this they could observe a half-swapped instances vector
    // mid-reallocation. Bump the scene generation too, so a snapshot captured
    // before this mutation is skipped by the workers rather than rendered
    // against the new instance layout. (Buffer lifetime is already covered by
    // CompiledScene's keep-alive; this closes the instances-vector data race.)
    std::unique_lock<std::shared_mutex> gpu_lk(m_gpu_mutex);
    m_scene_generation.fetch_add(1, std::memory_order_release);
    extend(m_compiled_scene->instanceToMaterialIndex);
    extend(m_compiled_scene->instanceProgramIndex);
    extend(m_compiled_scene->instanceUvOffset);
    extend(m_compiled_scene->materials);

    m_compiled_scene->instances = std::move(newInstances);
    // Gate on the live acceleration-structure flag rather than on
    // whether a TLAS happens to exist. The earlier ON→OFF transition
    // left a stale TLAS in m_compiled_scene; without this gate every
    // subsequent refresh would rebuild it for nothing, which is the
    // exact "PT toggle off doesn't restore FPS" symptom. Now: if PT
    // is off, we both skip the rebuild AND drop any lingering TLAS
    // so the rasterizer-only path stays cheap.
    if (m_build_acceleration_structures) {
        m_compiled_scene->tlas = std::unique_ptr<tracey::TopLevelAccelerationStructure>(
            m_device->createTopLevelAccelerationStructure(
                std::span<const tracey::BottomLevelAccelerationStructure *>(
                    blasPtrs.data(), blasPtrs.size()),
                std::span<const tracey::Tlas::Instance>(
                    m_compiled_scene->instances.data(),
                    m_compiled_scene->instances.size())));
    } else if (m_compiled_scene->tlas) {
        m_compiled_scene->tlas.reset();
    }
    // In-place mutation: stamp a fresh revision so path tracer backends
    // that cache per-scene resources (acceleration structures, buffer
    // copies) see the change. Skipping this renders stale transforms on
    // any backend that trusts the revision.
    m_compiled_scene->revision = tracey::SceneCompiler::nextSceneRevision();
    return true;
}

RenderResult RenderEngine::render_frame(bool clear_accumulation, bool want_pixels) {
    if (!m_path_tracer)
        throw std::runtime_error("Path tracer not initialized");
    if (!m_compiled_scene)
        throw std::runtime_error("Scene not compiled");
    if (!m_scene->hasCamera())
        throw std::runtime_error("Scene has no camera");

    // Empty scene (every actor hidden, only lights/subnet markers, or fresh
    // graph with no object_output): nothing for the path tracer to trace
    // against. Return a zeroed RenderResult instead of crashing — the
    // rasterizer still runs and shows the ground / overlays.
    if (m_compiled_scene->instances.empty())
    {
        const uint32_t width = m_pt_width;
        const uint32_t height = m_pt_height;
        // Size from the PT's actual output format — the EXR export forces it to
        // float (16 bpp) regardless of m_config.hdr_output, and a mismatch here
        // overruns the readback buffer.
        const size_t bytes_per_pixel = m_path_tracer->hdrOutput() ? 16 : 4;
        RenderResult result;
        result.width = width;
        result.height = height;
        result.sample_count = m_path_tracer->sampleCount();
        result.render_time_ms = 0.0;
        if (want_pixels)
        {
            result.pixels.assign(static_cast<size_t>(width) * height * bytes_per_pixel, 0);
        }
        return result;
    }

    const double render_time_ms =
        m_path_tracer->render(*m_compiled_scene, m_scene->camera(),
                              clear_accumulation, want_pixels);

    const uint32_t width = m_path_tracer->width();
    const uint32_t height = m_path_tracer->height();

    RenderResult result;
    result.width = width;
    result.height = height;
    result.sample_count = m_path_tracer->sampleCount();
    result.render_time_ms = render_time_ms;
    if (want_pixels)
    {
        // Size from the PT's actual output format (the EXR export forces float
        // output independently of m_config.hdr_output) — a mismatch overruns
        // result.pixels in path_tracer->readback().
        const size_t bytes_per_pixel = m_path_tracer->hdrOutput() ? 16 : 4;
        const size_t buffer_size = static_cast<size_t>(width) * height * bytes_per_pixel;
        result.pixels.resize(buffer_size);
        m_path_tracer->readback(result.pixels.data());
    }
    return result;
}

void RenderEngine::set_export_aovs(bool v) {
    if (m_config.export_aovs == v) return;
    m_config.export_aovs = v;
    // Recreate the PT so the output image (now float), accumulator, and AOV
    // buffers rebuild; re-push the material programs (the fresh backend starts
    // empty — same reasoning as set_resolutions()).
    if (m_path_tracer) {
        // Drain all in-flight GPU work before freeing the old path tracer's
        // images. The editor's viewport presenter blits the PT output image
        // (Metal↔Vulkan interop) asynchronously; destroying it while that blit
        // is still executing is use-after-free → device loss. Tab switches that
        // resize the PT used to crash exactly here on big scenes.
        if (m_device) m_device->waitIdle();
        m_path_tracer.reset();
        initialize_path_tracer();
        if (m_compiled_scene && !m_compiled_scene->materialPrograms.headers().empty()) {
            m_path_tracer->setMaterialPrograms(m_compiled_scene->materialPrograms);
        }
    }
}

void RenderEngine::set_pt_backend(const std::string& backend) {
    if (m_config.pt_backend == backend) return;
    // Unique lock: recreating the path tracer frees its GPU resources — exclude
    // both render workers. See m_gpu_mutex.
    std::unique_lock<std::shared_mutex> gpu_lk(m_gpu_mutex);
    m_config.pt_backend = backend;
    // Recreate the PT against the new backend; the façade picks output
    // resources per the backend's outputKind (Metal IOSurface vs CPU pixels).
    // Re-push material programs (the fresh backend starts empty).
    if (m_path_tracer) {
        // Drain all in-flight GPU work before freeing the old path tracer's
        // images. The editor's viewport presenter blits the PT output image
        // (Metal↔Vulkan interop) asynchronously; destroying it while that blit
        // is still executing is use-after-free → device loss. Tab switches that
        // resize the PT used to crash exactly here on big scenes.
        if (m_device) m_device->waitIdle();
        m_path_tracer.reset();
        initialize_path_tracer();
        if (m_compiled_scene && !m_compiled_scene->materialPrograms.headers().empty()) {
            m_path_tracer->setMaterialPrograms(m_compiled_scene->materialPrograms);
        }
    }
}

void RenderEngine::set_resolution(uint32_t width, uint32_t height) {
    // Back-compat: when only one size is given, raster + PT share it.
    set_resolutions(width, height, width, height);
}

void RenderEngine::set_resolutions(uint32_t raster_w, uint32_t raster_h,
                                   uint32_t pt_w, uint32_t pt_h) {
    if (raster_w == 0 || raster_h == 0) return;
    // Unique lock: recreates the path tracer AND the rasterizer (frees their
    // command pools, images and buffers). No render worker may be mid-render on
    // them — see m_gpu_mutex. (Reached only via the plural overload; the
    // singular set_resolution delegates here, so this is the one lock point.)
    std::unique_lock<std::shared_mutex> gpu_lk(m_gpu_mutex);
    if (pt_w == 0) pt_w = raster_w;
    if (pt_h == 0) pt_h = raster_h;

    const bool raster_changed =
        raster_w != m_config.width || raster_h != m_config.height;
    const bool pt_changed = pt_w != m_pt_width || pt_h != m_pt_height;
    if (!raster_changed && !pt_changed) return;

    m_config.width = raster_w;
    m_config.height = raster_h;
    m_pt_width = pt_w;
    m_pt_height = pt_h;

    if (pt_changed && m_path_tracer) {
        // Recreate the path tracer so its output image, descriptor sets, and
        // accumulator get rebuilt at the new size. Geometry/materials/lights
        // re-bind on the next dispatch (the fresh backend's scene revision
        // differs from the compiled scene's), but the material-program buffer
        // is uploaded out-of-band by setMaterialPrograms — the fresh instance
        // starts empty, so we MUST re-push it here. Without this the hit
        // shader's runMaterialProgram reads a stale/empty program slot and
        // every surface renders as garbage (blown-out white) until the next
        // compile_scene. Switching to the Render workspace collapses the
        // dopesheet, which resizes the viewport and trips exactly this path.
        // Drain all in-flight GPU work before freeing the old path tracer's
        // images. The editor's viewport presenter blits the PT output image
        // (Metal↔Vulkan interop) asynchronously; destroying it while that blit
        // is still executing is use-after-free → device loss. Tab switches that
        // resize the PT used to crash exactly here on big scenes.
        if (m_device) m_device->waitIdle();
        m_path_tracer.reset();
        initialize_path_tracer();
        if (m_compiled_scene && !m_compiled_scene->materialPrograms.headers().empty()) {
            m_path_tracer->setMaterialPrograms(m_compiled_scene->materialPrograms);
        }
    }
    if (raster_changed && m_rasterizer) {
        if (m_device) m_device->waitIdle(); // same hazard as the PT above
        m_rasterizer.reset();
        initialize_rasterizer();
    }
}

uint32_t RenderEngine::current_samples() const {
    return m_path_tracer ? m_path_tracer->sampleCount() : 0;
}

uint32_t RenderEngine::max_bounces() const {
    if (m_path_tracer)
        return m_path_tracer->maxBounces();
    return m_config.max_bounces;
}

void RenderEngine::set_max_bounces(uint32_t bounces) {
    m_config.max_bounces = bounces;
    if (m_path_tracer)
        m_path_tracer->setMaxBounces(bounces);
}

}  // namespace tracey_editor
