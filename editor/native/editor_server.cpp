#include "editor_server.hpp"

#include "json_helpers.hpp"
#include "editor_server_cmds_common.hpp"
#include "platform/platform.hpp"
#include "scene_state.hpp"
#include "video_exporter.hpp"

#include "core/parallel.hpp"
#include "io/denoiser.hpp"
#include "io/exr_writer.hpp"
#include "io/png_writer.hpp"
#include "scene/actor.hpp"
#include "scene/camera.hpp"
#include "scene/gltf_loader.hpp"
#include "scene/usd_loader.hpp"
#include "scene/material_instance.hpp"
#include "scene/scene.hpp"
#include "scene/scene_instance.hpp"
#include "scene/scene_object.hpp"
#include "scene/transform.hpp"

#include "path_tracer/api/path_tracer.hpp"
#include "graph/graphs/shader_graph/serialization.hpp"

#include "geometry/geometry_converter.hpp"
#include "sops/serialization.hpp"
#include "sops/sop_graph.hpp"
#include "sops/sop_node.hpp"
#include "sops/sop_registry.hpp"
#include "sops/nodes/attribute_vop_sop.hpp"
#include "sops/nodes/instance_vop_sop.hpp"
#include "sops/nodes/dop_import_sop.hpp"
#include "vops/vop_graph.hpp"
#include "vops/vop_registry.hpp"
#include "vops/serialization.hpp"
#include "vops/register_builtins.hpp"
#include "dops/dop_graph.hpp"
#include "dops/dop_node.hpp"
#include "dops/dop_registry.hpp"
#include "dops/serialization.hpp"
#include "dops/register_builtins.hpp"
#include "dops/nodes/pop_force.hpp"

#include "vops/codegen/compute_dispatch.hpp"
#include "sops/codegen/copy_to_points_compute.hpp"
#include "sops/codegen/transform_compute.hpp"
#include "sops/codegen/merge_compute.hpp"
#include "geometry/attribute_allocator.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>  // glm::rotation for from→to vector quat
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt for default camera pose

#include <json.hpp>

#ifdef TRACEY_HAS_USD
// Defined in tracey_usd (src/sops/nodes/usd_import_sop.cpp). Forward-declared
// here — the usd_import SOP type isn't in any core header (USD stays out of
// core); the editor registers it into the shared SopRegistry at startup.
namespace tracey { namespace sops { void registerUsdImportSop(); } }
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>

using nlohmann::json;

namespace tracey_editor {


std::filesystem::path EditorServer::project_material_dir() const {
    if (m_project_dir.empty()) return {};
    return m_project_dir / "materials";
}

std::filesystem::path EditorServer::resolve_material_path(const std::string &name) const {
    // Project-local materials shadow global ones with the same name —
    // the rationale being a project file declares its own copy of a
    // shared material when it wants to deviate, and that override
    // should win.
    std::error_code ec;
    if (!m_project_dir.empty()) {
        std::filesystem::path local = project_material_dir() / (name + ".json");
        if (std::filesystem::exists(local, ec)) return local;
    }
    std::filesystem::path global = global_material_dir() / (name + ".json");
    if (std::filesystem::exists(global, ec)) return global;
    return {};
}


EditorServer::EditorServer(std::unique_ptr<RenderEngine> engine, EditorWindow* window)
    : m_engine(std::move(engine)), m_window(window) {
    // SOP framework setup. Register the v1 built-in nodes once per process
    // before the first set_sop_graph / catalog query lands. Idempotent-ish:
    // calling registerBuiltinSops() twice would duplicate entries, so it
    // only runs the first time an EditorServer is constructed.
    static bool s_sopsRegistered = false;
    if (!s_sopsRegistered) {
        tracey::sops::registerBuiltinSops();
        tracey::vops::registerBuiltinVops();
        tracey::dops::registerBuiltinDops();
#ifdef TRACEY_HAS_USD
        // The usd_import SOP lives in tracey_usd (USD stays out of core); the
        // editor is its only consumer, so it registers the type into the shared
        // SopRegistry singleton here after the builtins.
        tracey::sops::registerUsdImportSop();
#endif
        s_sopsRegistered = true;
    }
    m_sop_graph = std::make_unique<tracey::sops::SopGraph>(0);
    m_dop_graph = std::make_unique<tracey::dops::DopGraph>(0);
    wire_dop_sop_provider();

    // Register the engine's device with AttributeAllocator so any
    // Attribute<T>::buffer() call across the codebase can lazily
    // allocate its GPU storage against the right Vulkan context.
    // This is the foundation for the GPU-resident Geometry refactor
    // (Phase A); the dispatcher + future SOP kernels read it the
    // same way.
    if (m_engine) {
        tracey::AttributeAllocator::setDevice(&m_engine->device());
        // Match the engine's BVH-build flag to the live-viewport default
        // (PT inset preview off → no BVHs on the first compile). The
        // set_pt_preview command flips both flags together once the user
        // explicitly enables the preview.
        m_engine->set_build_acceleration_structures(m_pt_preview_enabled);
    }

    // Wire up the GPU VOP dispatcher against the engine's compute
    // device. Fail-safe: any throw here just leaves attribute_vop /
    // pop_force on the CPU code path — we don't want a missing
    // compute backend to break the editor.
    if (m_engine) {
        try {
            m_vop_dispatcher = std::make_unique<tracey::vops::codegen::VopComputeDispatcher>(
                &m_engine->device());
            tracey::vops::codegen::VopComputeDispatcher::setGlobal(m_vop_dispatcher.get());
            std::fprintf(stderr, "[vop:compute] GPU dispatcher ready\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[vop:compute] GPU dispatcher unavailable, "
                                 "falling back to CPU: %s\n", e.what());
        }
        // copy_to_points has its own dispatcher (fixed kernel, feature-
        // flagged variants). Same fail-safe contract: a missing GPU
        // dispatcher leaves the SOP on the CPU path.
        try {
            m_ctp_dispatcher = std::make_unique<tracey::sops::codegen::CopyToPointsCompute>(
                &m_engine->device());
            tracey::sops::codegen::CopyToPointsCompute::setGlobal(m_ctp_dispatcher.get());
            std::fprintf(stderr, "[ctp:compute] GPU dispatcher ready\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ctp:compute] GPU dispatcher unavailable, "
                                 "falling back to CPU: %s\n", e.what());
        }
        // transform SOP has its own dispatcher (in-place SRT compute
        // for Vec3 attributes). Same fail-safe contract.
        try {
            m_xform_dispatcher = std::make_unique<tracey::sops::codegen::TransformCompute>(
                &m_engine->device());
            tracey::sops::codegen::TransformCompute::setGlobal(m_xform_dispatcher.get());
            std::fprintf(stderr, "[xform:compute] GPU dispatcher ready\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[xform:compute] GPU dispatcher unavailable, "
                                 "falling back to CPU: %s\n", e.what());
        }
        // merge SOP — vkCmdCopyBuffer-based concat. No shader, just
        // owns the Vulkan device handle. Same fail-safe contract.
        try {
            m_merge_dispatcher = std::make_unique<tracey::sops::codegen::MergeCompute>(
                &m_engine->device());
            tracey::sops::codegen::MergeCompute::setGlobal(m_merge_dispatcher.get());
            std::fprintf(stderr, "[merge:compute] GPU dispatcher ready\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[merge:compute] GPU dispatcher unavailable, "
                                 "falling back to CPU: %s\n", e.what());
        }
    }

    m_cook_thread = std::thread([this] { cook_worker_loop(); });

    // Render worker. Started AFTER m_engine is fully initialised so the
    // first dispatch isn't racing the rasterizer/path-tracer ctors.
    m_render_thread = std::thread([this] { render_thread_main(); });

    // Default-Dome spawn. A fresh editor session opens with no scene
    // file; without this the viewport stays unlit until the user adds a
    // light (the shader has an unlit-albedo fallback, but Blender/Unity
    // convention is to have an env light from the start). Skipped if
    // the scene already contains any actor — load_scene runs before
    // this on app-launch-with-project, and a project that intentionally
    // ships no Dome should stay that way.
    if (m_engine && m_engine->scene().actors().empty()) {
        auto* actor = m_engine->scene().createActor();
        actor->setName("Dome");
        tracey::Light light;
        light.type = tracey::LightType::Dome;
        actor->setLight(light);
    }
}

EditorServer::~EditorServer() {
    // Tell the export worker to bail; join before tearing down the engine
    // since the worker writes to the path tracer.
    m_export_cancel.store(true);
    if (m_export_thread.joinable()) m_export_thread.join();

    {
        std::lock_guard<std::mutex> lk(m_cook_request_mutex);
        m_cook_shutdown = true;
    }
    m_cook_request_cv.notify_one();
    if (m_cook_thread.joinable()) m_cook_thread.join();

    // Render worker also has to stop BEFORE the engine tears down — it
    // dereferences m_engine->rasterizer/path_tracer on every iteration.
    stop_render_thread();

    // Clear the global dispatcher BEFORE the engine (and its device)
    // tear down, so any late cook that races shutdown sees nullptr
    // and falls back to CPU rather than dereferencing a destroyed
    // VkDevice.
    tracey::vops::codegen::VopComputeDispatcher::setGlobal(nullptr);
    m_vop_dispatcher.reset();
    tracey::sops::codegen::CopyToPointsCompute::setGlobal(nullptr);
    m_ctp_dispatcher.reset();
    tracey::sops::codegen::TransformCompute::setGlobal(nullptr);
    m_xform_dispatcher.reset();
    tracey::sops::codegen::MergeCompute::setGlobal(nullptr);
    m_merge_dispatcher.reset();
    // And the attribute allocator — once the device goes, any
    // pending Attribute<T>::buffer() / bufferConst() call from a
    // late cook must see "no device available" and skip the GPU
    // path instead of holding a dangling pointer.
    tracey::AttributeAllocator::setDevice(nullptr);

    // Tear the viewport (and its VkSurface) down explicitly, while the engine
    // — and therefore the VkInstance the surface was created from, and the
    // CAMetalLayer it wraps — are still guaranteed alive. Member-destruction
    // order would do the same thing implicitly, but stating it here protects
    // against future header re-ordering.
    m_viewport.reset();
}

void EditorServer::set_broadcast_callback(BroadcastCallback cb) {
    m_broadcast = std::move(cb);
}

void EditorServer::broadcast(const std::string& message) {
    if (m_broadcast) m_broadcast(message);
}

void EditorServer::wire_dop_sop_provider() {
    if (m_dop_graph) m_dop_graph->setSopProvider(&m_sop_provider);
}

void EditorServer::ensure_viewport_renderer(uint32_t pixel_w, uint32_t pixel_h) {
    if (!m_window || pixel_w == 0 || pixel_h == 0) return;

    if (!m_viewport) {
        try {
            m_viewport = std::make_unique<ViewportRenderer>(
                m_engine->device(), m_window->gpu_surface(), m_window->gpu_display(),
                pixel_w, pixel_h);
            m_viewport_pixel_w = pixel_w;
            m_viewport_pixel_h = pixel_h;
            // Right-size the rasterizer (full viewport) and PT (inset).
            const InsetRect r = compute_inset_rect(pixel_w, pixel_h);
            m_engine->set_resolutions(pixel_w, pixel_h, r.w, r.h);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[viewport] failed to create renderer: %s\n", e.what());
            m_viewport.reset();
        }
        return;
    }

    if (pixel_w != m_viewport_pixel_w || pixel_h != m_viewport_pixel_h) {
        // Resize the swapchain only (cheap — a flag flip; the actual recreate
        // happens lazily on the next present). The renderer pipelines stay
        // at their current size until set_viewport_resolution arrives, which
        // the frontend debounces (250ms). Without this split a splitter drag
        // would tear down + rebuild the path tracer and rasterizer per pixel.
        m_viewport->resize(pixel_w, pixel_h);
        m_viewport_pixel_w = pixel_w;
        m_viewport_pixel_h = pixel_h;
        m_clear_next_frame = true;
    }
}

// Pick the right path-tracer render size for the current (fullscreen,
// override, viewport) tuple and push it to the engine. Called whenever
// any of those inputs changes (set_pt_fullscreen, set_pt_render_resolution,
// or viewport resize via ensure_viewport_renderer). Idempotent — silently
// no-ops when the viewport hasn't been initialised yet.
void EditorServer::apply_pt_resolution() {
    if (!m_engine || m_viewport_pixel_w == 0 || m_viewport_pixel_h == 0) return;

    uint32_t pt_w, pt_h;
    if (m_pt_fullscreen) {
        // Render workspace. Honour the override when set; otherwise
        // match the viewport so there's no upscale.
        if (m_pt_render_w > 0 && m_pt_render_h > 0) {
            pt_w = m_pt_render_w;
            pt_h = m_pt_render_h;
        } else {
            pt_w = m_viewport_pixel_w;
            pt_h = m_viewport_pixel_h;
        }
    } else {
        // PiP composite. Resolution always tracks the inset rect; the
        // override is ignored because at PiP size it'd just be wasteful.
        const InsetRect r = compute_inset_rect(m_viewport_pixel_w, m_viewport_pixel_h);
        pt_w = r.w;
        pt_h = r.h;
    }
    // Only clear the accumulator when the size actually changed — this helper
    // is called on every viewport-resolution report (which the frontend sends
    // on any layout shift), and an unconditional clear would stop the path
    // tracer ever accumulating.
    const auto [oldR_w, oldR_h] = m_engine->resolution();
    const auto [oldP_w, oldP_h] = m_engine->pt_resolution();
    m_engine->set_resolutions(m_viewport_pixel_w, m_viewport_pixel_h, pt_w, pt_h);
    const auto [newR_w, newR_h] = m_engine->resolution();
    const auto [newP_w, newP_h] = m_engine->pt_resolution();
    if (newR_w != oldR_w || newR_h != oldR_h || newP_w != oldP_w || newP_h != oldP_h)
        m_clear_next_frame = true;
}

// Cook the SOP graph and rebuild the live scene from the result. Must be
// called with m_mutex held; runs on the main thread (touches Vulkan via the
// final compile_scene() call).
//
// Synchronous path used for one-shot operations (load_scene). The live
// editing path goes through post_cook_request → cook_worker_loop →
// drain_cook_result + apply_emitted instead.
void EditorServer::cook_and_apply() {
    if (!m_sop_graph || !m_engine) return;

    // Same cook_status broadcast as the async path so the frontend's loading
    // status fires on synchronous load_scene cooks too.
    if (m_broadcast) m_broadcast(R"({"event":"cook_status","busy":true})");

    // Stamp the current-frame sim geometry into every dop_import SOP
    // before cooking. The async path uses CookRequest::dop_stamps for
    // this; the synchronous path (video export, load_scene) doesn't go
    // through that worker, so without this the dop_import nodes cook
    // with their default-empty stamp and the rendered actors come back
    // with zero vertices — i.e. the exported video frame is solid black.
    if (m_dop_graph && m_has_dop_imports) {
        auto stamps = collect_dop_stamps(m_timeline.current_time);
        for (auto& [uid, geo] : stamps) {
            if (auto* node = findNodeRecursive(m_sop_graph.get(), uid)) {
                tracey::sops::setDopImportGeometry(node, std::move(geo));
            }
        }
    }

    tracey::sops::CookDiagnostic diag;
    // Same Houdini-style per-node cook cache as the worker path, just a
    // separate instance — this one's owned by the main thread under
    // m_mutex. Reuses cached Geometry for any node whose
    // (kind, params, upstream cookIds) fingerprint is unchanged.
    m_main_cook_cache.markAllUntouched();
    auto emitted = m_sop_graph->cook(&diag, m_timeline.current_time,
                                     &m_main_cook_cache);
    m_main_cook_cache.evictUntouched();
    if (!diag.ok) {
        std::fprintf(stderr, "[sop] cook failed: %s (node uid=%zu)\n",
                     diag.message.c_str(), diag.nodeUid);
        if (m_broadcast) m_broadcast(R"({"event":"cook_status","busy":false})");
        return;
    }
    apply_emitted(std::move(emitted));
    if (m_broadcast) m_broadcast(R"({"event":"cook_status","busy":false})");
}

// Replace the live scene's actors + named SceneObjects with the emitted set.
// We don't try to incrementally update — even moderately complex graphs can
// change topology arbitrarily, and the path tracer's recompile already costs
// more than scene rebuild. Material assignments survive across cooks because
// they're stored on the ObjectOutput SOP node's `material_library_name`
// parameter and re-resolved here.
//
// Must be called with m_mutex held; main-thread only (Vulkan recompile).
// FNV-1a primitives used by the per-actor + global signatures below.
namespace {
constexpr uint64_t kSigOffset = 0xcbf29ce484222325ULL;
constexpr uint64_t kSigPrime  = 0x00000100000001b3ULL;

// Compose a (sourceNodeUid, instanceIndex) into a 64-bit key for the per-
// actor tracking maps. Layout: low 24 bits = instanceIndex (16M instances
// per SOP, comfortably more than any reasonable scatter/instance graph),
// high 40 bits = sourceNodeUid. The high bits stay 40-wide so editor uid
// counters can grow well past 2^32 before any collision risk.
inline uint64_t make_actor_key(size_t uid, uint32_t inst) {
    return (static_cast<uint64_t>(uid) << 24) |
           (static_cast<uint64_t>(inst) & 0xFFFFFFULL);
}
inline uint64_t make_actor_key(const tracey::sops::EmittedActor &a) {
    return make_actor_key(a.sourceNodeUid, a.instanceIndex);
}
inline void sig_mix(uint64_t &h, const void *p, size_t n) {
    // 8-byte bulk path for the per-instance hash walks (transform_sig
    // over 40K particles is a ~1.6 MB sweep — the old per-byte FNV ran
    // at ~600 MB/s, dominating apply_emitted's rebuild bucket). The
    // bulk path runs at L1 bandwidth (~10 GB/s) and the per-byte
    // tail handles non-multiple-of-8 buffers (strings, mat3, etc.)
    // so existing callers stay correct.
    const auto *b = static_cast<const unsigned char *>(p);
    while (n >= 8) {
        uint64_t v;
        std::memcpy(&v, b, 8);  // memcpy is the canonical unaligned-load idiom
        h ^= v;
        h *= kSigPrime;
        h ^= h >> 31;  // avalanche so byte-shifts within a 64-bit chunk diverge
        b += 8;
        n -= 8;
    }
    while (n > 0) {
        h ^= *b++;
        h *= kSigPrime;
        --n;
    }
}
inline void sig_mix_str(uint64_t &h, const std::string &s) {
    sig_mix(h, s.data(), s.size());
    const uint64_t n = s.size();
    sig_mix(h, &n, sizeof(n));
}

// 64-bit fingerprint of a Geometry payload — captures everything
// `GeometryConverter::toSceneObject` reads (positions, vertex→point
// topology, primitive list, per-point / per-vertex N/uv/Cd). Two
// Geometry values producing identical SceneObjects share this hash, so
// apply_emitted can dedupe them onto one shared SceneObject + BLAS.
//
// Stable within one process run; not for on-disk comparison.
uint64_t geometry_dedup_hash(const tracey::Geometry &g);

// Per-pointer hash cache. Cleared at the start of every apply_emitted /
// emitted_signature pass so two Geometries that happen to reuse the same
// heap address across cooks can't return stale entries — within a single
// pass, the emitted vector holds all the shared_ptrs alive, so pointer
// identity is safe. The whole point of this cache is the instance SOP:
// 120 actors all referencing the same stamp pay the hashing cost once
// instead of 120 times.
std::unordered_map<const tracey::Geometry*, uint64_t> &geometryHashCache() {
    thread_local std::unordered_map<const tracey::Geometry*, uint64_t> c;
    return c;
}
void resetGeometryHashCache() { geometryHashCache().clear(); }

// shared_ptr overload — EmittedActor::geometry is shared so apply_emitted
// can run N instance actors through one alloc. Null (lights, subnet
// markers) → zero, the empty-geometry digest.
uint64_t geometry_dedup_hash(const std::shared_ptr<const tracey::Geometry> &g) {
    if (!g) return 0;
    auto &cache = geometryHashCache();
    const tracey::Geometry *raw = g.get();
    if (auto it = cache.find(raw); it != cache.end()) return it->second;
    const uint64_t h = geometry_dedup_hash(*g);
    cache[raw] = h;
    return h;
}

uint64_t geometry_dedup_hash(const tracey::Geometry &g) {
    uint64_t h = kSigOffset;

    auto mix_vec3_attr = [&](const tracey::Attribute<tracey::Vec3> *a, char tag) {
        if (!a) return;
        sig_mix(h, &tag, 1);
        const uint64_t n = a->data().size();
        sig_mix(h, &n, sizeof(n));
        if (n > 0) sig_mix(h, a->data().data(), n * sizeof(tracey::Vec3));
    };
    auto mix_vec2_attr = [&](const tracey::Attribute<tracey::Vec2> *a, char tag) {
        if (!a) return;
        sig_mix(h, &tag, 1);
        const uint64_t n = a->data().size();
        sig_mix(h, &n, sizeof(n));
        if (n > 0) sig_mix(h, a->data().data(), n * sizeof(tracey::Vec2));
    };

    // Positions — the BLAS-affecting field. Always present after
    // construction; if for some reason absent, the SceneObject would be
    // empty anyway and won't reach the rasterizer.
    if (const auto *P = g.points().get<tracey::Vec3>("P"))
    {
        const uint64_t n = P->data().size();
        sig_mix(h, &n, sizeof(n));
        if (n > 0) sig_mix(h, P->data().data(), n * sizeof(tracey::Vec3));
    }
    // Vertex→point topology + primitive list — same mesh data laid out
    // differently produces different SceneObject corner streams.
    const auto &v2p = g.vertexToPoint();
    const uint64_t vn = v2p.size();
    sig_mix(h, &vn, sizeof(vn));
    if (vn > 0) sig_mix(h, v2p.data(), vn * sizeof(uint32_t));
    const auto &prims = g.primitivesList();
    const uint64_t pn = prims.size();
    sig_mix(h, &pn, sizeof(pn));
    if (pn > 0) sig_mix(h, prims.data(), pn * sizeof(tracey::GeoPrimitive));
    // Shading-relevant per-corner / per-point attributes. Tag bytes
    // disambiguate "N on points" vs "N on vertices" so different storage
    // produces different hashes even with identical values.
    mix_vec3_attr(g.points().get<tracey::Vec3>("N"),  'n');
    mix_vec3_attr(g.vertices().get<tracey::Vec3>("N"), 'N');
    mix_vec2_attr(g.points().get<tracey::Vec2>("uv"),  'u');
    mix_vec2_attr(g.vertices().get<tracey::Vec2>("uv"), 'U');
    mix_vec3_attr(g.points().get<tracey::Vec3>("Cd"),  'c');
    mix_vec3_attr(g.vertices().get<tracey::Vec3>("Cd"), 'C');
    return h;
}

// Hash of just the TRS of an actor — flips on any transform tweak, stable
// otherwise. The fast path uses this to identify pure transform deltas.
uint64_t actor_transform_sig(const tracey::sops::EmittedActor &a) {
    uint64_t h = kSigOffset;
    sig_mix(h, &a.translate, sizeof(a.translate));
    sig_mix(h, &a.rotation, sizeof(a.rotation));
    sig_mix(h, &a.scale, sizeof(a.scale));
    // Instance groups fold every per-instance transform into a single
    // hash. A particle moving anywhere changes this; structural
    // count/tint changes go through actor_structural_sig instead.
    for (const auto &e : a.instances) {
        sig_mix(h, &e.translate, sizeof(e.translate));
        sig_mix(h, &e.rotation,  sizeof(e.rotation));
        sig_mix(h, &e.scale,     sizeof(e.scale));
    }
    return h;
}

// Hash of everything BUT the TRS — name, parent uid, light/subnet bits,
// material library, geometry positions. A diff in any of these forces
// the slow path (full scene rebuild).
uint64_t actor_structural_sig(const tracey::sops::EmittedActor &a) {
    uint64_t h = kSigOffset;
    sig_mix(h, &a.parentNodeUid, sizeof(a.parentNodeUid));
    sig_mix(h, &a.isSubnetMarker, sizeof(a.isSubnetMarker));
    sig_mix(h, &a.isLight, sizeof(a.isLight));
    sig_mix(h, &a.lightType, sizeof(a.lightType));
    sig_mix(h, &a.lightColor, sizeof(a.lightColor));
    sig_mix(h, &a.lightIntensity, sizeof(a.lightIntensity));
    sig_mix_str(h, a.name);
    sig_mix_str(h, a.materialLibraryName);
    // Per-instance tint folds into structural — a Cd-only template edit
    // changes only this slot but still has to invalidate the actor's
    // material so the new color reaches the materialBuffer.
    sig_mix(h, &a.hasTint, sizeof(a.hasTint));
    if (a.hasTint) sig_mix(h, &a.tint, sizeof(a.tint));
    // Object Output inline material override — structural so a slider change
    // forces the material-rebuild (slow) path; without it the per-actor diff
    // sees no change and the new material never reaches the materialBuffer.
    sig_mix(h, &a.overrideMaterial, sizeof(a.overrideMaterial));
    if (a.overrideMaterial) {
        sig_mix(h, &a.ovBaseColor, sizeof(a.ovBaseColor));
        sig_mix(h, &a.ovMetallic, sizeof(a.ovMetallic));
        sig_mix(h, &a.ovRoughness, sizeof(a.ovRoughness));
        sig_mix(h, &a.ovEmission, sizeof(a.ovEmission));
        sig_mix(h, &a.ovEmissionStrength, sizeof(a.ovEmissionStrength));
        sig_mix(h, &a.ovTransmission, sizeof(a.ovTransmission));
        sig_mix(h, &a.ovIor, sizeof(a.ovIor));
        sig_mix(h, &a.ovOpacity, sizeof(a.ovOpacity));
        sig_mix(h, &a.ovClearcoat, sizeof(a.ovClearcoat));
        sig_mix(h, &a.ovClearcoatRoughness, sizeof(a.ovClearcoatRoughness));
        sig_mix(h, &a.ovSheen, sizeof(a.ovSheen));
        sig_mix(h, &a.ovSubsurface, sizeof(a.ovSubsurface));
        sig_mix(h, &a.ovSubsurfaceColor, sizeof(a.ovSubsurfaceColor));
        sig_mix(h, &a.ovAnisotropy, sizeof(a.ovAnisotropy));
    }
    // Instance-group count + per-instance tints are INTENTIONALLY NOT
    // folded in here. A particle sim spawning/dying entries every cook
    // would otherwise flip this hash and force the slow path —
    // compile_scene rebuilding materialBuffer + per-instance UV/program
    // walks for 40K particles at 60 Hz dominated rebuild_ms. The fast
    // TRS path resizes Actor.instances in place to absorb count changes;
    // per-instance tint changes show up as stale color until the next
    // structural change (stamp / material library / etc.) — a future
    // dedicated material-refresh path closes that gap.
    // Geometry digest — same shape as the dedup hash so a VOP-driven
    // edit to Cd / N / uv (point or vertex class) shows up as a
    // structural change. Previously this hashed only positions, which
    // meant an attribute_vop writing geo_output.Cd left the apply_emitted
    // fast path thinking nothing had changed and the rasterizer never
    // saw the new colors.
    const uint64_t geo = geometry_dedup_hash(a.geometry);
    sig_mix(h, &geo, sizeof(geo));
    return h;
}
}  // namespace

// Fingerprint of an emitted-actor list. Captures only the bits that affect
// the live scene — per-actor TRS, name/parent/material, light fields, and a
// shallow digest of the geometry data — so cook outputs that are byte-equal
// to the previous cook produce the same hash and apply_emitted can short-
// circuit. Excludes anything that would always differ (no pointers; no
// per-cook ordering noise beyond the emitted-vector order).
static uint64_t emitted_signature(const std::vector<tracey::sops::EmittedActor>& emitted) {
    constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime  = 0x00000100000001b3ULL;
    uint64_t h = kFnvOffset;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= kFnvPrime; }
    };
    auto mixStr = [&](const std::string& s) {
        mix(s.data(), s.size());
        // Length-suffix so "ab" + "c" doesn't collide with "a" + "bc".
        const uint64_t n = s.size();
        mix(&n, sizeof(n));
    };
    const uint32_t count = static_cast<uint32_t>(emitted.size());
    mix(&count, sizeof(count));
    for (const auto& a : emitted) {
        mix(&a.sourceNodeUid, sizeof(a.sourceNodeUid));
        mix(&a.parentNodeUid, sizeof(a.parentNodeUid));
        mix(&a.isSubnetMarker, sizeof(a.isSubnetMarker));
        mix(&a.isLight, sizeof(a.isLight));
        mix(&a.translate, sizeof(a.translate));
        mix(&a.rotation, sizeof(a.rotation));
        mix(&a.scale, sizeof(a.scale));
        mix(&a.lightType, sizeof(a.lightType));
        mix(&a.lightColor, sizeof(a.lightColor));
        mix(&a.lightIntensity, sizeof(a.lightIntensity));
        mixStr(a.name);
        mixStr(a.materialLibraryName);
        // Instance-group payload: count + per-instance TRS + tint. A
        // particle sim where only positions change has a.translate/.../
        // .scale all stuck at identity (the group sits at origin) — so
        // without this the signature would never flip and the
        // "nothing changed" early-out would freeze the viewport.
        const uint64_t ni = a.instances.size();
        mix(&ni, sizeof(ni));
        for (const auto &e : a.instances) {
            mix(&e.translate, sizeof(e.translate));
            mix(&e.rotation,  sizeof(e.rotation));
            mix(&e.scale,     sizeof(e.scale));
            mix(&e.hasTint,   sizeof(e.hasTint));
            if (e.hasTint) mix(&e.tint, sizeof(e.tint));
        }
        // Object Output inline material override — fold in so dragging a
        // material slider flips the signature and apply_emitted actually
        // re-applies it (otherwise the "nothing changed" early-out swallows it).
        mix(&a.overrideMaterial, sizeof(a.overrideMaterial));
        if (a.overrideMaterial) {
            mix(&a.ovBaseColor, sizeof(a.ovBaseColor));
            mix(&a.ovMetallic, sizeof(a.ovMetallic));
            mix(&a.ovRoughness, sizeof(a.ovRoughness));
            mix(&a.ovEmission, sizeof(a.ovEmission));
            mix(&a.ovEmissionStrength, sizeof(a.ovEmissionStrength));
            mix(&a.ovTransmission, sizeof(a.ovTransmission));
            mix(&a.ovIor, sizeof(a.ovIor));
            mix(&a.ovOpacity, sizeof(a.ovOpacity));
            mix(&a.ovClearcoat, sizeof(a.ovClearcoat));
            mix(&a.ovClearcoatRoughness, sizeof(a.ovClearcoatRoughness));
            mix(&a.ovSheen, sizeof(a.ovSheen));
            mix(&a.ovSubsurface, sizeof(a.ovSubsurface));
            mix(&a.ovSubsurfaceColor, sizeof(a.ovSubsurfaceColor));
            mix(&a.ovAnisotropy, sizeof(a.ovAnisotropy));
        }
        // Geometry digest — defer to geometry_dedup_hash so a VOP that
        // writes Cd / N / uv (point or vertex class) flips this
        // signature and forces apply_emitted to re-run. The earlier
        // "positions only" digest predates the rasterizer consuming
        // per-vertex colors and was silently swallowing any
        // attribute-only edit.
        const uint64_t geo = geometry_dedup_hash(a.geometry);
        mix(&geo, sizeof(geo));
    }
    return h;
}

void EditorServer::apply_emitted(std::vector<tracey::sops::EmittedActor>&& emitted) {
    if (!m_engine) return;

    // The geometry-hash cache is keyed by raw Geometry pointer. Within
    // this call the `emitted` vector holds shared_ptrs alive, so pointer
    // identity is sound; clearing here prevents a previous cook's freed
    // pointers (potentially re-used by a fresh allocation this cook)
    // from returning stale hashes.
    resetGeometryHashCache();

    // Houdini-style "nothing changed" early-out. The signature covers every
    // actor-visible field — if it matches the last applied cook, no actor
    // was added, removed, repositioned, renamed, or had its geometry edited
    // since last time. Skipping scene.clear() + compile_scene + BLAS/TLAS
    // rebuild means a Cmd+Z to the same state, a re-pushed graph with no
    // semantic change, or an unrelated edit elsewhere is effectively free.
    const uint64_t sig = emitted_signature(emitted);
    if (m_has_applied_once && sig == m_last_emitted_signature
        && m_engine->compiled_scene_ready()) {
        // Nothing observable changed — but apply_animation_at downstream
        // may still want to overlay animated transforms. We don't broadcast
        // scene_changed because the actor list is byte-identical to the
        // previous cook.
        return;
    }

    // Per-actor delta classification: build fresh sig pairs for each emitted
    // actor and compare against the previous apply. If every difference vs.
    // last time is a TRS-only delta (no actor added/removed, no structural
    // change), we can keep the existing scene + BLAS / material buffers
    // intact and just refresh the TLAS — orders of magnitude cheaper than
    // a full apply_emitted + compile_scene.
    std::unordered_map<uint64_t, ActorSig> newSigs;
    newSigs.reserve(emitted.size());
    // Each entry is (composite key, index into `emitted`) so the fast path
    // can directly pull the new TRS without a linear scan when there are
    // many instance actors.
    std::vector<std::pair<uint64_t, size_t>> moved_keys;
    bool fastPathEligible =
        m_has_applied_once && m_engine->compiled_scene_ready();
    for (size_t ei = 0; ei < emitted.size(); ++ei) {
        const auto &a = emitted[ei];
        if (a.sourceNodeUid == 0) {  // root-level emit with no source node — bail to slow path
            fastPathEligible = false;
        }
        const uint64_t key = make_actor_key(a);
        ActorSig s;
        s.transform_sig  = actor_transform_sig(a);
        s.structural_sig = actor_structural_sig(a);
        newSigs[key] = s;
        if (!fastPathEligible) continue;
        auto it = m_actor_signatures.find(key);
        if (it == m_actor_signatures.end()) {
            fastPathEligible = false;  // new actor — slow path
        } else if (it->second.structural_sig != s.structural_sig) {
            fastPathEligible = false;  // geometry / material / etc. changed
        } else if (it->second.transform_sig != s.transform_sig) {
            moved_keys.push_back({key, ei});
        }
    }
    // Removed actors (in last apply but not this one) also force the slow
    // path — scene needs to drop their Actors/SceneObjects.
    if (fastPathEligible) {
        for (const auto &[k, _] : m_actor_signatures) {
            if (newSigs.find(k) == newSigs.end()) {
                fastPathEligible = false;
                break;
            }
        }
    }

    if (fastPathEligible && !moved_keys.empty()) {
        // Transform-only fast path. Walk moved keys, push the new TRS onto
        // the existing live Actor, then ask the engine to rebuild ONLY the
        // TLAS from the current scene. compile_scene's heavy lifting (BLAS,
        // material buffer, UV buffer, program lookup) all stays.
        auto &liveScene = m_engine->scene();
        for (const auto &[key, ei] : moved_keys) {
            auto actorIt = m_emitted_actor_to_actor.find(key);
            if (actorIt == m_emitted_actor_to_actor.end()) continue;
            auto *actor = liveScene.getActor(actorIt->second);
            if (!actor) continue;
            const auto &a = emitted[ei];
            if (!a.instances.empty()) {
                // Instance group: the Actor sits at identity; per-particle
                // TRS lives in each SceneInstance's localTransform.
                // Particle birth/death changes the instance count, so
                // we resize the SceneInstance vector to match — cloning
                // the last existing slot's material into new entries (per-
                // instance tints stay stale until the next structural
                // refresh, which is the documented tradeoff for keeping
                // the fast path firing every cook).
                auto &slots = actor->instances();
                const size_t newN = a.instances.size();
                if (slots.size() < newN && !slots.empty()) {
                    slots.resize(newN, slots.back());
                } else if (slots.size() > newN) {
                    slots.resize(newN);
                }
                const size_t n = std::min(slots.size(), newN);
                // Per-slot writes touch only their own index — safe to
                // chunk across worker threads. At 400k particles this
                // drops the serial ~5–10 ms write loop to <1 ms on the
                // M3 Ultra. parallel_for_chunks falls back to a serial
                // body below ~1k entries so small scenes don't pay
                // thread-spawn overhead.
                const auto &entries = a.instances;
                tracey::parallel_for_chunks(n,
                    [&entries, &slots](size_t begin, size_t end) {
                        for (size_t i = begin; i < end; ++i) {
                            const auto &e = entries[i];
                            tracey::Transform xf;
                            xf.setPosition(e.translate);
                            xf.setRotation(tracey::Quaternion(e.rotation.x, e.rotation.y,
                                                               e.rotation.z, e.rotation.w));
                            xf.setScale(e.scale);
                            slots[i].setLocalTransform(xf);
                        }
                    });
            } else {
                tracey::Transform xf;
                xf.setPosition(a.translate);
                xf.setRotation(tracey::Quaternion(a.rotation.x, a.rotation.y,
                                                   a.rotation.z, a.rotation.w));
                xf.setScale(a.scale);
                actor->setTransform(xf);
            }
        }
        if (m_engine->refresh_tlas_only()) {
            m_actor_signatures = std::move(newSigs);
            m_last_emitted_signature = sig;
            m_has_applied_once = true;
            m_clear_next_frame = true;
            if (m_broadcast) m_broadcast(R"({"event":"scene_changed"})");
            // Animation overlay still wants to re-run after a transform
            // update so any animated channels write fresh values on top.
            m_timeline_dirty = true;
            return;
        }
        // refresh_tlas_only returned false (topology drifted, e.g. a hidden
        // actor flipped visible between compile and apply). Fall through
        // to the slow path.
    }

    m_last_emitted_signature = sig;
    m_has_applied_once = true;

    // The camera state is independent of the SOP graph (driven by user
    // fly-through) so we preserve it across cooks; otherwise every parameter
    // tweak would snap the viewport back to the default angle.
    auto& scene = m_engine->scene();

    // Incremental slow path: rather than `scene.clear()` + reconstruct
    // from scratch, diff vs. m_actor_signatures and only recreate the
    // actors / SceneObjects whose structural_sig changed. Unchanged
    // actors keep their existing Actor + SceneObject + child links, so
    // we skip the GeometryConverter::toSceneObject vertex-copy and the
    // Scene::createActor allocation for them entirely. The BLAS cache
    // handles BVH reuse on the compile_scene side.
    //
    // Removed actors (uid present in m_actor_signatures but not in
    // newSigs) get scene.removeActor + scene.removeObject. Added actors
    // (key present in newSigs but not in m_actor_signatures) take the
    // create path below. Structural-change actors are remove-then-create.
    std::unordered_set<uint64_t> newKeys;
    std::unordered_set<size_t> newUids;
    newKeys.reserve(emitted.size());
    for (const auto &a : emitted) {
        newKeys.insert(make_actor_key(a));
        newUids.insert(a.sourceNodeUid);
    }

    // Helper: drop the SceneObject reference owned by `key` from the
    // refcount + content-hash table. Removes the SceneObject from the live
    // scene only when no other actor still references it (refcount → 0).
    auto release_object_for_key = [&](uint64_t key) {
        auto objIt = m_sop_node_object_names.find(key);
        if (objIt == m_sop_node_object_names.end()) return;
        const std::string name = objIt->second;
        m_sop_node_object_names.erase(objIt);
        auto rcIt = m_scene_object_refcount.find(name);
        if (rcIt == m_scene_object_refcount.end()) return;
        if (--rcIt->second > 0) return;
        m_scene_object_refcount.erase(rcIt);
        scene.removeObject(name);
        // Drop the reverse content-hash entry that pointed at this name.
        for (auto h2n = m_geometry_hash_to_object_name.begin();
             h2n != m_geometry_hash_to_object_name.end();)
        {
            if (h2n->second == name) h2n = m_geometry_hash_to_object_name.erase(h2n);
            else ++h2n;
        }
    };

    // Iterate the *old* m_actor_signatures (still holds the previous
    // cook's set) and prune anything that isn't in this cook's newKeys.
    // This is where actors from a deleted SOP (e.g. removing an Instance
    // SOP that had emitted N TLAS instances) get torn down. The new
    // signature map is swapped in AFTER this diff so we still have the
    // old set to walk against.
    for (auto it = m_actor_signatures.begin(); it != m_actor_signatures.end();)
    {
        if (newKeys.find(it->first) != newKeys.end()) { ++it; continue; }
        auto actorIt = m_emitted_actor_to_actor.find(it->first);
        if (actorIt != m_emitted_actor_to_actor.end()) {
            scene.removeActor(actorIt->second);
            // Also drop the convenience sourceNodeUid→actor mapping when
            // the removed actor is the primary (instanceIndex==0).
            const size_t uid = static_cast<size_t>(it->first >> 24);
            const uint32_t inst = static_cast<uint32_t>(it->first & 0xFFFFFFULL);
            if (inst == 0) m_sop_node_to_actor.erase(uid);
            m_emitted_actor_to_actor.erase(actorIt);
        }
        release_object_for_key(it->first);
        // Only forget the per-uid visibility flag when *every* actor for
        // that uid is gone (instance SOPs share one visibility flag across
        // all their instances).
        const size_t uid = static_cast<size_t>(it->first >> 24);
        if (newUids.find(uid) == newUids.end()) {
            m_sop_node_visible.erase(uid);
        }
        it = m_actor_signatures.erase(it);
    }
    // NOTE: m_actor_signatures is intentionally NOT swapped here yet —
    // the per-actor create loop below still needs to look up each new
    // key's *previous* signature to decide between "unchanged → skip",
    // "transform-only → just retarget", and "structural change →
    // remove-and-recreate". The swap happens after the create loop.

    // Pass 1: create one Actor per emit. Subnet markers create transform-only
    // actors (no SceneObject, no SceneInstance, no material) — they're parent
    // transform nodes in the scene tree. SopGraph::cook() pushes subnet
    // markers before their children, so the parent uid is already in
    // m_sop_node_to_actor when pass 2 wires the parent links.
    // Helper: restore the per-actor display flag for the actor that was just
    // emitted from this SOP node. Default visible when the map has no entry.
    auto restoreVisibility = [&](tracey::Actor* actor, size_t sourceNodeUid) {
        if (!actor || sourceNodeUid == 0) return;
        auto it = m_sop_node_visible.find(sourceNodeUid);
        if (it != m_sop_node_visible.end()) actor->setVisible(it->second);
    };

    // ── glTF material / texture forwarding ──────────────────────────────
    // gltf_import SOPs stamp the source path + mesh name as Detail attrs on
    // the geometry. We use those here to (a) copy the source's embedded
    // textures into the live scene under the same "embedded:N" ids the
    // material refers to, and (b) pull the matching SceneInstance's
    // MaterialInstance and apply it to the live actor instead of the
    // flat-grey default. Without this an imported glTF renders untextured.
    //
    // Cache by path so a multi-primitive glTF only re-parses once per cook
    // apply. The cache is local to this call — released when apply_emitted
    // returns, so we don't pin loaded scenes between cooks.
    auto getSourceScene = [&](const std::string& path)
        -> std::shared_ptr<const tracey::Scene> {
        if (path.empty()) return nullptr;
        try {
            // Shares the process-wide cache with GltfImportSop::cook, so the
            // editor never re-parses a file just to recover its materials.
            return tracey::GltfLoader::loadFromFileCached(path);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[apply_emitted] glTF material load failed for %s: %s\n",
                path.c_str(), e.what());
            return nullptr;
        }
    };

    auto pullGltfMaterial = [&](const tracey::Geometry& geo,
                                tracey::MaterialInstance* out) -> bool {
        const auto* pathAttr = geo.detail().get<std::string>("_gltf_source_path");
        const auto* meshAttr = geo.detail().get<std::string>("_gltf_source_mesh");
        if (!pathAttr || !meshAttr) return false;
        if (pathAttr->data().empty() || meshAttr->data().empty()) return false;
        const std::string& path = pathAttr->data()[0];
        const std::string& meshName = meshAttr->data()[0];
        auto src = getSourceScene(path);
        if (!src) return false;

        // Mirror every embedded texture id from the source into the live
        // scene so the renderer's `getEmbeddedTexture("embedded:N")` lookup
        // resolves. Scene::addEmbeddedTexture is idempotent on duplicate ids
        // — repeat copies for shared meshes are harmless.
        for (const auto& [id, tex] : src->embeddedTextures()) {
            if (!scene.hasEmbeddedTexture(id)) {
                tracey::EmbeddedTexture copy = tex;
                scene.addEmbeddedTexture(id, std::move(copy));
            }
        }

        // Walk source actors → find the SceneInstance whose objectRef
        // matches our mesh name; that instance carries the glTF material.
        for (const auto& a : src->actors()) {
            if (!a) continue;
            for (const auto& inst : a->instances()) {
                if (inst.objectRef() == meshName) {
                    *out = inst.material();
                    return true;
                }
            }
        }
        return false;
    };

#ifdef TRACEY_HAS_USD
    // USD twin of pullGltfMaterial: usd_import SOPs stamp _usd_source_path /
    // _usd_source_mesh; the bound UsdPreviewSurface material lives on the
    // cached stage's matching SceneInstance (objectRef == the prim path), so
    // the same actor-walk recovers it. Guarded — tracey_usd is only linked in
    // USD-enabled editor builds. (USD textures aren't embedded into the Scene
    // yet, so there's no embedded-texture mirror here — a U1.3 follow-up.)
    auto pullUsdMaterial = [&](const tracey::Geometry& geo,
                               tracey::MaterialInstance* out) -> bool {
        const auto* pathAttr = geo.detail().get<std::string>("_usd_source_path");
        const auto* meshAttr = geo.detail().get<std::string>("_usd_source_mesh");
        if (!pathAttr || !meshAttr) return false;
        if (pathAttr->data().empty() || meshAttr->data().empty()) return false;
        const std::string& path = pathAttr->data()[0];
        const std::string& meshName = meshAttr->data()[0];
        auto src = tracey::UsdLoader::loadFromFileCached(path);
        if (!src) return false;
        for (const auto& a : src->actors()) {
            if (!a) continue;
            for (const auto& inst : a->instances()) {
                if (inst.objectRef() == meshName) {
                    *out = inst.material();
                    return true;
                }
            }
        }
        return false;
    };
#endif

    // Composite keys whose Actor got newly created (or recreated after a
    // structural change) in this pass. Used by Pass 2 below to gate parent
    // re-wiring — addChild isn't idempotent, so re-running it on unchanged
    // actors would push duplicate child uids.
    std::unordered_set<uint64_t> recreatedKeys;

    for (const auto& ea : emitted) {
        const uint64_t actorKey = make_actor_key(ea);
        // Skip-when-unchanged: if this composite key already has a live
        // actor in the scene with the same structural_sig, the actor's
        // SceneObject / material / instance bookkeeping is still valid.
        // We only need to update its transform if transform_sig differs —
        // compile_scene below picks up actor->transform() into the new
        // TLAS.
        const auto sigIt = m_actor_signatures.find(actorKey);
        const ActorSig &newSig = newSigs[actorKey];
        if (sigIt != m_actor_signatures.end() &&
            sigIt->second.structural_sig == newSig.structural_sig)
        {
            if (sigIt->second.transform_sig != newSig.transform_sig)
            {
                auto actorIt = m_emitted_actor_to_actor.find(actorKey);
                if (actorIt != m_emitted_actor_to_actor.end()) {
                    auto *actor = scene.getActor(actorIt->second);
                    if (actor) {
                        if (!ea.instances.empty()) {
                            // Instance group — resize + update per-particle
                            // local transforms in place. Mirror of the
                            // global fast path branch.
                            auto &slots = actor->instances();
                            const size_t newN = ea.instances.size();
                            if (slots.size() < newN && !slots.empty()) {
                                slots.resize(newN, slots.back());
                            } else if (slots.size() > newN) {
                                slots.resize(newN);
                            }
                            const size_t n = std::min(slots.size(), newN);
                            const auto &entries = ea.instances;
                            tracey::parallel_for_chunks(n,
                                [&entries, &slots](size_t begin, size_t end) {
                                    for (size_t i = begin; i < end; ++i) {
                                        const auto &e = entries[i];
                                        tracey::Transform xf;
                                        xf.setPosition(e.translate);
                                        xf.setRotation(tracey::Quaternion(e.rotation.x, e.rotation.y,
                                                                           e.rotation.z, e.rotation.w));
                                        xf.setScale(e.scale);
                                        slots[i].setLocalTransform(xf);
                                    }
                                });
                        } else {
                            tracey::Transform xf;
                            xf.setPosition(ea.translate);
                            xf.setRotation(tracey::Quaternion(ea.rotation.x, ea.rotation.y,
                                                               ea.rotation.z, ea.rotation.w));
                            xf.setScale(ea.scale);
                            actor->setTransform(xf);
                        }
                    }
                }
            }
            continue;
        }

        // Structural change OR brand-new key: tear down any prior actor +
        // release this key's SceneObject reference (the shared SceneObject
        // only drops out of the scene when no other actor still uses it).
        {
            auto actorIt = m_emitted_actor_to_actor.find(actorKey);
            if (actorIt != m_emitted_actor_to_actor.end()) {
                scene.removeActor(actorIt->second);
                if (ea.instanceIndex == 0) m_sop_node_to_actor.erase(ea.sourceNodeUid);
                m_emitted_actor_to_actor.erase(actorIt);
            }
            release_object_for_key(actorKey);
        }
        recreatedKeys.insert(actorKey);

        if (ea.isSubnetMarker) {
            auto* actor = scene.createActor();
            actor->setName(ea.name.empty() ? "subnet" : ea.name);
            tracey::Transform xform;
            xform.setPosition(ea.translate);
            xform.setRotation(tracey::Quaternion(ea.rotation.x, ea.rotation.y,
                                                 ea.rotation.z, ea.rotation.w));
            xform.setScale(ea.scale);
            actor->setTransform(xform);
            if (ea.sourceNodeUid != 0) {
                m_emitted_actor_to_actor[actorKey] = actor->getUid();
                if (ea.instanceIndex == 0)
                    m_sop_node_to_actor[ea.sourceNodeUid] = actor->getUid();
            }
            restoreVisibility(actor, ea.sourceNodeUid);
            continue;
        }

        if (ea.isLight) {
            // Houdini-style /obj light. Transform-only actor with a Light
            // component attached; no SceneObject, no SceneInstance. The
            // SceneCompiler picks light actors up into a separate light
            // list later in the wavefront SSBO bind.
            auto* actor = scene.createActor();
            actor->setName(ea.name.empty() ? "light" : ea.name);
            tracey::Transform xform;
            xform.setPosition(ea.translate);
            xform.setRotation(tracey::Quaternion(ea.rotation.x, ea.rotation.y,
                                                 ea.rotation.z, ea.rotation.w));
            xform.setScale(ea.scale);
            actor->setTransform(xform);

            tracey::Light light;
            light.type = (ea.lightType == 1) ? tracey::LightType::Distant
                                             : tracey::LightType::Point;
            light.color = ea.lightColor;
            light.intensity = ea.lightIntensity;
            actor->setLight(light);

            if (ea.sourceNodeUid != 0) {
                m_emitted_actor_to_actor[actorKey] = actor->getUid();
                if (ea.instanceIndex == 0)
                    m_sop_node_to_actor[ea.sourceNodeUid] = actor->getUid();
            }
            restoreVisibility(actor, ea.sourceNodeUid);
            continue;
        }

        // Each emitted actor's geometry becomes a SceneObject — but only if
        // we haven't already added an identical Geometry in this process.
        // Two emit sources with the same vertex content share ONE
        // SceneObject (and therefore one vertex buffer, color buffer, and
        // BLAS), bumping the refcount so the SceneObject only goes away
        // when the last referencing actor does.
        if (!ea.geometry) continue;  // defensive: real-geo branch guards above
                                     // should have already skipped lights /
                                     // subnet markers.
        const uint64_t geomHash = geometry_dedup_hash(ea.geometry);
        std::string objectName;
        auto dedupIt = m_geometry_hash_to_object_name.find(geomHash);
        if (dedupIt != m_geometry_hash_to_object_name.end() &&
            scene.hasObject(dedupIt->second))
        {
            // Cache hit: reuse the existing SceneObject. We deliberately
            // re-verify it's still in the live scene — a previous teardown
            // could have evicted it but left the hash entry behind in
            // pathological cases.
            objectName = dedupIt->second;
        }
        else
        {
            // Fresh content: build a new SceneObject. Suffix the uid so
            // two distinct-content actors with the same display name (e.g.
            // multiple `Body_prim_0`s) don't clobber each other in
            // scene.m_objects.
            objectName = (ea.name.empty() ? std::string{"actor"} : ea.name) +
                         "_" + std::to_string(ea.sourceNodeUid);
            scene.addObject(objectName, tracey::GeometryConverter::toSceneObject(*ea.geometry, objectName));
            m_geometry_hash_to_object_name[geomHash] = objectName;
        }
        m_sop_node_object_names[actorKey] = objectName;
        ++m_scene_object_refcount[objectName];

        auto* actor = scene.createActor();
        actor->setName(objectName);

        tracey::Transform xform;
        xform.setPosition(ea.translate);
        // ea.rotation is a quaternion (wxyz); v1 cook emits identity but we
        // honour it if the codegen path or a future TransformParam fills it in.
        xform.setRotation(tracey::Quaternion(ea.rotation.x, ea.rotation.y,
                                             ea.rotation.z, ea.rotation.w));
        xform.setScale(ea.scale);
        actor->setTransform(xform);

        // If the user assigned a material library to this output, attach the
        // graph JSON now so the next compile_scene picks it up. Track the
        // library name on the Actor too so the save-material handler can
        // refresh just the bound actors without re-cooking the SOP graph.
        if (!ea.materialLibraryName.empty() &&
            is_safe_library_name(ea.materialLibraryName)) {
            // resolve_material_path checks the project's local
            // materials folder first and falls back to the global
            // library. A project that ships its own copy of a
            // same-named material now shadows the global one.
            std::ifstream in(resolve_material_path(ea.materialLibraryName));
            if (in) {
                std::stringstream ss;
                ss << in.rdbuf();
                actor->setMaterialGraphJson(ss.str());
                actor->setMaterialLibraryName(ea.materialLibraryName);
            }
        } else {
            // Empty SOP-side material name means "no graph assigned" —
            // clear both the library name AND the graph JSON. Leaving
            // the graph behind would let a stale material from a
            // previous cook keep applying after the user has cleared
            // the assignment, even though the SOP graph no longer
            // references it.
            actor->setMaterialGraphJson({});
            actor->setMaterialLibraryName({});
        }

        // Default material instance — replaced below by the glTF source's
        // material when the geometry carries the `_gltf_source_*` tags.
        tracey::MaterialInstance mat("pbr");
        mat.setAlbedo(tracey::Vec3(0.8f));
        mat.setMetallic(0.0f);
        mat.setRoughness(0.5f);
        // Honour the user's library override (set above via
        // setMaterialGraphJson) by leaving the flat default in place — the
        // material graph wins. Otherwise, prefer the glTF-source material so
        // imported actors render with their textures + factors.
        if (ea.materialLibraryName.empty() ||
            !is_safe_library_name(ea.materialLibraryName)) {
            tracey::MaterialInstance fromGltf("pbr");
            if (pullGltfMaterial(*ea.geometry, &fromGltf)) {
                mat = fromGltf;
            }
#ifdef TRACEY_HAS_USD
            // USD-imported actors carry _usd_source_* instead — pull the bound
            // UsdPreviewSurface material the same way.
            tracey::MaterialInstance fromUsd("pbr");
            if (pullUsdMaterial(*ea.geometry, &fromUsd)) {
                mat = fromUsd;
            }
#endif
        }
        // Per-instance albedo tint (Phase C of GPU instancing). When the
        // upstream cook attached a tint — e.g. `instance` SOP forwarding
        // a per-point Cd from the template — multiply it into the base
        // material albedo. Each TLAS instance gets its own materialBuffer
        // entry, so 1000 instances sharing one BLAS still get 1000
        // distinct colors. Skip when a material library is in play — the
        // material graph already controls albedo through its own knobs.
        const bool tintAllowed = ea.materialLibraryName.empty() ||
                                  !is_safe_library_name(ea.materialLibraryName);
        if (ea.hasTint && tintAllowed) {
            const tracey::Vec3 base = mat.albedo().value_or(tracey::Vec3(1.0f));
            mat.setAlbedo(tracey::Vec3(base.x * ea.tint.x,
                                        base.y * ea.tint.y,
                                        base.z * ea.tint.z));
        }

        // Object Output inline material override — the output node's factor
        // params drive the material (sliders, keyframable) when the user
        // toggled it on. Wins over the glTF/SOP source + tint; the material
        // library graph still takes precedence at hit time when assigned.
        if (ea.overrideMaterial) {
            mat.setAlbedo(ea.ovBaseColor);
            mat.setMetallic(ea.ovMetallic);
            mat.setRoughness(ea.ovRoughness);
            mat.setEmission(ea.ovEmission);
            mat.setFloat("emissionStrength", ea.ovEmissionStrength);
            mat.setFloat("transmission", ea.ovTransmission);
            mat.setFloat("ior", ea.ovIor);
            mat.setFloat("opacity", ea.ovOpacity);
            mat.setFloat("clearcoat", ea.ovClearcoat);
            mat.setFloat("clearcoatRoughness", ea.ovClearcoatRoughness);
            mat.setFloat("sheen", ea.ovSheen);
            mat.setFloat("subsurface", ea.ovSubsurface);
            mat.setVec3("subsurfaceColor", ea.ovSubsurfaceColor);
            mat.setFloat("anisotropy", ea.ovAnisotropy);
        }

        if (!ea.instances.empty()) {
            // Instance-group emit: one Actor sitting at identity, N
            // SceneInstances each carrying its own local transform and
            // per-particle tinted material. compile_scene's per-instance
            // loop walks these to build N TLAS entries pointing at the
            // shared BLAS, exactly the GPU-instancing topology we want.
            // The Actor's own transform stays identity (set above to
            // ea.translate/.../scale, which are also identity for an
            // instance emit — the cook side leaves them at defaults).
            for (const auto &e : ea.instances) {
                tracey::MaterialInstance imat = mat;
                if (e.hasTint && tintAllowed) {
                    const tracey::Vec3 base = imat.albedo().value_or(tracey::Vec3(1.0f));
                    imat.setAlbedo(tracey::Vec3(base.x * e.tint.x,
                                                base.y * e.tint.y,
                                                base.z * e.tint.z));
                }
                tracey::SceneInstance si(objectName, imat);
                tracey::Transform xf;
                xf.setPosition(e.translate);
                xf.setRotation(tracey::Quaternion(e.rotation.x, e.rotation.y,
                                                   e.rotation.z, e.rotation.w));
                xf.setScale(e.scale);
                si.setLocalTransform(xf);
                actor->addInstance(std::move(si));
            }
        } else {
            actor->addInstance(tracey::SceneInstance(objectName, mat));
        }

        // Stable actor↔SOP mapping via the composite key threaded through
        // EmittedActor. The actor we just appended is at the back of
        // scene.actors().
        if (ea.sourceNodeUid != 0 && !scene.actors().empty()) {
            const uint64_t newActorUid = scene.actors().back()->getUid();
            m_emitted_actor_to_actor[actorKey] = newActorUid;
            if (ea.instanceIndex == 0)
                m_sop_node_to_actor[ea.sourceNodeUid] = newActorUid;
        }
        restoreVisibility(actor, ea.sourceNodeUid);
    }

    // Pass 2: wire parent → child edges for recreated actors only. Both
    // directions are stored so Scene::flatten() can walk top-down without
    // a per-actor reverse scan. Unchanged actors keep their existing
    // parent/child links — addChild isn't idempotent, so re-running it
    // every cook on those would duplicate child uids. Parenting is
    // expressed in SOP-node terms (parentNodeUid → sourceNodeUid), so we
    // use the SOP→primary-actor map on both sides.
    for (const auto& ea : emitted) {
        if (ea.parentNodeUid == 0) continue;
        const uint64_t actorKey = make_actor_key(ea);
        if (recreatedKeys.find(actorKey) == recreatedKeys.end()) continue;
        const auto childIt = m_emitted_actor_to_actor.find(actorKey);
        const auto parentIt = m_sop_node_to_actor.find(ea.parentNodeUid);
        if (childIt == m_emitted_actor_to_actor.end() ||
            parentIt == m_sop_node_to_actor.end()) {
            continue;
        }
        auto* child = scene.getActor(childIt->second);
        auto* parent = scene.getActor(parentIt->second);
        if (!child || !parent) continue;
        parent->addChild(child);
        child->setParent(parent->getUid());
    }

    // Both diff passes have read m_actor_signatures (previous state) and
    // newSigs (this cook's state). Promote newSigs to the canonical
    // m_actor_signatures now that we're done diffing.
    m_actor_signatures = std::move(newSigs);

    // Recompile so the path tracer picks up the new BLAS/TLAS + material
    // programs, and reset accumulation.
    if (m_engine->path_tracer_ready()) {
        m_engine->compile_scene();
        m_clear_next_frame = true;
    }

    if (m_broadcast) {
        m_broadcast(R"({"event":"scene_changed"})");
    }

    // Recompute whether the graph has any attribute_vop with animated
    // promoted host params. Drives the auto re-cook on time change in
    // render_tick.
    m_has_animated_sop_params = detect_animated_sop_params();

    // Same idea for dop_import — when present, every playhead move needs
    // to cook the DOP forward and re-stamp + re-cook the SOP graph.
    m_has_dop_imports = detect_dop_imports();

    // Cook reset every actor's transform to its SOP-constant baseline; the
    // next render_tick re-evaluates animated overrides on top.
    m_timeline_dirty = true;
}

// ── Timeline / animation override ──────────────────────────────────────────


// Step the playhead forward (or backward, in PingPong) by `dt` seconds,
// applying the active LoopMode. Returns true when current_time actually
// changed. Shared between the wall-clock async path (called from
// render_tick with dt = elapsed wall time) and the frame-locked path
// (called from drain_cook_result with dt = 1/fps once a cook lands).
bool EditorServer::advance_playhead_by(double dt) {
    if (!m_timeline.playing || dt <= 0.0) return false;
    const double t0 = range_start_seconds(m_timeline.frame_start, m_timeline.fps);
    const double t1 = range_end_seconds(m_timeline.frame_end, m_timeline.fps);
    const double range = std::max(t1 - t0, 1.0 / std::max(m_timeline.fps, 1.0));

    double t = m_timeline.current_time + dt * m_timeline.pingpong_dir;

    switch (m_timeline.loop) {
        case LoopMode::Once:
            if (t >= t1) {
                t = t1;
                m_timeline.playing = false;
            } else if (t < t0) {
                t = t0;
            }
            break;
        case LoopMode::Loop: {
            if (t >= t1 || t < t0) {
                double off = std::fmod(t - t0, range);
                if (off < 0.0) off += range;
                t = t0 + off;
            }
            break;
        }
        case LoopMode::PingPong: {
            if (t >= t1) {
                t = t1 - (t - t1);
                m_timeline.pingpong_dir = -1.0;
            } else if (t < t0) {
                t = t0 + (t0 - t);
                m_timeline.pingpong_dir = 1.0;
            }
            break;
        }
    }
    if (t == m_timeline.current_time) return false;
    m_timeline.current_time = t;
    return true;
}

bool EditorServer::advance_playhead(double dt) {
    // Frame-locked playback advances inside drain_cook_result instead,
    // one frame at a time after each cook completion. Wall-clock dt
    // would skip frames whenever the cook is slower than realtime, so
    // we suppress it here.
    if (m_timeline.frame_locked) return false;
    return advance_playhead_by(dt);
}

// Walk the actor↔SOP map, evaluate animated translate/rotate/scale params at
// `time`, and override the corresponding actor transforms. The path tracer +
// rasterizer both render from the cached `m_compiled_scene` (TLAS instance
// transforms are baked at compile-time), so we must recompile after touching
// any transform — the same way the inspector slider path does it (App.tsx →
// set_actor_transform → compileScene).
//
// Rotation is stored on the SOP node as per-axis euler-degrees and converted
// to quaternion (ZYX intrinsic, matching transform_sop and the cook emit
// path) here. Per-component channels animate naturally — same shape as
// translate/scale — at the cost of gimbal lock at extremes (Houdini lives
// with this and so do we for v1).
//
// Other animated params (primitive sizes, etc.) need a full re-cook to take
// effect, not just a recompile; for v1 we only handle object_output / subnet
// transforms.
void EditorServer::apply_animation_at(double time) {
    if (!m_sop_graph || !m_engine) return;

    bool any_changed = false;
    for (const auto& [outputUid, actorUid] : m_sop_node_to_actor) {
        auto* node = findNodeRecursive(m_sop_graph.get(),outputUid);
        if (!node) continue;
        auto* actor = m_engine->scene().getActor(actorUid);
        if (!actor) continue;

        const tracey::sops::Parameter* pT = nullptr;
        const tracey::sops::Parameter* pR = nullptr;
        const tracey::sops::Parameter* pS = nullptr;
        for (const auto& p : node->parameters()) {
            if      (p.name == "translate")        pT = &p;
            else if (p.name == "rotate_euler_deg") pR = &p;
            else if (p.name == "scale")            pS = &p;
        }
        const bool tAnim = pT && pT->isAnimated();
        const bool rAnim = pR && pR->isAnimated();
        const bool sAnim = pS && pS->isAnimated();
        if (!tAnim && !rAnim && !sAnim) continue;

        tracey::Transform xf = actor->transform();
        if (tAnim) {
            auto v = pT->evaluateAt(time);
            if (auto* vec = std::get_if<tracey::Vec3>(&v)) xf.setPosition(*vec);
        }
        if (rAnim) {
            auto v = pR->evaluateAt(time);
            if (auto* deg = std::get_if<tracey::Vec3>(&v)) {
                constexpr float kDeg2Rad = 3.1415926535f / 180.0f;
                const tracey::Vec3 rad = *deg * kDeg2Rad;
                glm::quat qx = glm::angleAxis(rad.x, glm::vec3(1, 0, 0));
                glm::quat qy = glm::angleAxis(rad.y, glm::vec3(0, 1, 0));
                glm::quat qz = glm::angleAxis(rad.z, glm::vec3(0, 0, 1));
                xf.setRotation(qz * qy * qx);
            }
        }
        if (sAnim) {
            auto v = pS->evaluateAt(time);
            if (auto* vec = std::get_if<tracey::Vec3>(&v)) xf.setScale(*vec);
        }
        actor->setTransform(xf);
        any_changed = true;
    }
    if (any_changed) {
        m_clear_next_frame = true;
        // Rebuild the TLAS with the new instance transforms. Without this the
        // path tracer + rasterizer keep rendering the cached compile-time
        // transforms and the override is invisible.
        if (m_engine->path_tracer_ready() && m_engine->compiled_scene_ready()) {
            try {
                m_engine->compile_scene();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[anim] compile_scene failed: %s\n", e.what());
            }
        }
    }
}

// Walks the canonical SOP graph (recursing into subnet inner graphs) and
// returns true if any node carries an animated parameter that needs a real
// per-playhead re-cook. The exception set: translate/rotate/scale on the
// emit-side nodes (object_output, subnet, light) — those are applied to the
// live Actors by apply_animation_at without re-cooking, so a graph whose
// ONLY animation is output-node TRS stays on that fast path. Everything
// else (effector strength, scatter count, attribute_vop promotions, …)
// flips the auto re-cook on. Called from apply_emitted after every cook.
bool EditorServer::detect_animated_sop_params() const {
    if (!m_sop_graph) return false;
    auto isEmitTrsFastPath = [](const tracey::sops::SopNode& n,
                                const tracey::sops::Parameter& p) {
        const std::string k = n.kind();
        const bool emitNode = (k == "object_output" || k == "subnet" || k == "light");
        if (!emitNode) return false;
        return p.name == "translate" || p.name == "rotate_euler_deg" ||
               p.name == "scale";
    };
    std::function<bool(const tracey::sops::SopGraph*)> walk;
    walk = [&](const tracey::sops::SopGraph* g) -> bool {
        if (!g) return false;
        for (const auto& n : g->nodes()) {
            auto* sn = dynamic_cast<const tracey::sops::SopNode*>(n.get());
            if (!sn) continue;
            for (const auto& p : sn->parameters()) {
                if (p.isAnimated() && !isEmitTrsFastPath(*sn, p)) return true;
            }
            if (sn->innerGraph() && walk(sn->innerGraph())) return true;
        }
        return false;
    };
    return walk(m_sop_graph.get());
}

// True iff the canonical graph carries a dop_import SOP anywhere (including
// inside subnets). The playhead-driven DOP re-cook in render_tick reads
// this so projects without particles don't pay the cost on every scrub.
bool EditorServer::detect_dop_imports() const {
    if (!m_sop_graph) return false;
    std::function<bool(const tracey::sops::SopGraph*)> walk;
    walk = [&](const tracey::sops::SopGraph* g) -> bool {
        if (!g) return false;
        for (const auto& n : g->nodes()) {
            auto* sn = dynamic_cast<const tracey::sops::SopNode*>(n.get());
            if (!sn) continue;
            if (sn->kind() == "dop_import") return true;
            if (sn->innerGraph() && walk(sn->innerGraph())) return true;
        }
        return false;
    };
    return walk(m_sop_graph.get());
}

// Cook the DopGraph to the frame matching `time` (cheap when already
// cached), then walk the SOP graph collecting (uid, current-frame
// geometry) pairs for every dop_import SOP. Caller must hold m_mutex.
// Returns empty when there's nothing to stamp.
std::vector<std::pair<size_t, tracey::Geometry>>
EditorServer::collect_dop_stamps(double time) {
    std::vector<std::pair<size_t, tracey::Geometry>> out;
    if (!m_dop_graph || !m_sop_graph || !m_has_dop_imports) return out;

    // 1-based frame index. time=0 → frame 1, etc. Match the convention
    // in DopGraph::cookOneFrame (header.time = (frameIdx - 1) / fps).
    //
    // The epsilon nudge is load-bearing: the video export computes
    // `frame_time = (f - 1) / fps`, then we recover the frame index via
    // `floor(frame_time * fps) + 1`. In IEEE-754 doubles `(1/24)*24`
    // rounds to ~0.99999…, not 1.0, so the naive floor() gives 0 and
    // every export frame past the first reads frame 1's sim state —
    // i.e. the entire exported video shows the same first-frame
    // particle snapshot. The live viewport doesn't hit this because
    // the playhead time is set continuously by the scrub, not derived
    // from an integer frame.
    const double fps = std::max(m_timeline.fps, 1e-6);
    const int frame_idx = std::max(1,
        static_cast<int>(std::floor(time * fps + 1e-6)) + 1);

    // Scrubbing backward (or graph edited) requires re-sim from the
    // empty baseline. cookToFrame is monotonic; clear first when we're
    // landing in the past.
    const int prev_cached = m_dop_graph->cachedToFrame();
    if (frame_idx < prev_cached) {
        m_dop_graph->clearCache();
    }
    m_dop_graph->cookToFrame(frame_idx, fps);

    // Broadcast cache-extent change so the dopesheet UI can draw a
    // "cached up to frame N" indicator. Cheap: only fires when the
    // extent actually moved.
    const int new_cached = m_dop_graph->cachedToFrame();
    if (m_broadcast && new_cached != prev_cached) {
        json msg = {
            {"event", "dop_status"},
            {"cached_to_frame", new_cached},
            {"current_frame",   frame_idx},
        };
        m_broadcast(msg.dump());
    }

    const tracey::dops::SimState* state = m_dop_graph->frame(frame_idx);
    if (!state) return out;

    // Collect uids of every dop_import node in m_sop_graph, recursing.
    std::vector<size_t> uids;
    std::function<void(const tracey::sops::SopGraph*)> walk;
    walk = [&](const tracey::sops::SopGraph* g) {
        if (!g) return;
        for (const auto& n : g->nodes()) {
            auto* sn = dynamic_cast<const tracey::sops::SopNode*>(n.get());
            if (!sn) continue;
            if (sn->kind() == "dop_import") uids.push_back(sn->uid());
            if (sn->innerGraph()) walk(sn->innerGraph());
        }
    };
    walk(m_sop_graph.get());

    out.reserve(uids.size());
    for (size_t uid : uids) {
        // Same Geometry payload for every importer in v1 — Phase N could
        // add a "which actor / which DOP network" selector and split here.
        out.emplace_back(uid, state->geometry);
    }
    return out;
}

// True if any SOP node anywhere in the graph carries an animated parameter
// (translate/rotate/scale on a subnet, an animated VOP promotion, a
// keyframed scalar on any leaf, etc.). Used by the export loop to decide
// whether per-frame state can be skipped on a static scene.
bool EditorServer::detect_any_animation() const {
    if (!m_sop_graph) return false;
    // A DOP-driven scene is animated even if no SOP parameter is keyed —
    // particle state changes every frame from the simulation itself.
    // Without this, the video export's `scene_static` short-circuit
    // skips the per-frame re-cook and every frame after the first
    // renders with stale dop_import geometry — the "exported video
    // freezes at frame 1" symptom.
    if (m_has_dop_imports) return true;
    std::function<bool(const tracey::sops::SopGraph*)> walk;
    walk = [&](const tracey::sops::SopGraph* g) -> bool {
        if (!g) return false;
        for (const auto& n : g->nodes()) {
            auto* sn = dynamic_cast<const tracey::sops::SopNode*>(n.get());
            if (!sn) continue;
            for (const auto& p : sn->parameters()) {
                if (p.isAnimated()) return true;
            }
            if (sn->innerGraph() && walk(sn->innerGraph())) return true;
        }
        return false;
    };
    return walk(m_sop_graph.get());
}

// Worker thread: pull JSON requests, deserialize + cook on a private
// SopGraph copy, hand the emitted actor list to the main thread via
// m_pending_cook_result. Latest-wins: a new set_sop_graph while a cook is
// running overwrites the pending request, and the worker only ever cooks
// the most recently posted JSON.
void EditorServer::cook_worker_loop() {
    while (true) {
        CookRequest request;
        {
            std::unique_lock<std::mutex> lk(m_cook_request_mutex);
            m_cook_request_cv.wait(lk, [&] {
                return m_cook_shutdown || m_pending_cook_request.has_value();
            });
            if (m_cook_shutdown) return;
            request = std::move(*m_pending_cook_request);
            m_pending_cook_request.reset();
        }

        std::unique_ptr<tracey::sops::SopGraph> graph;
        try {
            graph = tracey::sops::deserializeSopGraph(request.graph_json);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[sop worker] parse failed: %s\n", e.what());
            continue;
        }
        if (!graph) continue;

        // Apply DOP-import side-channel stamps. The serialized graph drops
        // dop_import's m_stamped Geometry; the canonical EditorServer side
        // collected the current-frame geometries and passed them through
        // CookRequest::dop_stamps. We poke them into the worker's private
        // graph clone here so cook() sees real data.
        for (auto& [uid, geo] : request.dop_stamps) {
            if (auto* node = findNodeRecursive(graph.get(), uid)) {
                tracey::sops::setDopImportGeometry(node, std::move(geo));
            }
        }

        tracey::sops::CookDiagnostic diag;
        std::vector<tracey::sops::EmittedActor> emitted;
        std::vector<tracey::sops::NodeCookTiming> timings;
        try {
            // Houdini-style cook cache: per-node Geometry cache keyed by
            // (kind, params hash, upstream cookIds, time if time-dep). The
            // worker thread owns the cache exclusively, so no lock needed
            // — the loop only ever runs one cook at a time. Mark all
            // untouched up-front and evict after the cook so entries from
            // a node the user deleted get freed.
            m_worker_cook_cache.markAllUntouched();
            emitted = graph->cook(&diag, request.time, &m_worker_cook_cache, &timings);
            m_worker_cook_cache.evictUntouched();
        } catch (const std::exception& e) {
            // Without this catch, an uncaught exception from any SOP's cook()
            // would terminate the worker thread for the lifetime of the
            // process, leaving subsequent edits silently uncooked. Log and
            // keep the worker alive.
            std::fprintf(stderr, "[sop worker] cook threw: %s\n", e.what());
            continue;
        }
        if (!diag.ok) {
            std::fprintf(stderr, "[sop worker] cook failed: %s (node uid=%zu)\n",
                         diag.message.c_str(), diag.nodeUid);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(m_cook_result_mutex);
            // Last-wins: if the main thread hasn't drained the previous
            // result yet, overwrite it with this newer one.
            m_pending_cook_result = PendingCookResult{
                std::move(emitted),
                std::move(timings),
            };
        }
    }
}

// ── Render worker thread ──────────────────────────────────────────────
// Runs the rasterizer off the main thread so the UI never blocks on a
// GPU fence wait. render_tick on the main thread posts a snapshot (camera +
// shared_ptr to compiled scene) here; this loop drains the latest and
// dispatches. The output image is single-buffered; correctness across the
// "worker writing while main thread presents" boundary relies on:
//   • Both threads serializing their command-buffer submissions through
//     vulkanQueueMutex (Rasterizer::render now releases that mutex around
//     its fence wait, so contention is microseconds).
//   • Vulkan's per-queue execution ordering — a present-blit submitted
//     after a raster on the same queue can't start before the raster
//     finishes, even if both submissions sit briefly on the CPU side.
void EditorServer::render_thread_main() {
    for (;;) {
        RenderRequest request;
        {
            std::unique_lock<std::mutex> lk(m_render_mutex);
            m_render_cv.wait(lk, [this] {
                return m_render_thread_should_exit ||
                       m_pending_render_request.has_value();
            });
            if (m_render_thread_should_exit) return;
            request = std::move(*m_pending_render_request);
            m_pending_render_request.reset();
        }

        if (!request.scene || !m_engine) continue;

        try {
            const double ms = m_engine->render_rasterizer_with(
                *request.scene, request.camera);
            m_worker_raster_ms.store(ms, std::memory_order_relaxed);
            m_render_frames_completed.fetch_add(1, std::memory_order_release);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[render worker] dispatch failed: %s\n", e.what());
            // Don't bump the completion counter — main thread will keep
            // showing the previously-presented frame until we recover.
        }
    }
}

void EditorServer::stop_render_thread() {
    {
        std::lock_guard<std::mutex> lk(m_render_mutex);
        m_render_thread_should_exit = true;
    }
    m_render_cv.notify_one();
    if (m_render_thread.joinable()) m_render_thread.join();
}

// Called from the message thread under m_mutex. Hands a serialized graph
// (+ playhead time) to the worker. Returns immediately; the cook completes
// asynchronously and gets applied on the next render_tick.
void EditorServer::post_cook_request(std::string graph_json, double time) {
    // Cook the DOP graph forward to this frame and snapshot the current
    // sim state for every dop_import SOP. The worker thread re-stamps
    // these into its private graph clone before cook(), so the serialized
    // JSON doesn't have to carry geometry payloads.
    auto stamps = collect_dop_stamps(time);
    {
        std::lock_guard<std::mutex> lk(m_cook_request_mutex);
        m_pending_cook_request = CookRequest{
            std::move(graph_json),
            time,
            std::move(stamps),
        };
    }
    m_cook_request_cv.notify_one();
    // Tell the frontend the cook is in flight so it can surface a loading
    // status. The matching busy:false fires from drain_cook_result after
    // apply_emitted lands; if multiple requests coalesce (latest-wins), the
    // extra busy:true broadcasts are harmless — they just re-affirm state.
    if (m_broadcast) {
        m_broadcast(R"({"event":"cook_status","busy":true})");
    }
}

// Called from render_tick on the main thread (already holds m_mutex).
// Applies any cook result the worker has produced since the previous tick.
void EditorServer::drain_cook_result() {
    std::optional<PendingCookResult> result;
    {
        std::lock_guard<std::mutex> lk(m_cook_result_mutex);
        if (!m_pending_cook_result) return;
        result = std::move(m_pending_cook_result);
        m_pending_cook_result.reset();
    }
    apply_emitted(std::move(result->emitted));

    // Broadcast per-node timings so the profiler tab can refresh. Doing it
    // here (rather than in apply_emitted) keeps the profiler decoupled
    // from scene rebuild side-effects — it just needs the latest "what
    // did the worker do?" snapshot.
    if (m_broadcast && !result->timings.empty()) {
        json arr = json::array();
        double total_ms = 0.0;
        for (const auto& nct : result->timings) {
            arr.push_back({
                {"node_uid",        static_cast<uint64_t>(nct.nodeUid)},
                {"parent_node_uid", static_cast<uint64_t>(nct.parentNodeUid)},
                {"kind",            nct.kind},
                {"name",            nct.name},
                {"ms",              nct.ms},
            });
            total_ms += nct.ms;
        }
        json msg = {
            {"event",   "cook_timings"},
            {"total_ms", total_ms},
            {"rows",    std::move(arr)},
        };
        m_broadcast(msg.dump());
    }

    // Clear the loading status. If another request is already queued behind
    // this one, the next post_cook_request will re-broadcast busy:true.
    if (m_broadcast) {
        m_broadcast(R"({"event":"cook_status","busy":false})");
    }

    // Frame-locked playback: the cook completion just delivered frame N's
    // state. Advance the playhead by exactly 1/fps and post the next
    // cook so the chain keeps moving — wall-clock dt isn't driving it
    // in this mode, so without this step playback would stop after the
    // very first frame.
    //
    // We skip the kick if there's nothing for the cook to do (no
    // animated VOP promotion + no DOP imports — i.e. the graph is
    // static) so static scenes don't busy-loop the cook worker.
    if (m_timeline.frame_locked && m_timeline.playing &&
        !m_last_pushed_graph_json.empty() &&
        (m_has_animated_sop_params || m_has_dop_imports))
    {
        const double stride = 1.0 / std::max(m_timeline.fps, 1e-6);
        if (advance_playhead_by(stride)) {
            // Mark the timeline dirty so the next render_tick re-runs
            // animation overrides at the new time. The cook request is
            // posted here directly (not via render_tick) so the worker
            // stays continuously busy.
            m_timeline_dirty = true;
            post_cook_request(m_last_pushed_graph_json, m_timeline.current_time);
            // Broadcast the new playhead position so the dopesheet UI
            // tracks frame-locked playback the same way it tracks
            // wall-clock playback (timeline_tick in render_tick fires
            // at 30 Hz but only when m_timeline.playing && dt rolled
            // it forward, which doesn't happen here).
            if (m_broadcast) {
                json msg = {
                    {"event", "timeline_tick"},
                    {"time",  m_timeline.current_time},
                    {"playing", m_timeline.playing},
                };
                m_broadcast(msg.dump());
            }
        }
    }
}

// Per-frame offline render. Runs on m_export_thread; takes m_mutex for the
// cook + render so it doesn't race with command-handler scene mutations or
// the cook worker's apply_emitted(). render_tick() early-returns while we
// hold m_export_in_progress, which prevents the live viewport from also
// touching the path tracer.
void EditorServer::export_video_loop(VideoExportRequest req) {
    auto broadcast_event = [this](const json& msg) {
        if (m_broadcast) m_broadcast(msg.dump());
    };

    const int total = req.frame_end - req.frame_start + 1;
    if (total <= 0 || req.fps <= 0.0 || req.samples_per_frame <= 0) {
        broadcast_event({{"event", "video_export_error"},
                         {"message", "invalid range / fps / samples"}});
        m_export_in_progress.store(false);
        return;
    }

    // Snapshot the playhead so the user's timeline isn't permanently shifted
    // by an export. Restored on exit (success, error, or cancel).
    double saved_time = 0.0;
    bool saved_time_known = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        saved_time = m_timeline.current_time;
        saved_time_known = true;
    }

    // Snapshot resolutions and camera aspect so we can restore them on exit.
    // If the user passed explicit width/height, resize the path tracer to
    // that — leaving the rasterizer (and hence the live viewport overlay) at
    // whatever it was. set_resolutions() recreates the PT and the next
    // cook_and_apply() call will recompile against it.
    uint32_t width = 0, height = 0;
    uint32_t saved_raster_w = 0, saved_raster_h = 0;
    uint32_t saved_pt_w = 0, saved_pt_h = 0;
    float saved_aspect = 1.0f;
    bool resolution_changed = false;
    uint32_t saved_max_bounces = 0;
    bool bounces_changed = false;
    const bool exr = (req.format == "exr");
    bool export_aovs_changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_engine || !m_engine->path_tracer_ready() ||
            !m_engine->compiled_scene_ready() || !m_engine->scene().hasCamera()) {
            broadcast_event({{"event", "video_export_error"},
                             {"message", "engine not ready (open a scene first)"}});
            m_export_in_progress.store(false);
            return;
        }
        const auto [r_w, r_h] = m_engine->resolution();
        const auto [p_w, p_h] = m_engine->pt_resolution();
        saved_raster_w = r_w;
        saved_raster_h = r_h;
        saved_pt_w = p_w;
        saved_pt_h = p_h;
        saved_aspect = m_engine->scene().camera().aspectRatio();

        if (req.width > 0 && req.height > 0) {
            width = static_cast<uint32_t>(req.width);
            height = static_cast<uint32_t>(req.height);
            m_engine->set_resolutions(saved_raster_w, saved_raster_h, width, height);
            tracey::Camera cam = m_engine->scene().camera();
            cam.setAspectRatio(static_cast<float>(width) /
                               static_cast<float>(height));
            m_engine->scene().setCamera(cam);
            resolution_changed = (width != saved_pt_w || height != saved_pt_h);
        } else {
            width = saved_pt_w;
            height = saved_pt_h;
        }

        saved_max_bounces = m_engine->max_bounces();
        if (req.max_bounces > 0 &&
            static_cast<uint32_t>(req.max_bounces) != saved_max_bounces) {
            m_engine->set_max_bounces(static_cast<uint32_t>(req.max_bounces));
            bounces_changed = true;
        }

        // EXR mode: switch the PT into AOV + linear-beauty output (recreates it).
        if (exr && !m_engine->export_aovs()) {
            m_engine->set_export_aovs(true);
            export_aovs_changed = true;
        }
    }
    if (width == 0 || height == 0) {
        broadcast_event({{"event", "video_export_error"},
                         {"message", "viewport has zero size"}});
        m_export_in_progress.store(false);
        return;
    }

    auto restore_engine_state = [&]() {
        if (!resolution_changed && !bounces_changed && !export_aovs_changed) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (export_aovs_changed) {
            m_engine->set_export_aovs(false);
        }
        if (resolution_changed) {
            m_engine->set_resolutions(saved_raster_w, saved_raster_h,
                                      saved_pt_w, saved_pt_h);
            if (m_engine->scene().hasCamera()) {
                tracey::Camera cam = m_engine->scene().camera();
                cam.setAspectRatio(saved_aspect);
                m_engine->scene().setCamera(cam);
            }
        }
        if (bounces_changed) {
            m_engine->set_max_bounces(saved_max_bounces);
        }
        m_clear_next_frame = true;
    };

    VideoExporter exporter;
    if (!exr) {
        const auto codec = (req.codec == "prores")
            ? VideoExporter::Codec::ProRes422
            : VideoExporter::Codec::H264;
        if (!exporter.begin(req.path, width, height,
                            static_cast<uint32_t>(std::lround(req.fps)), codec)) {
            broadcast_event({{"event", "video_export_error"},
                             {"message", exporter.last_error()}});
            restore_engine_state();
            m_export_in_progress.store(false);
            return;
        }
    }

    // Per-frame EXR sequence path: "<dir>/<stem>.NNNN.exr".
    const std::filesystem::path exr_base(req.path);
    auto exr_frame_path = [&](int frame) -> std::string {
        char num[16];
        std::snprintf(num, sizeof(num), "%04d", frame);
        return (exr_base.parent_path() /
                (exr_base.stem().string() + "." + num + ".exr")).string();
    };
    // Pull the first N interleaved channels out of a readback's RGBA32F plane.
    const size_t pixel_count = static_cast<size_t>(width) * height;
    auto take_n = [&](const float *rgba, int n) {
        std::vector<float> out(pixel_count * static_cast<size_t>(n));
        for (size_t p = 0; p < pixel_count; ++p)
            for (int c = 0; c < n; ++c) out[p * n + c] = rgba[p * 4 + c];
        return out;
    };

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    bool success = true;

    // Detect once at the top of the export whether the graph is fully
    // static (no animated channels and no time-dependent VOPs). For a
    // static scene every per-frame cook produces byte-identical output —
    // so we cook+apply once before the first frame, then per frame only
    // render the requested samples. Skipping the cook for an N-second
    // export of a 20-mesh scene saves N*30 BLAS rebuilds.
    //
    // For animated scenes we fall back to the per-frame cook + apply
    // path, same as before. The proper per-actor BLAS cache lands next.
    bool scene_static = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        scene_static = !detect_any_animation();
    }
    bool cooked_once = false;

    for (int f = req.frame_start; f <= req.frame_end; ++f) {
        if (m_export_cancel.load()) break;

        // Frame numbering is 1-based; time at frame N is (N-1)/fps. Mirrors
        // stores/timeline.ts secondsForFrame().
        const double frame_time = (req.fps > 0.0) ? (f - 1) / req.fps : 0.0;
        const int frame_index = f - req.frame_start;  // 0-based for the writer

        // A render dispatch can throw (e.g. a Metal command-buffer error); on a
        // worker thread that would terminate the whole app. Catch it, report it
        // as an export error, and stop cleanly.
        try {
        // Step 1: seek + cook + accumulate samples — all under m_mutex.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_timeline.current_time = frame_time;
            // Skip the per-frame cook+apply on static scenes after the
            // first frame — the BLAS / scene state is already correct.
            if (!scene_static || !cooked_once) {
                cook_and_apply();
                cooked_once = true;
            }
            apply_animation_at(frame_time);

            // R4 motion blur: build shutter-close instance poses so moving
            // objects blur. Evaluate the animation at frame_time + shutter·(1/fps),
            // snapshot those instances as instancesEnd, then restore the start
            // pose and stamp a fresh revision so the path-tracer backend rebuilds
            // with the hardware/swept motion AS. Rigid per-instance (keyed TRS)
            // motion only; camera + deformation blur are follow-ups.
            const float shutter = m_engine->scene().camera().shutter();
            if (shutter > 0.0f && m_engine->compiled_scene_ready()) {
                const double dt = static_cast<double>(shutter) / std::max(req.fps, 1e-6);
                apply_animation_at(frame_time + dt);
                m_engine->compile_scene();
                std::vector<tracey::Tlas::Instance> endInstances;
                if (auto snap = m_engine->compiled_scene_snapshot())
                    endInstances = snap->instances;
                apply_animation_at(frame_time);   // restore the shutter-open pose
                m_engine->compile_scene();
                m_engine->set_motion_end_instances(std::move(endInstances));
            }

            for (int s = 0; s < req.samples_per_frame; ++s) {
                if (m_export_cancel.load()) break;
                const bool clear = (s == 0);
                // Only the final sample needs the readback blit (beauty comes
                // back via the readback buffer); earlier samples skip it. AOVs
                // read from their own shared buffers, so they don't need it.
                const bool last = (s == req.samples_per_frame - 1);
                m_engine->render_frame(clear, /*want_pixels=*/last);
            }
        }

        if (m_export_cancel.load()) break;

        // Step 2: pull the accumulated frame back and write it. Read under the
        // lock for a coherent PT state, then drop the lock for the file write
        // so command handlers can still answer e.g. export_video_cancel.
        if (exr) {
            // Linear beauty (readback() is linear/float in export-AOV mode) +
            // each AOV layer (RGBA32F), assembled into one multi-layer EXR.
            std::vector<float> beauty(pixel_count * 4);
            std::vector<float> albedo(pixel_count * 4), normalAov(pixel_count * 4);
            std::vector<float> depth(pixel_count * 4), position(pixel_count * 4);
            std::vector<float> emission(pixel_count * 4), instanceId(pixel_count * 4);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto *pt = m_engine->path_tracer();
                pt->readback(reinterpret_cast<uint8_t *>(beauty.data()));
                pt->readbackAOV(tracey::AovKind::Albedo, albedo.data());
                pt->readbackAOV(tracey::AovKind::Normal, normalAov.data());
                pt->readbackAOV(tracey::AovKind::Depth, depth.data());
                pt->readbackAOV(tracey::AovKind::Position, position.data());
                pt->readbackAOV(tracey::AovKind::Emission, emission.data());
                pt->readbackAOV(tracey::AovKind::InstanceId, instanceId.data());
            }
            // Optional OIDN denoise of the linear beauty, guided by albedo +
            // normal. On failure (or no denoiser built) fall back to the raw
            // beauty so the export still completes.
            std::vector<float> denoised;
            const float *beautySrc = beauty.data();
            if (req.denoise && tracey::denoiserAvailable()) {
                denoised.resize(pixel_count * 4);
                std::string derr;
                if (tracey::denoiseImage(static_cast<int>(width), static_cast<int>(height),
                                         beauty.data(), albedo.data(), normalAov.data(),
                                         denoised.data(), &derr)) {
                    beautySrc = denoised.data();
                } else {
                    std::fprintf(stderr, "[export] denoise failed: %s\n", derr.c_str());
                }
            }
            const auto bRGB = take_n(beautySrc, 3);
            const auto aRGB = take_n(albedo.data(), 3);
            const auto nXYZ = take_n(normalAov.data(), 3);
            const auto pXYZ = take_n(position.data(), 3);
            const auto eRGB = take_n(emission.data(), 3);
            const auto zR = take_n(depth.data(), 1);
            const auto idR = take_n(instanceId.data(), 1);
            const std::vector<tracey::ExrLayer> layers = {
                {"", 3, bRGB.data()},
                {"albedo", 3, aRGB.data()},
                {"N", 3, nXYZ.data()},
                {"P", 3, pXYZ.data()},
                {"emission", 3, eRGB.data()},
                {"Z", 1, zR.data()},
                {"id", 1, idR.data()},
            };
            std::string exr_err;
            if (!tracey::writeMultiLayerExr(exr_frame_path(f), static_cast<int>(width),
                                            static_cast<int>(height), layers, &exr_err)) {
                broadcast_event({{"event", "video_export_error"},
                                 {"message", "EXR write failed: " + exr_err}});
                success = false;
                break;
            }
        } else {
            std::vector<uint8_t> pixels;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const size_t bytes = static_cast<size_t>(width) * height * 4;
                pixels.resize(bytes);
                m_engine->path_tracer()->readback(pixels.data());
            }

            if (!exporter.append_frame(pixels.data(),
                                       static_cast<uint32_t>(frame_index))) {
                broadcast_event({{"event", "video_export_error"},
                                 {"message", exporter.last_error()}});
                success = false;
                break;
            }
        }
        } catch (const std::exception &e) {
            broadcast_event({{"event", "video_export_error"},
                             {"message", std::string("render failed: ") + e.what()}});
            success = false;
            break;
        }

        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        broadcast_event({{"event", "video_export_progress"},
                         {"frame", frame_index + 1},
                         {"total", total},
                         {"elapsed_ms", elapsed_ms}});
    }

    const bool cancelled = m_export_cancel.load();
    const bool finished_ok = exr ? true : exporter.finish(cancelled || !success);

    // Restore PT resolution + camera aspect for the live viewport before we
    // hand control back. Done before the playhead restore so the next
    // render_tick sees a consistent state.
    restore_engine_state();

    // Restore the user's playhead so the live viewport doesn't pop to the
    // export's last frame.
    if (saved_time_known) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_timeline.current_time = saved_time;
        m_timeline_dirty = true;
    }

    if (!success) {
        // Already broadcast video_export_error above.
    } else if (cancelled) {
        broadcast_event({{"event", "video_export_done"},
                         {"path", req.path},
                         {"cancelled", true}});
    } else if (!finished_ok) {
        broadcast_event({{"event", "video_export_error"},
                         {"message", exporter.last_error()}});
    } else {
        broadcast_event({{"event", "video_export_done"},
                         {"path", req.path},
                         {"cancelled", false}});
    }

    m_export_cancel.store(false);
    m_export_in_progress.store(false);
}

void EditorServer::render_still_loop(RenderStillRequest req) {
    auto broadcast_event = [this](const json& msg) {
        if (m_broadcast) m_broadcast(msg.dump());
    };
    const bool exr = (req.format == "exr");

    // Snapshot + (re)configure the path tracer, mirroring export_video_loop:
    // optional resize to the target still resolution, optional bounce override,
    // and EXR → AOV+linear output mode. Recompile the CURRENT scene against the
    // reconfigured PT (the BlasCache makes this cheap — no re-cook of the SOP
    // graph, and no timeline seek: a still is "this frame, as it is now").
    uint32_t width = 0, height = 0;
    uint32_t saved_raster_w = 0, saved_raster_h = 0, saved_pt_w = 0, saved_pt_h = 0;
    float saved_aspect = 1.0f;
    uint32_t saved_max_bounces = 0;
    bool resolution_changed = false, bounces_changed = false, export_aovs_changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_engine || !m_engine->path_tracer_ready() ||
            !m_engine->compiled_scene_ready() || !m_engine->scene().hasCamera()) {
            broadcast_event({{"event", "render_still_error"},
                             {"message", "engine not ready (open a scene first)"}});
            m_export_in_progress.store(false);
            return;
        }
        const auto [r_w, r_h] = m_engine->resolution();
        const auto [p_w, p_h] = m_engine->pt_resolution();
        saved_raster_w = r_w; saved_raster_h = r_h;
        saved_pt_w = p_w; saved_pt_h = p_h;
        saved_aspect = m_engine->scene().camera().aspectRatio();

        if (req.width > 0 && req.height > 0) {
            width = static_cast<uint32_t>(req.width);
            height = static_cast<uint32_t>(req.height);
            m_engine->set_resolutions(saved_raster_w, saved_raster_h, width, height);
            tracey::Camera cam = m_engine->scene().camera();
            cam.setAspectRatio(static_cast<float>(width) / static_cast<float>(height));
            m_engine->scene().setCamera(cam);
            resolution_changed = (width != saved_pt_w || height != saved_pt_h);
        } else {
            width = saved_pt_w;
            height = saved_pt_h;
        }

        saved_max_bounces = m_engine->max_bounces();
        if (req.max_bounces > 0 &&
            static_cast<uint32_t>(req.max_bounces) != saved_max_bounces) {
            m_engine->set_max_bounces(static_cast<uint32_t>(req.max_bounces));
            bounces_changed = true;
        }
        if (exr && !m_engine->export_aovs()) {
            m_engine->set_export_aovs(true);
            export_aovs_changed = true;
        }
        // Re-bind the current scene to the (possibly resized / AOV-mode) PT.
        m_engine->compile_scene();
    }
    if (width == 0 || height == 0) {
        broadcast_event({{"event", "render_still_error"},
                         {"message", "viewport has zero size"}});
        m_export_in_progress.store(false);
        return;
    }

    auto restore_engine_state = [&]() {
        if (!resolution_changed && !bounces_changed && !export_aovs_changed) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (export_aovs_changed) m_engine->set_export_aovs(false);
        if (resolution_changed) {
            m_engine->set_resolutions(saved_raster_w, saved_raster_h, saved_pt_w, saved_pt_h);
            if (m_engine->scene().hasCamera()) {
                tracey::Camera cam = m_engine->scene().camera();
                cam.setAspectRatio(saved_aspect);
                m_engine->scene().setCamera(cam);
            }
        }
        if (bounces_changed) m_engine->set_max_bounces(saved_max_bounces);
        m_engine->compile_scene(); // rebind the original scene to the restored PT
        m_clear_next_frame = true;
    };

    const int samples = std::max(1, req.samples);
    const size_t pixel_count = static_cast<size_t>(width) * height;
    bool success = true;
    std::string errMsg;

    auto take_n = [&](const float *rgba, int n) {
        std::vector<float> out(pixel_count * static_cast<size_t>(n));
        for (size_t p = 0; p < pixel_count; ++p)
            for (int c = 0; c < n; ++c) out[p * n + c] = rgba[p * 4 + c];
        return out;
    };

    try {
        // Accumulate all samples under the lock — render_tick() is paused
        // (m_export_in_progress), so we hold the engine for the whole render.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (int s = 0; s < samples; ++s) {
                if (m_export_cancel.load()) break;
                const bool last = (s == samples - 1);
                m_engine->render_frame(/*clear=*/s == 0, /*want_pixels=*/last);
            }
        }

        if (m_export_cancel.load()) {
            // fall through to the done(cancelled) broadcast below
        } else if (exr) {
            std::vector<float> beauty(pixel_count * 4), albedo(pixel_count * 4),
                normalAov(pixel_count * 4), depth(pixel_count * 4),
                position(pixel_count * 4), emission(pixel_count * 4),
                instanceId(pixel_count * 4);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto *pt = m_engine->path_tracer();
                pt->readback(reinterpret_cast<uint8_t *>(beauty.data()));
                pt->readbackAOV(tracey::AovKind::Albedo, albedo.data());
                pt->readbackAOV(tracey::AovKind::Normal, normalAov.data());
                pt->readbackAOV(tracey::AovKind::Depth, depth.data());
                pt->readbackAOV(tracey::AovKind::Position, position.data());
                pt->readbackAOV(tracey::AovKind::Emission, emission.data());
                pt->readbackAOV(tracey::AovKind::InstanceId, instanceId.data());
            }
            std::vector<float> denoised;
            const float *beautySrc = beauty.data();
            if (req.denoise && tracey::denoiserAvailable()) {
                denoised.resize(pixel_count * 4);
                std::string derr;
                if (tracey::denoiseImage(static_cast<int>(width), static_cast<int>(height),
                                         beauty.data(), albedo.data(), normalAov.data(),
                                         denoised.data(), &derr))
                    beautySrc = denoised.data();
                else
                    std::fprintf(stderr, "[still] denoise failed: %s\n", derr.c_str());
            }
            const auto bRGB = take_n(beautySrc, 3);
            const auto aRGB = take_n(albedo.data(), 3);
            const auto nXYZ = take_n(normalAov.data(), 3);
            const auto pXYZ = take_n(position.data(), 3);
            const auto eRGB = take_n(emission.data(), 3);
            const auto zR = take_n(depth.data(), 1);
            const auto idR = take_n(instanceId.data(), 1);
            const std::vector<tracey::ExrLayer> layers = {
                {"", 3, bRGB.data()},        {"albedo", 3, aRGB.data()},
                {"N", 3, nXYZ.data()},       {"P", 3, pXYZ.data()},
                {"emission", 3, eRGB.data()},{"Z", 1, zR.data()},
                {"id", 1, idR.data()},
            };
            std::string exr_err;
            if (!tracey::writeMultiLayerExr(req.path, static_cast<int>(width),
                                            static_cast<int>(height), layers, &exr_err)) {
                success = false; errMsg = "EXR write failed: " + exr_err;
            }
        } else {
            std::vector<uint8_t> pixels(pixel_count * 4);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_engine->path_tracer()->readback(pixels.data());
            }
            if (!tracey::writePng(req.path, static_cast<int>(width),
                                  static_cast<int>(height), pixels.data())) {
                success = false; errMsg = "PNG write failed: " + req.path;
            }
        }
    } catch (const std::exception &e) {
        success = false;
        errMsg = std::string("render failed: ") + e.what();
    }

    restore_engine_state();

    if (m_export_cancel.load()) {
        broadcast_event({{"event", "render_still_done"}, {"path", req.path}, {"cancelled", true}});
    } else if (!success) {
        broadcast_event({{"event", "render_still_error"}, {"message", errMsg}});
    } else {
        broadcast_event({{"event", "render_still_done"}, {"path", req.path}, {"cancelled", false}});
    }
    m_export_cancel.store(false);
    m_export_in_progress.store(false);
}

void EditorServer::import_usd_stage_worker(UsdStageImportRequest req) {
#ifdef TRACEY_HAS_USD
    auto progress = [this](const char* stage, int done, int total) {
        if (m_broadcast)
            m_broadcast(json{{"event", "usd_import_progress"}, {"stage", stage},
                             {"done", done}, {"total", total}}.dump());
    };

    // Heavy parse — off the mutex AND off the main thread, so the UI stays live.
    progress("Reading USD…", 0, 0);
    auto src = tracey::UsdLoader::loadFromFileCached(req.path);
    if (!src) {
        if (m_broadcast)
            m_broadcast(json{{"event", "usd_import_error"},
                             {"message", "failed to load " + req.path}}.dump());
        m_import_in_progress.store(false);
        return;
    }

    int lightCount = 0, instanceCount = 0;
    bool setCam = false;
    {
        // Scene mutation + compile under m_mutex. render_tick try_locks, so it
        // just skips frames while we hold it — the main run loop (WebView, the
        // progress events below) keeps turning, so no beachball.
        std::lock_guard<std::mutex> lock(m_mutex);

        if (req.lights) {
            progress("Lights…", 0, 0);
            for (const auto* a : src->actors()) {
                if (!a || !a->hasLight()) continue;
                auto* actor = m_engine->scene().createActor();
                actor->setName(a->name().empty() ? "usd_light" : a->name());
                actor->setTransform(a->transform());
                actor->setLight(*a->light());
                ++lightCount;
            }
        }

        if (req.camera && src->hasCamera()) {
            const tracey::Camera& uc = src->camera();
            const glm::vec3 P = uc.position();
            const glm::vec3 F = glm::normalize(glm::mat3_cast(uc.rotation()) * glm::vec3(0, 0, -1));
            glm::vec3 mn(1e30f), mx(-1e30f);
            bool anyGeo = false;
            for (const auto& node : src->flatten()) {
                if (!node.actor) continue;
                for (const auto& inst : node.actor->instances()) {
                    const auto* obj = src->getObject(inst.objectRef());
                    if (!obj) continue;
                    glm::mat4 w = node.worldTransform;
                    if (inst.hasLocalTransform()) w = w * inst.localTransform()->toMatrix();
                    for (const auto& p : obj->positions()) {
                        const glm::vec4 wp = w * glm::vec4(p, 1.0f);
                        mn = glm::min(mn, glm::vec3(wp));
                        mx = glm::max(mx, glm::vec3(wp));
                        anyGeo = true;
                    }
                }
            }
            const glm::vec3 center = anyGeo ? (mn + mx) * 0.5f : glm::vec3(0.0f);
            float d = glm::dot(center - P, F);
            if (!(d > 0.5f)) d = std::max(0.5f, glm::length(center - P));
            const glm::vec3 pivot = P + F * d;
            m_orbit_pivot_x = pivot.x; m_orbit_pivot_y = pivot.y; m_orbit_pivot_z = pivot.z;
            m_orbit_distance = d;
            m_orbit_pitch = std::asin(std::clamp(F.y, -1.0f, 1.0f));
            m_orbit_yaw = std::atan2(-F.x, -F.z);
            m_orbit_initialized = true;
            const glm::quat qyaw = glm::angleAxis(m_orbit_yaw, glm::vec3(0, 1, 0));
            const glm::quat qpitch = glm::angleAxis(m_orbit_pitch, glm::vec3(1, 0, 0));
            const glm::quat rot = qyaw * qpitch;
            tracey::Camera cam = uc;
            cam.setRotation(rot);
            cam.setPosition(pivot - (rot * glm::vec3(0, 0, -1)) * d);
            m_engine->scene().setCamera(cam);
            setCam = true;
        }

        if (req.instances) {
            // Count placements up front for a determinate progress bar.
            int totalInst = 0;
            for (const auto* a : src->actors()) {
                if (!a) continue;
                const std::string& nm = a->name();
                if (nm.rfind("instance:", 0) == 0 || nm.rfind("instancer:", 0) == 0)
                    totalInst += static_cast<int>(a->instances().size());
            }
            tracey::Actor* instActor = nullptr;
            for (const auto* a : src->actors()) {
                if (!a) continue;
                const std::string& nm = a->name();
                if (nm.rfind("instance:", 0) != 0 && nm.rfind("instancer:", 0) != 0) continue;
                for (const auto& inst : a->instances()) {
                    const std::string& objRef = inst.objectRef();
                    if (!m_engine->scene().hasObject(objRef)) {
                        const auto* srcObj = src->getObject(objRef);
                        if (!srcObj) continue;
                        m_engine->scene().addObject(
                            objRef, std::make_unique<tracey::SceneObject>(*srcObj));
                    }
                    if (!instActor) {
                        instActor = m_engine->scene().createActor();
                        std::string base = req.path;
                        auto slash = base.find_last_of("/\\");
                        if (slash != std::string::npos) base = base.substr(slash + 1);
                        auto dot = base.find_last_of('.');
                        if (dot != std::string::npos) base = base.substr(0, dot);
                        instActor->setName(base + " (instances)");
                    }
                    instActor->addInstance(inst);
                    if ((++instanceCount % 500) == 0)
                        progress("Placing instances…", instanceCount, totalInst);
                }
            }
        }

        // Only recompile if we actually ADDED geometry (lights/instances).
        // For a stage with no instances/lights (e.g. the non-instanced Kitchen
        // Set — its meshes come via the SOP cook, which already compiled),
        // a second compile here is not just wasteful: it churns the BlasCache,
        // which can free buffers the live compiled scene the rasterizer is
        // drawing still points at → a dangling VkBuffer → "vkCmdBindVertexBuffers
        // pBuffers[0] is VK_NULL_HANDLE". Skipping the no-op compile avoids it.
        if ((lightCount > 0 || instanceCount > 0) && m_engine->path_tracer_ready())
            m_engine->compile_scene();
        m_clear_next_frame = true;
    }

    if (m_broadcast) {
        m_broadcast(json{{"event", "usd_import_done"}, {"lights", lightCount},
                         {"camera", setCam}, {"instances", instanceCount}}.dump());
        m_broadcast(R"({"event":"scene_changed"})");
    }
    m_import_in_progress.store(false);
#else
    (void)req;
    if (m_broadcast)
        m_broadcast(R"({"event":"usd_import_error","message":"this build has no OpenUSD support"})");
    m_import_in_progress.store(false);
#endif
}

bool EditorServer::update_camera_from_input(double dt) {
    if (!m_window) return false;
    auto& input = m_window->input();

    if (!m_engine->scene().hasCamera()) {
        // Default camera if scene didn't ship one. Pulled up and back so the
        // ground grid is visible at a slight downward angle on first frame —
        // the previous (0,0,5) eye-level pose put the y=0 plane edge-on.
        // Rotation is set via lookAt so the very first render (before the
        // orbital state seeds itself in this same function) already frames
        // the origin properly.
        tracey::Camera cam;
        const glm::vec3 pos(0.0f, 4.0f, 8.0f);
        const glm::vec3 target(0.0f, 0.0f, 0.0f);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        cam.setPosition(pos);
        cam.setRotation(glm::quat_cast(glm::inverse(glm::lookAt(pos, target, up))));
        m_engine->scene().setCamera(cam);
    }
    tracey::Camera cam = m_engine->scene().camera();

    if (!m_orbit_initialized) {
        // Seed the orbital state from the existing camera. Pivot defaults to
        // world origin; distance is the camera's distance from it. Yaw/pitch
        // are derived from the camera's forward direction so the first frame
        // is visually identical to the input camera.
        glm::vec3 toCam = glm::vec3(cam.position());
        m_orbit_pivot_x = 0.0f;
        m_orbit_pivot_y = 0.0f;
        m_orbit_pivot_z = 0.0f;
        m_orbit_distance = std::max(0.1f, glm::length(toCam));
        glm::vec3 fwd = -glm::normalize(toCam.x == 0.f && toCam.y == 0.f && toCam.z == 0.f
                                        ? glm::vec3(0, 0, 1)
                                        : toCam);
        m_orbit_pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
        m_orbit_yaw = std::atan2(-fwd.x, -fwd.z);
        m_orbit_initialized = true;
    }

    bool changed = false;

    // Drag-to-navigate: LMB tumbles, MMB pans, RMB dollies — no modifier key.
    constexpr float TUMBLE_SENS = 0.005f;  // radians per pixel
    constexpr float DOLLY_SENS  = 0.01f;   // log-units per pixel
    constexpr float WHEEL_SENS  = 0.05f;   // log-units per scroll tick

    if (input.mouse_dx != 0.0f || input.mouse_dy != 0.0f) {
        if (input.mouse_left) {
            m_orbit_yaw   -= input.mouse_dx * TUMBLE_SENS;
            m_orbit_pitch -= input.mouse_dy * TUMBLE_SENS;
            constexpr float kPitchLimit = 1.5707f - 0.01f;
            m_orbit_pitch = std::clamp(m_orbit_pitch, -kPitchLimit, kPitchLimit);
            changed = true;
        } else if (input.mouse_middle) {
            // Pan: scale by distance + fov so a fixed pixel delta moves the
            // pivot by the same screen-space amount regardless of zoom.
            const float vh = std::max(1.0f, static_cast<float>(m_viewport_pixel_h));
            const float worldPerPx = 2.0f * m_orbit_distance *
                std::tan(glm::radians(cam.fov() * 0.5f)) / vh;
            const glm::vec3 r = cam.right();
            const glm::vec3 u = cam.up();
            const glm::vec3 delta = -r * (input.mouse_dx * worldPerPx) +
                                     u * (input.mouse_dy * worldPerPx);
            m_orbit_pivot_x += delta.x;
            m_orbit_pivot_y += delta.y;
            m_orbit_pivot_z += delta.z;
            changed = true;
        } else if (input.mouse_right) {
            // Dolly: exponential so zooming stays smooth at any distance.
            m_orbit_distance *= std::exp(input.mouse_dy * DOLLY_SENS);
            m_orbit_distance = std::max(0.01f, m_orbit_distance);
            changed = true;
        }
    }
    input.mouse_dx = 0.0f;
    input.mouse_dy = 0.0f;

    if (input.scroll_dy != 0.0f) {
        m_orbit_distance *= std::exp(-input.scroll_dy * WHEEL_SENS);
        m_orbit_distance = std::max(0.01f, m_orbit_distance);
        changed = true;
    }
    input.scroll_dx = 0.0f;
    input.scroll_dy = 0.0f;

    // Keyboard strafe (fly): translate the whole camera rig along the view
    // axes while keys are held — the universal WASD+QE scheme. W/S = forward
    // /back (along the look direction), A/D = left/right, Q/E = down/up
    // (world vertical). Moves the orbit pivot (camera follows it), so it
    // composes with mouse orbit/pan/dolly. Click the viewport first so it has
    // keyboard focus. Speed scales with orbit distance (consistent feel at any
    // zoom) and dt (frame-rate independent).
    {
        const glm::quat qyaw   = glm::angleAxis(m_orbit_yaw,   glm::vec3(0, 1, 0));
        const glm::quat qpitch = glm::angleAxis(m_orbit_pitch, glm::vec3(1, 0, 0));
        const glm::quat rot    = qyaw * qpitch;
        const glm::vec3 fwd   = rot * glm::vec3(0, 0, -1);
        const glm::vec3 right = rot * glm::vec3(1, 0, 0);
        const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        glm::vec3 move(0.0f);
        if (input.key_w) move += fwd;
        if (input.key_s) move -= fwd;
        if (input.key_d) move += right;
        if (input.key_a) move -= right;
        if (input.key_e) move += worldUp;
        if (input.key_q) move -= worldUp;
        if (glm::dot(move, move) > 1e-6f) {
            constexpr float STRAFE_SENS = 1.5f; // orbit-distances per second
            const float speed = std::max(0.05f, m_orbit_distance) * STRAFE_SENS *
                                 static_cast<float>(dt);
            const glm::vec3 d = glm::normalize(move) * speed;
            m_orbit_pivot_x += d.x;
            m_orbit_pivot_y += d.y;
            m_orbit_pivot_z += d.z;
            changed = true;
        }
    }

    if (changed) {
        // Compose yaw (around world Y) then pitch (around local X) and place
        // the camera on the orbit sphere centred at the pivot.
        glm::quat qyaw   = glm::angleAxis(m_orbit_yaw,   glm::vec3(0, 1, 0));
        glm::quat qpitch = glm::angleAxis(m_orbit_pitch, glm::vec3(1, 0, 0));
        glm::quat rotation = qyaw * qpitch;
        glm::vec3 forward = rotation * glm::vec3(0, 0, -1);
        glm::vec3 pivot{m_orbit_pivot_x, m_orbit_pivot_y, m_orbit_pivot_z};
        cam.setPosition(pivot - forward * m_orbit_distance);
        cam.setRotation(rotation);
        if (m_viewport_pixel_h > 0)
            cam.setAspectRatio(static_cast<float>(m_viewport_pixel_w) /
                               static_cast<float>(m_viewport_pixel_h));
        m_engine->scene().setCamera(cam);
    }
    return changed;
}

void EditorServer::render_tick() {
    // Pause the live viewport while an offline worker has the engine — the
    // sequence/still export worker OR the async USD-import worker. Both mutate
    // GPU resources (compile_scene, BLAS/vertex-buffer create+free, memory
    // map/unmap) on their own thread; if we kept rendering we'd touch the same
    // VkDeviceMemory / buffers concurrently (a Vulkan threading error — and a
    // use-after-free on buffers the import frees). The import also hides the
    // viewport behind the loading overlay, so there's nothing to show anyway.
    if (m_export_in_progress.load() || m_import_in_progress.load()) return;

    // try_lock instead of lock: render_tick fires from CVDisplayLink via
    // dispatch_async(main_queue). If a command handler holds m_mutex while
    // pumping the main run loop (e.g. NSSavePanel/NSOpenPanel runModal), the
    // dispatched tick would re-enter the same thread on the same std::mutex
    // and deadlock. Skipping the frame is cheap; the next vsync retries.
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;

    // Wall-clock dt — needed for both the camera fly-through and the
    // playhead. Computed up front so the playhead advances even before a
    // viewport renderer exists (useful for headless smoke tests).
    using clock = std::chrono::steady_clock;
    const auto tickStart = clock::now();
    const double now = std::chrono::duration<double>(tickStart.time_since_epoch()).count();
    const double dt = m_last_tick_time > 0.0 ? std::min(now - m_last_tick_time, 0.1) : 1.0 / 60.0;
    m_last_tick_time = now;

    // Phase timing helper. We can't measure `present_ms` until after the
    // present at the bottom of render_tick, so the broadcast below
    // publishes the *previous* tick's slice as a 1-frame-delayed
    // approximation. At 60 FPS that's ~16 ms older than the headline
    // FPS but well under the 250 ms broadcast cadence, so the user
    // can't perceive the delay; it just keeps the timing code linear.
    double rebuild_ms = 0.0;
    double raster_ms  = 0.0;
    double present_ms = 0.0;
    auto elapsedMs = [](clock::time_point start) {
        return std::chrono::duration<double, std::milli>(clock::now() - start).count();
    };

    // Apply any cook result the worker produced since the last tick. Cook
    // resets actor transforms to constants; apply_emitted sets
    // m_timeline_dirty so we re-evaluate animation overrides below.
    // Wrap in a try/catch: an exception escaping here would terminate the
    // process because the tick runs on the main thread (CVDisplayLink →
    // dispatch_async(main_queue)). The render-pass try/catch further down
    // doesn't cover this region.
    {
        const auto t0 = clock::now();
        try {
            drain_cook_result();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[render_tick] drain_cook_result failed: %s\n", e.what());
        }
        rebuild_ms = elapsedMs(t0);
    }

    // Advance the playhead during playback. Seeks via timeline_set_playhead
    // already updated current_time + set m_timeline_dirty, so we only need to
    // step it forward here.
    const bool playhead_moved = advance_playhead(dt);

    // Re-evaluate animated overrides whenever the playhead moved or a key
    // was edited / cook completed.
    const bool time_changed = playhead_moved || m_timeline_dirty;
    if (time_changed) {
        apply_animation_at(m_timeline.current_time);
        m_timeline_dirty = false;

        // Auto re-cook when the graph has animated VOP promotions: the
        // override path can't reach VOP-side knobs (only actor transforms),
        // so we re-cook the cached root JSON with the new playhead time.
        // Latest-wins in the worker keeps this cheap during rapid scrub.
        // Also fire when the graph has dop_import nodes — those need a
        // fresh sim-state stamp every frame (post_cook_request collects
        // the stamps automatically via collect_dop_stamps). The two
        // branches share the post; combine into one condition.
        const bool need_recook = (m_has_animated_sop_params || m_has_dop_imports)
                                 && !m_last_pushed_graph_json.empty();
        if (need_recook) {
            post_cook_request(m_last_pushed_graph_json, m_timeline.current_time);
        }
    }

    // Rate-limited timeline broadcast (~30 Hz) so the frontend playbar
    // follows the playhead without flooding the message bus.
    if (m_timeline.playing && m_broadcast &&
        (now - m_last_timeline_broadcast) > (1.0 / 30.0)) {
        m_last_timeline_broadcast = now;
        json msg = {
            {"event", "timeline_tick"},
            {"time", m_timeline.current_time},
            {"playing", m_timeline.playing},
        };
        m_broadcast(msg.dump());
    }

    if (!m_viewport_active || !m_window) return;
    if (!m_engine->path_tracer_ready() || !m_engine->rasterizer_ready() ||
        !m_engine->compiled_scene_ready()) return;

    ensure_viewport_renderer(m_window->viewport_pixel_width(),
                             m_window->viewport_pixel_height());
    if (!m_viewport) return;

    // Suppress camera orbit/pan/dolly while the JS modal grab owns the
    // mouse; broadcast pointer state instead so the grab math can run.
    if (m_viewport_grab_active) {
        if (m_window && m_broadcast) {
            auto& input = m_window->input();
            const bool changed =
                input.mouse_x != m_last_broadcast_mouse_x ||
                input.mouse_y != m_last_broadcast_mouse_y ||
                input.mouse_left  != m_last_broadcast_mouse_left ||
                input.mouse_right != m_last_broadcast_mouse_right;
            if (changed) {
                m_last_broadcast_mouse_x     = input.mouse_x;
                m_last_broadcast_mouse_y     = input.mouse_y;
                m_last_broadcast_mouse_left  = input.mouse_left;
                m_last_broadcast_mouse_right = input.mouse_right;
                json msg = {
                    {"event", "viewport_pointer"},
                    {"x", input.mouse_x},
                    {"y", input.mouse_y},
                    {"left",  input.mouse_left},
                    {"right", input.mouse_right},
                };
                m_broadcast(msg.dump());
            }
            // Drain the deltas so the camera doesn't snap when the grab
            // ends (otherwise the accumulated dx/dy from the drag would
            // fire on the first tick after the JS sends grab_active=false).
            input.mouse_dx = 0.0f;
            input.mouse_dy = 0.0f;
        }
    } else {
        if (update_camera_from_input(dt)) m_clear_next_frame = true;
    }

    const bool has_geometry = m_engine->has_renderable_geometry();
    try {
        // Only consume the pending clear when the PT actually dispatches —
        // otherwise a clear request raised while preview is off (camera
        // move, scene edit) would be silently lost. set_pt_preview's
        // off→on handler also raises this flag, so the very first frame
        // after enabling always starts a fresh accumulation.
        const bool clear = m_pt_preview_enabled && m_clear_next_frame;
        if (m_pt_preview_enabled) m_clear_next_frame = false;
        // Rasterizer is offloaded to a worker thread. Snapshot the scene
        // + camera under m_mutex (we already hold it via the tick's
        // try_lock above), then hand off to the worker. The worker writes
        // into the rasterizer's single output image; we present that image
        // below once it has produced at least one completed frame.
        //
        // Latest-wins: if the worker hasn't drained the previous request,
        // we overwrite it. That matches the live-viewport expectation —
        // we always want the freshest snapshot, not a backlog of stale
        // ones.
        if (m_engine->compiled_scene_ready() && m_engine->scene().hasCamera())
        {
            RenderRequest req;
            req.scene = m_engine->compiled_scene_snapshot();
            req.camera = m_engine->scene().camera();
            {
                std::lock_guard<std::mutex> rlk(m_render_mutex);
                m_pending_render_request = std::move(req);
            }
            m_render_cv.notify_one();
        }
        // The worker reports its measured ms back; we just read the
        // most recent value for the profiler bucket.
        raster_ms = m_worker_raster_ms.load(std::memory_order_relaxed);
        // Expensive path tracer pass: needs a BVH, so it only runs once the
        // scene has at least one instance. Accumulates into the inset rect,
        // one sample per tick, until max_samples is reached.
        const bool at_cap = !clear &&
            m_engine->current_samples() >= m_engine->max_samples();
        // The path tracer dispatch is the most expensive thing in
        // render_tick — skip it entirely when the inset preview is off.
        // We still run the rasterizer above so the viewport stays live.
        if (m_pt_preview_enabled && has_geometry && !at_cap) {
            // want_pixels=false: the live viewport composites
            // path_tracer()->outputImage() straight off the GPU through
            // ViewportRenderer::present_composite. Pulling the framebuffer
            // back to the CPU here would just discard it after grabbing
            // width/height/render_time_ms — and the elided
            // vkCmdCopyImageToBuffer + mapForReading shave the unmeasured
            // chunk out of the per-tick budget.
            auto result = m_engine->render_frame(clear, /*want_pixels=*/false);
            m_last_render_width = result.width;
            m_last_render_height = result.height;
            m_last_render_time_ms = result.render_time_ms;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[viewport] render failed: %s\n", e.what());
        return;
    }

    auto* tracer = m_engine->path_tracer();
    auto* raster = m_engine->rasterizer();
    if (tracer && raster) {
        auto* raster_output = raster->outputImage();
        // Worker may not have produced its first raster frame yet (first
        // tick after launch, before the worker has drained any request).
        // Blitting the rasterizer's output image without a completed
        // dispatch would show uninitialised content; gate everything that
        // reads `raster_output` on this counter. The PT-fullscreen branch
        // doesn't need raster at all, so it stays reachable.
        const bool raster_ready =
            m_render_frames_completed.load(std::memory_order_acquire) > 0;
        const auto t0 = clock::now();
        bool presented = false;
        // A present can throw on a recoverable swapchain condition (the surface
        // momentarily reports a 0×0 / out-of-date drawable while switching to
        // the Render tab or PT preview). Treat any such throw like a present
        // failure: drop the frame and zero the tracked size so the next tick
        // recreates the swapchain at the real size — never let it crash the app.
        try {
        if (m_pt_preview_enabled && has_geometry && m_pt_fullscreen) {
            // Render workspace: PT replaces the rasterizer entirely.
            // No raster output needed.
            if (auto* pt_output = tracer->outputImage()) {
                if (!m_viewport->present(pt_output)) {
                    m_viewport_pixel_w = 0;
                    m_viewport_pixel_h = 0;
                }
                presented = true;
            }
        } else if (raster_output && raster_ready) {
            if (!has_geometry || !m_pt_preview_enabled) {
                // Raster-only present. Most common path during scene
                // editing; the rasterizer-only branch skips the composite
                // sampler bind + inset blit in ViewportRenderer.
                if (!m_viewport->present(raster_output)) {
                    m_viewport_pixel_w = 0;
                    m_viewport_pixel_h = 0;
                }
                presented = true;
            } else if (auto* pt_output = tracer->outputImage()) {
                // PiP composite: rasterizer fills the viewport, PT
                // accumulator overlays in the top-right inset.
                const InsetRect r = compute_inset_rect(m_viewport_pixel_w, m_viewport_pixel_h);
                if (!m_viewport->present_composite(raster_output, pt_output,
                                                   r.x, r.y, r.w, r.h)) {
                    m_viewport_pixel_w = 0;
                    m_viewport_pixel_h = 0;
                }
                presented = true;
            }
        }
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[viewport] present failed (recoverable — skipping frame): %s\n", e.what());
            m_viewport_pixel_w = 0;
            m_viewport_pixel_h = 0;
            presented = false;
        }
        if (presented) present_ms = elapsedMs(t0);
    }

    const double tick_ms = elapsedMs(tickStart);

    // EMA-smooth the buckets so the profiler readout doesn't twitch
    // on per-tick jitter. The 0.9/0.1 weighting matches m_smoothed_dt:
    // ~10-tick half-life, which at the broadcast cadence below feels
    // responsive without being noisy.
    auto ema = [](double prev, double curr) {
        return prev > 1e-9 ? prev * 0.9 + curr * 0.1 : curr;
    };
    m_smoothed_tick_ms    = ema(m_smoothed_tick_ms,    tick_ms);
    m_smoothed_rebuild_ms = ema(m_smoothed_rebuild_ms, rebuild_ms);
    m_smoothed_raster_ms  = ema(m_smoothed_raster_ms,  raster_ms);
    m_smoothed_present_ms = ema(m_smoothed_present_ms, present_ms);

    // Throttled render-stats broadcast for the profiler tab. 250 ms
    // cadence is fast enough to feel responsive and well under the
    // message-bus overhead the timeline_tick broadcasts establish.
    if (dt > 0.0) m_smoothed_dt = m_smoothed_dt * 0.9 + dt * 0.1;
    if (m_broadcast && now - m_last_render_stats_broadcast > 0.25) {
        m_last_render_stats_broadcast = now;
        const double fps = (m_smoothed_dt > 1e-6) ? (1.0 / m_smoothed_dt) : 0.0;
        json msg = {
            {"event",           "render_stats"},
            {"fps",             fps},
            {"render_time_ms",  m_last_render_time_ms},
            {"tick_ms",         m_smoothed_tick_ms},
            {"rebuild_ms",      m_smoothed_rebuild_ms},
            {"raster_ms",       m_smoothed_raster_ms},
            {"present_ms",      m_smoothed_present_ms},
            {"triangles",       static_cast<uint64_t>(m_engine->total_triangles())},
            {"instances",       static_cast<uint64_t>(m_engine->total_instances())},
            {"bvh_nodes",       static_cast<uint64_t>(m_engine->total_bvh_nodes())},
            {"samples",         m_engine->current_samples()},
            {"max_samples",     m_engine->max_samples()},
        };
        m_broadcast(msg.dump());
    }
}

std::string EditorServer::handle_command(const std::string& json_request) {
    json req;
    try {
        req = json::parse(json_request);
    } catch (const std::exception& e) {
        return err_response(std::string{"JSON parse error: "} + e.what());
    }

    const std::string cmd = req.value("cmd", std::string{});
    if (cmd.empty()) return err_response("Missing 'cmd' field");

    // Native modal file dialogs ([NSOpenPanel runModal] etc.) pump the main
    // run loop while open, which means other JS commands queued on the same
    // thread try to re-acquire m_mutex and deadlock the main thread until
    // WebKit's deferral block times out (beachball). Dialogs don't touch
    // shared engine state, so handle them BEFORE taking the lock.
    if (cmd == "open_file_dialog" || cmd == "save_file_dialog" ||
        cmd == "open_folder_dialog") {
        if (!m_window) return err_response("No window for dialog");
        try {
            std::vector<FileFilter> filters;
            if (req.contains("filters")) {
                for (const auto& f : req["filters"]) {
                    FileFilter ff;
                    ff.description = f.value("description", std::string{});
                    if (f.contains("extensions")) {
                        for (const auto& e : f["extensions"])
                            ff.extensions.push_back(e.get<std::string>());
                    }
                    filters.push_back(std::move(ff));
                }
            }
            std::string path;
            if (cmd == "open_file_dialog") {
                const auto title = req.value("title", std::string{"Open"});
                path = m_window->open_file_dialog(title.c_str(), filters);
            } else if (cmd == "save_file_dialog") {
                const auto title = req.value("title", std::string{"Save"});
                const auto default_name = req.value("default_name", std::string{});
                path = m_window->save_file_dialog(title.c_str(),
                                                  default_name.c_str(),
                                                  filters);
            } else {
                const auto title = req.value("title", std::string{"Open Folder"});
                path = m_window->open_folder_dialog(title.c_str());
            }
            return ok_response(path.empty() ? json(nullptr) : json(path));
        } catch (const std::exception& e) {
            return err_response(std::string{"dialog error: "} + e.what());
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    try {
        // Command modules — split per domain into editor_server_cmds_*.cpp.
        // Each returns the response string when it handled `cmd`, nullopt
        // otherwise. All run with m_mutex held, inside this try block.
        if (auto r = handle_scene_commands(cmd, req))    return *r;
        if (auto r = handle_render_commands(cmd, req))   return *r;
        if (auto r = handle_material_commands(cmd, req)) return *r;
        if (auto r = handle_graph_commands(cmd, req))    return *r;
        if (auto r = handle_timeline_commands(cmd, req)) return *r;
        if (auto r = handle_io_commands(cmd, req))       return *r;

        // Native dialog commands are handled above the mutex, before this
        // switch — their runModal pumps the main run loop and can't safely
        // hold m_mutex while doing so.

        return err_response("Unknown command: " + cmd);

    } catch (const json::exception& e) {
        return err_response(std::string{"JSON error: "} + e.what());
    } catch (const std::exception& e) {
        return err_response(e.what());
    } catch (...) {
        return err_response("Unknown error");
    }
}

}  // namespace tracey_editor
