// pop_force — DOP node that hosts a VopGraph and runs it per particle
// every substep. The VOP subnet's geo_output node writes into the
// `force` point attribute, which pop_solver then consumes.
//
// Same shape as src/sops/nodes/attribute_vop_sop.cpp:
//   • Owns a std::unique_ptr<VopGraph> lazily seeded with a passthrough
//     geo_input.force → geo_output.force wire.
//   • prepare() calls each VOP node's prepare() against the SimState's
//     geometry so geo_output can materialise the target attributes it
//     plans to write (e.g. user routes noise → geo_output.Cd to drive
//     particle color from the same subnet).
//   • cookFrame() walks every particle and calls VopGraph::evaluatePoint
//     so the geo_input/geo_output terminals interact with the live
//     geometry.
//
// VOP-side param promotion to host-DopNode parameters is deferred — the
// cookFrame path doesn't sample time-animated host params yet because
// DopGraph::cookFrame has no time-sampling hook. Add when needed.

#include "pop_force.hpp"

#include "../dop_node.hpp"
#include "../dop_graph.hpp"
#include "../dop_registry.hpp"
#include "../sim_state.hpp"

#include "../../core/parallel.hpp"
#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute_table.hpp"
#include "../../vops/vop_graph.hpp"
#include "../../vops/vop_node.hpp"
#include "../../vops/vop_registry.hpp"
#include "../../vops/serialization.hpp"
#include "../../vops/codegen/compute_dispatch.hpp"

#include "json.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace tracey
{
    namespace dops
    {
        namespace
        {
            class PopForceDop : public DopNode
            {
            public:
                explicit PopForceDop(size_t uid) : DopNode(uid) {}

                std::string kind() const override { return "pop_force"; }
                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    // Like other force DOPs, pop_force takes no explicit
                    // sim-data wire — it operates on the shared SimState
                    // via attribute reads/writes. The single "out" port
                    // exists so the user can keep the visual chain linear
                    // (source → force → solver) and topo sort runs them
                    // in the right order.
                    io.addInput(PortInfo::createInput("in", DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                void prepare(SimState &state) const override
                {
                    Geometry &g = state.geometry;
                    // Make sure `force` exists before the VOP subnet's
                    // geo_input.force / geo_output.force ports read or
                    // write it.
                    if (!g.points().get<Vec3>("force"))
                        g.points().add<Vec3>("force", Vec3(0.0f));

                    if (!m_vopGraph) m_vopGraph = makeSeededVopGraph();

                    // Run each VOP node's prepare() so geo_output can
                    // materialise the attributes it's about to write. Same
                    // serial-before-parallel reasoning as attribute_vop_sop:
                    // adding a new attribute reallocates the table.
                    for (const auto &n : m_vopGraph->nodes())
                    {
                        if (auto *vn = dynamic_cast<vops::VopNode *>(n.get()))
                            vn->prepare(g);
                    }
                    m_vopGraph->compile();
                }

                void cookFrame(DopEvalContext &ctx) const override
                {
                    if (!ctx.state || !m_vopGraph) return;
                    Geometry &g = ctx.state->geometry;
                    const size_t n = g.pointCount();
                    if (n == 0) return;

                    // GPU dispatch when wired up and the particle count
                    // justifies upload+dispatch overhead. pop_force runs
                    // every substep so the win adds up fast — the
                    // pipeline cache means only the first cook compiles
                    // the shader; subsequent dispatches reuse it.
                    constexpr size_t kGpuThreshold = 512;
                    auto *dispatcher = vops::codegen::VopComputeDispatcher::getGlobal();
                    if (dispatcher && n >= kGpuThreshold)
                    {
                        try
                        {
                            dispatcher->dispatch(*m_vopGraph, g);
                            return;
                        }
                        catch (const std::exception &e)
                        {
                            std::fprintf(stderr,
                                "[pop_force] GPU dispatch failed, CPU fallback: %s\n",
                                e.what());
                        }
                    }

                    // CPU parallel-for fallback. The VopGraph's compile()
                    // ran in prepare() (serial, BEFORE the worker fan-
                    // out) so the slot table is already built;
                    // geo_input/geo_output reads + writes touch per-point
                    // indices exclusively, so threads don't alias each
                    // other. Each thread owns a fresh slot buffer reused
                    // across its chunk — same shape as
                    // attribute_vop_sop's parallel-for.
                    tracey::parallel_for_chunks(n, [this, &g](size_t begin, size_t end) {
                        std::vector<vops::Value> slots;
                        for (size_t i = begin; i < end; ++i)
                            m_vopGraph->evaluatePoint(i, g, slots);
                    });
                }

                vops::VopGraph &vopGraph()
                {
                    if (!m_vopGraph) m_vopGraph = makeSeededVopGraph();
                    return *m_vopGraph;
                }
                const vops::VopGraph &vopGraph() const
                {
                    if (!m_vopGraph) m_vopGraph = makeSeededVopGraph();
                    return *m_vopGraph;
                }
                void setVopGraph(std::unique_ptr<vops::VopGraph> g)
                {
                    m_vopGraph = std::move(g);
                }

                // Fresh-graph seed: geo_input → geo_output with the
                // `force` port wired through. The user inserts force-
                // producing nodes on that wire (or adds noise → multiply
                // → geo_output.force from scratch). Matches
                // attribute_vop_sop's seed shape; only the pre-wired port
                // differs, because pop_force is specifically about
                // driving the `force` attribute.
                //
                // Port index 5 is `force` on both nodes — see
                // kVecPorts in src/vops/nodes/geo_io_vops.cpp.
                static std::unique_ptr<vops::VopGraph> makeSeededVopGraph()
                {
                    auto g = std::make_unique<vops::VopGraph>(0);
                    auto &reg = vops::VopRegistry::instance();
                    auto inNode  = reg.create("geo_input",  g->nextUid());
                    auto outNode = reg.create("geo_output", g->nextUid());
                    if (!inNode || !outNode) return g; // registry not loaded yet
                    inNode->setPos ( 80.0f, 60.0f);
                    outNode->setPos(360.0f, 60.0f);
                    const size_t inUid  = inNode->uid();
                    const size_t outUid = outNode->uid();
                    g->addNode(std::move(inNode));
                    g->addNode(std::move(outNode));
                    constexpr size_t kForcePort = 5;
                    g->addConnection({inUid, kForcePort, outUid, kForcePort});
                    return g;
                }

                std::string serializeExtraJson() const override
                {
                    nlohmann::json out;
                    if (m_vopGraph)
                    {
                        out["vop_graph"] = nlohmann::json::parse(
                            vops::serializeVopGraph(*m_vopGraph));
                    }
                    return out.dump();
                }

                void deserializeExtraJson(const std::string &jsonText) override
                {
                    if (jsonText.empty()) return;
                    try
                    {
                        auto j = nlohmann::json::parse(jsonText);
                        if (j.contains("vop_graph"))
                        {
                            m_vopGraph = vops::deserializeVopGraph(
                                j["vop_graph"].dump());
                        }
                    }
                    catch (...)
                    {
                        // Bad payload — leave the (possibly null) graph alone.
                    }
                }

            private:
                // mutable so const accessors can lazily seed it. Same
                // pattern as attribute_vop_sop's m_vopGraph.
                mutable std::unique_ptr<vops::VopGraph> m_vopGraph;
            };
        }

        vops::VopGraph *popForceVopGraph(DopNode *node)
        {
            auto *self = dynamic_cast<PopForceDop *>(node);
            if (!self) return nullptr;
            return &self->vopGraph();
        }
        const vops::VopGraph *popForceVopGraph(const DopNode *node)
        {
            const auto *self = dynamic_cast<const PopForceDop *>(node);
            if (!self) return nullptr;
            return &self->vopGraph();
        }
        void setPopForceVopGraph(DopNode *node, std::unique_ptr<vops::VopGraph> graph)
        {
            auto *self = dynamic_cast<PopForceDop *>(node);
            if (!self) return;
            self->setVopGraph(std::move(graph));
        }

        void registerPopForceDop()
        {
            DopRegistry::instance().registerType(
                {"pop_force", "Force (VOP)", "Force",
                 /*inputs*/ {{"in"}},
                 /*outputs*/ {{"out"}},
                 /*params*/ {}},
                [](size_t uid) -> std::unique_ptr<DopNode> {
                    return std::make_unique<PopForceDop>(uid);
                });
        }
    }
}
