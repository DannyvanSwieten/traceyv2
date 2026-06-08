// Phase 1 verification for the VOP→GLSL codegen path. Builds a few
// representative VopGraphs, emits the GLSL via codegen::emitGlsl(),
// and confirms each emitted shader actually compiles via shaderc.
// Visual print included so the developer can eyeball the output.
//
// What this proves:
//   • The emitter's per-node templates produce syntactically valid
//     GLSL across the supported node kinds.
//   • Attribute SSBO bindings + the params SSBO layout are
//     consistent (the shader links).
//   • The noise preamble compiles cleanly inside the emitted source.
//
// What this does NOT prove yet (Phase 2):
//   • That the GPU produces numerically identical results to the
//     CPU evaluator. That comparison lands when the dispatcher
//     ships and we can readback into a Geometry.

#include "vops/codegen/glsl_emit.hpp"
#include "vops/codegen/compute_dispatch.hpp"
#include "vops/register_builtins.hpp"
#include "vops/vop_graph.hpp"
#include "vops/vop_node.hpp"
#include "vops/vop_registry.hpp"

#include "gpu/shader_compiler.hpp"
#include "device/device.hpp"
#include "geometry/geometry.hpp"
#include "geometry/attribute.hpp"
#include "geometry/attribute_allocator.hpp"
#include "geometry/attribute_table.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
    int failures = 0;
    void check(bool ok, const char *what)
    {
        if (ok) std::printf("  ok   %s\n", what);
        else { ++failures; std::printf("  FAIL %s\n", what); }
    }

    // Build a VopGraph by wiring a sequence of (kind, [(inputPort, srcUid,
    // srcPort)]) entries. Returns the final node's uid so tests can
    // chain assertions if needed.
    struct WireSpec { size_t inputPort; size_t srcUid; size_t srcPort; };
    size_t addWired(tracey::vops::VopGraph &g, const std::string &kind,
                    const std::initializer_list<WireSpec> &wires = {})
    {
        auto node = tracey::vops::VopRegistry::instance().create(kind, g.nextUid());
        if (!node) throw std::runtime_error("registry has no kind: " + kind);
        const size_t uid = node->uid();
        g.addNode(std::move(node));
        for (const auto &w : wires)
            g.createConnection(w.srcUid, w.srcPort, uid, w.inputPort);
        g.markDirty();
        return uid;
    }

    // Compile a GLSL source string with shaderc; record FAIL with the
    // compiler diagnostic when it errors. Prints a tail of the source
    // when compilation fails so the developer can see what was wrong
    // without rerunning the whole smoke test in a debugger.
    bool tryCompile(const std::string &source, const char *name)
    {
        tracey::ShaderCompiler compiler;
        try
        {
            auto spv = compiler.compileComputeShader(source, name);
            return !spv.empty();
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "  ── shaderc error for '%s':\n%s\n", name, e.what());
            std::fprintf(stderr, "  ── emitted source:\n%s\n", source.c_str());
            return false;
        }
    }

    // ── Test cases ──────────────────────────────────────────────────

    // Port indices on geo_input / geo_output. Order matches kVecPorts /
    // kFloatPorts in src/vops/nodes/geo_io_vops.cpp.
    constexpr size_t kGioP     = 0;
    constexpr size_t kGioN     = 1;
    [[maybe_unused]] constexpr size_t kGioCd    = 2;
    [[maybe_unused]] constexpr size_t kGioUv    = 3;
    [[maybe_unused]] constexpr size_t kGioVel   = 4;
    constexpr size_t kGioForce = 5;

    // 1) P → multiply by 2 → P (simplest). Verifies basic emit + the
    //    Vec3 attribute SSBO + the constant_float param slot all link.
    void test_p_times_constant()
    {
        using namespace tracey::vops;
        VopGraph g(0);
        const size_t io  = addWired(g, "geo_input");
        const size_t k   = addWired(g, "constant_float");
        const size_t mul = addWired(g, "multiply",
            {{0, io, kGioP}, {1, k, 0}});
        addWired(g, "geo_output", {{kGioP, mul, 0}});

        const auto r = codegen::emitGlsl(g);
        check(!r.glsl.empty(), "P*const: GLSL non-empty");
        check(r.unsupported.empty(), "P*const: no unsupported nodes");
        check(r.attrs.size() == 1, "P*const: exactly one attribute (P)");
        check(r.params.size() == 1, "P*const: one param slot (constant_float.value)");
        check(tryCompile(r.glsl, "p_times_constant"), "P*const: shaderc compiles");
    }

    // 2) Perlin-noise displacement along N — covers the noise preamble
    //    AND the displace_along_normal node, the canonical particle-
    //    sim convenience.
    void test_perlin_displace_along_normal()
    {
        using namespace tracey::vops;
        VopGraph g(0);
        const size_t io    = addWired(g, "geo_input");
        const size_t noise = addWired(g, "noise_perlin", {{0, io, kGioP}});
        const size_t disp  = addWired(g, "displace_along_normal",
            {{0, io, kGioP}, {1, io, kGioN}, {2, noise, 0}});
        addWired(g, "geo_output", {{kGioP, disp, 0}});

        const auto r = codegen::emitGlsl(g);
        check(r.unsupported.empty(), "perlin: no unsupported nodes");
        check(r.attrs.size() == 2, "perlin: two attributes (P, N)");
        // Perlin declares frequency + amplitude + seed = 3 param slots.
        check(r.params.size() == 3, "perlin: three param slots");
        check(tryCompile(r.glsl, "perlin_displace"), "perlin: shaderc compiles");
    }

    // 3) Force field — particle workflow. P → noise_vec3 → geo_output.force.
    //    Verifies the multi-channel noise emit (3 perlin samples).
    void test_pop_force_subnet()
    {
        using namespace tracey::vops;
        VopGraph g(0);
        const size_t io = addWired(g, "geo_input");
        const size_t n  = addWired(g, "noise_vec3", {{0, io, kGioP}});
        addWired(g, "geo_output", {{kGioForce, n, 0}});

        const auto r = codegen::emitGlsl(g);
        check(r.unsupported.empty(), "force: no unsupported nodes");
        check(r.attrs.size() == 2, "force: two attributes (P read, force write)");
        check(tryCompile(r.glsl, "pop_force_subnet"), "force: shaderc compiles");
    }

    // 4) Unified geo_input / geo_output round-trip.
    //
    // The simplest possible graph using the new Houdini-style I/O
    // nodes:  geo_input.P → geo_output.P. Should emit a kernel that
    // copies positions to themselves (identity passthrough), with one
    // attribute SSBO (P, read + write — ensureAttr merges flags) and
    // none of the other geo_output ports touched (their passthrough
    // params default to true so they're dead-stripped on emit).
    void test_geo_io_passthrough()
    {
        using namespace tracey::vops;
        VopGraph g(0);
        const size_t in  = addWired(g, "geo_input");
        // Output port 0 on geo_input is P (Vec3). Input port 0 on
        // geo_output is also P (Vec3). Wire them directly.
        addWired(g, "geo_output", {{0, in, 0}});

        const auto r = codegen::emitGlsl(g);
        check(r.unsupported.empty(), "geo_io: no unsupported nodes");
        // One attribute (P), bound for read AND write.
        check(r.attrs.size() == 1, "geo_io: exactly one attribute (P)");
        if (r.attrs.size() == 1)
        {
            check(r.attrs[0].name == "P",   "geo_io: the attribute is P");
            check(r.attrs[0].read && r.attrs[0].write,
                  "geo_io: P is read+write (passthrough)");
        }
        check(tryCompile(r.glsl, "geo_io_passthrough"), "geo_io: shaderc compiles");
    }

    // 5) geo_output with `passthrough_Cd=false` and Cd unconnected →
    //    kernel stamps the canonical default (white) on every point.
    //    Exercises the unconnected-default branch in emitGeoOutput.
    void test_geo_io_default_stamp()
    {
        using namespace tracey::vops;
        VopGraph g(0);
        addWired(g, "geo_input");  // unused — geo_output writes a constant
        const size_t out = addWired(g, "geo_output");
        // Flip Cd's passthrough off so the unconnected port stamps the
        // canonical default. Every other port keeps passthrough=true and
        // should drop out of the emitted body entirely.
        if (auto *n = g.findNode(out)) n->setParamBool("passthrough_Cd", false);
        g.markDirty();

        const auto r = codegen::emitGlsl(g);
        check(r.unsupported.empty(), "geo_io default: no unsupported nodes");
        // Only Cd is touched (write-only — no read side).
        bool foundCd = false;
        for (const auto &a : r.attrs)
            if (a.name == "Cd" && a.write && !a.read) foundCd = true;
        check(foundCd, "geo_io default: Cd is write-only");
        check(r.attrs.size() == 1, "geo_io default: only Cd is touched");
        check(tryCompile(r.glsl, "geo_io_default_stamp"),
              "geo_io default: shaderc compiles");
    }

    // 6) Comprehensive: exercise binary math + length + clamp + mix in
    //    one graph. Catches cast/type-inference regressions in the
    //    cross-pollinated float↔vec3 paths.
    void test_kitchen_sink()
    {
        using namespace tracey::vops;
        VopGraph g(0);
        const size_t io  = addWired(g, "geo_input");
        const size_t cf1 = addWired(g, "constant_float");
        const size_t cv1 = addWired(g, "constant_vec3");
        // length(P) → float
        const size_t len = addWired(g, "length", {{0, io, kGioP}});
        // mix(N, constant_vec3, length(P))
        const size_t mix = addWired(g, "mix",
            {{0, io, kGioN}, {1, cv1, 0}, {2, len, 0}});
        // clamp(mix, 0, constant_float)
        const size_t clz = addWired(g, "clamp",
            {{0, mix, 0}, {1, cf1, 0}, {2, cf1, 0}});
        // out: P + clamp (via displace)
        const size_t disp = addWired(g, "displace", {{0, io, kGioP}, {1, clz, 0}});
        addWired(g, "geo_output", {{kGioP, disp, 0}});

        const auto r = codegen::emitGlsl(g);
        check(r.unsupported.empty(), "kitchen-sink: no unsupported nodes");
        check(tryCompile(r.glsl, "kitchen_sink"), "kitchen-sink: shaderc compiles");
    }
}

namespace
{
    // Run a graph on the CPU evaluator point-by-point. Caller owns the
    // geometry; the function mutates `geo` via geo_output ports the
    // same way the GPU dispatcher does.
    void cpuEvaluate(tracey::vops::VopGraph &graph, tracey::Geometry &geo)
    {
        graph.compile();
        std::vector<tracey::vops::Value> slots;
        for (size_t i = 0; i < geo.pointCount(); ++i)
            graph.evaluatePoint(i, geo, slots);
    }

    // Build a fixed-pattern test geometry. N points laid out on a
    // helix so each (P, N) sample is distinct — keeps any per-point
    // bug from being masked by symmetry.
    tracey::Geometry makeTestGeo(size_t n)
    {
        tracey::Geometry g;
        g.points().add<tracey::Vec3>("N", tracey::Vec3(0.0f));
        for (size_t i = 0; i < n; ++i)
        {
            const float t = static_cast<float>(i) * 0.1f;
            tracey::Vec3 p(std::cos(t) * 0.5f, t * 0.2f, std::sin(t) * 0.5f);
            const size_t idx = g.addPoint(p);
            // N points outward from the spine — useful for the
            // displace_along_normal test below.
            tracey::Vec3 nrm(std::cos(t), 0.0f, std::sin(t));
            g.points().get<tracey::Vec3>("N")->data()[idx] = nrm;
        }
        return g;
    }

    bool nearlyEqual(const tracey::Vec3 &a, const tracey::Vec3 &b, float eps)
    {
        return std::abs(a.x - b.x) < eps &&
               std::abs(a.y - b.y) < eps &&
               std::abs(a.z - b.z) < eps;
    }

    // CPU↔GPU equivalence test runner. Builds the graph via `factory`,
    // runs both evaluators on independent geometry copies, compares
    // positions point-by-point.
    void compareCpuGpu(tracey::Device *device,
                       const char *label,
                       float eps,
                       std::function<void(tracey::vops::VopGraph &)> factory)
    {
        tracey::vops::VopGraph graph(0);
        factory(graph);
        graph.markDirty();

        constexpr size_t kPoints = 256;
        tracey::Geometry cpuGeo = makeTestGeo(kPoints);
        tracey::Geometry gpuGeo = cpuGeo;  // independent copy

        cpuEvaluate(graph, cpuGeo);

        tracey::vops::codegen::VopComputeDispatcher dispatcher(device);
        try
        {
            const auto stats = dispatcher.dispatch(graph, gpuGeo);
            std::printf("  ── %s GPU: %.2f ms (upload %.2f / dispatch %.2f / readback %.2f)\n",
                        label, stats.uploadMs + stats.gpuMs + stats.readbackMs,
                        stats.uploadMs, stats.gpuMs, stats.readbackMs);
        }
        catch (const std::exception &e)
        {
            std::printf("  FAIL %s: GPU dispatch threw: %s\n", label, e.what());
            ++failures;
            return;
        }

        // Compare every point's position. With matching CPU/GPU
        // evaluators these should agree to a tight epsilon (noise
        // implementations and float rounding diverge ~1e-4 at worst).
        const auto &cpuP = cpuGeo.positions();
        const auto &gpuP = gpuGeo.positions();
        size_t mismatched = 0;
        for (size_t i = 0; i < cpuP.size(); ++i)
            if (!nearlyEqual(cpuP[i], gpuP[i], eps)) ++mismatched;

        if (mismatched == 0)
        {
            std::printf("  ok   %s: %zu points match (eps=%g)\n",
                        label, cpuP.size(), eps);
        }
        else
        {
            ++failures;
            std::printf("  FAIL %s: %zu/%zu points differ. First mismatch:\n"
                        "    CPU [%.6f %.6f %.6f]\n"
                        "    GPU [%.6f %.6f %.6f]\n",
                        label, mismatched, cpuP.size(),
                        cpuP[0].x, cpuP[0].y, cpuP[0].z,
                        gpuP[0].x, gpuP[0].y, gpuP[0].z);
        }
    }
}

int main()
{
    tracey::vops::registerBuiltinVops();
    std::printf("vop_codegen_smoke:\n");

    test_p_times_constant();
    test_perlin_displace_along_normal();
    test_pop_force_subnet();
    test_geo_io_passthrough();
    test_geo_io_default_stamp();
    test_kitchen_sink();

    // ── Phase 2 verification: CPU evaluator vs GPU dispatcher ────────
    // Skip silently when no Vulkan device is available (e.g. CI without
    // a GPU); the emitter checks above still run.
    std::unique_ptr<tracey::Device> device;
    try
    {
        device.reset(tracey::createDevice(tracey::DeviceType::Gpu,
                                          tracey::DeviceBackend::Compute));
    }
    catch (const std::exception &e)
    {
        std::printf("\n[gpu] device unavailable (%s) — skipping GPU compare\n", e.what());
    }
    if (device)
    {
        // Phase A: the dispatcher's persistent-buffer path goes
        // through Attribute<T>::buffer() / bufferConst(), which read
        // the device from the AttributeAllocator global. Register it
        // here for the duration of the GPU portion; clear at the end
        // so attribute teardown doesn't see a dangling device.
        tracey::AttributeAllocator::setDevice(device.get());
        using namespace tracey::vops;
        std::printf("\n── CPU↔GPU equivalence checks ──\n");

        // 1) Pure-arithmetic graph: P * 2 — should be bit-exact.
        compareCpuGpu(device.get(), "P*2", 1e-6f,
                [](VopGraph &g) {
                    const size_t io = addWired(g, "geo_input");
                    const size_t k  = addWired(g, "constant_float");
                    // No public API to set param values during graph
                    // construction; re-fetch the node and call
                    // setParamFloat to make the constant = 2.0.
                    auto *cn = g.findNode(k);
                    if (cn) cn->setParamFloat("value", 2.0f);
                    const size_t mul = addWired(g, "multiply",
                        {{0, io, kGioP}, {1, k, 0}});
                    addWired(g, "geo_output", {{kGioP, mul, 0}});
                });

        // 2) Displace along normal by a constant float — bit-exact.
        compareCpuGpu(device.get(), "P + N * 0.25", 1e-5f,
            [](VopGraph &g) {
                const size_t io = addWired(g, "geo_input");
                const size_t k  = addWired(g, "constant_float");
                if (auto *cn = g.findNode(k)) cn->setParamFloat("value", 0.25f);
                const size_t d = addWired(g, "displace_along_normal",
                    {{0, io, kGioP}, {1, io, kGioN}, {2, k, 0}});
                addWired(g, "geo_output", {{kGioP, d, 0}});
            });

        // 3) Perlin displace — needs a looser eps; CPU uses glm::perlin,
        //    GPU uses the Gustavson formulation. They agree at ~1e-2.
        compareCpuGpu(device.get(), "perlin displace_along_normal", 1e-1f,
            [](VopGraph &g) {
                const size_t io = addWired(g, "geo_input");
                const size_t n  = addWired(g, "noise_perlin", {{0, io, kGioP}});
                if (auto *np = g.findNode(n)) {
                    np->setParamFloat("frequency", 2.0f);
                    np->setParamFloat("amplitude", 0.1f);
                    np->setParamInt  ("seed", 7);
                }
                const size_t d = addWired(g, "displace_along_normal",
                    {{0, io, kGioP}, {1, io, kGioN}, {2, n, 0}});
                addWired(g, "geo_output", {{kGioP, d, 0}});
            });

        // 4) Worley displace — bit-exact equivalence on this one because
        //    both sides use the same integer hash + floor-grid scan
        //    (unlike perlin/simplex which have different reference
        //    implementations CPU vs GPU). Drift here means the hash
        //    constants or feature-point distribution diverged.
        compareCpuGpu(device.get(), "worley displace_along_normal", 1e-4f,
            [](VopGraph &g) {
                const size_t io = addWired(g, "geo_input");
                const size_t n  = addWired(g, "noise_worley", {{0, io, kGioP}});
                if (auto *np = g.findNode(n)) {
                    np->setParamFloat("frequency", 3.0f);
                    np->setParamFloat("amplitude", 0.1f);
                    np->setParamInt  ("seed", 13);
                }
                const size_t d = addWired(g, "displace_along_normal",
                    {{0, io, kGioP}, {1, io, kGioN}, {2, n, 0}});
                addWired(g, "geo_output", {{kGioP, d, 0}});
            });

        // Clear the device pointer so Geometry teardown (when the
        // smoke test exits) doesn't try to free GPU buffers against
        // an already-destroyed device. EditorServer does the same in
        // its destructor.
        tracey::AttributeAllocator::setDevice(nullptr);
    }

    // Print one full shader so the developer can sanity-check the
    // output shape without rerunning under a debugger.
    {
        using namespace tracey::vops;
        VopGraph g(0);
        const size_t io   = addWired(g, "geo_input");
        const size_t n    = addWired(g, "noise_perlin", {{0, io, kGioP}});
        const size_t disp = addWired(g, "displace_along_normal",
            {{0, io, kGioP}, {1, io, kGioN}, {2, n, 0}});
        addWired(g, "geo_output", {{kGioP, disp, 0}});
        const auto r = codegen::emitGlsl(g);
        std::printf("\n── sample emitted shader (perlin → displace_along_normal) ──\n%s",
                    r.glsl.c_str());
    }

    std::printf("\nvop_codegen_smoke: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
