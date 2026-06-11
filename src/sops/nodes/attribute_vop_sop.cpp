// AttributeVopSop — host SOP that owns a child VopGraph and runs it per
// point on the input geometry. Houdini's "Attribute VOP" SOP equivalent.
//
// Animation model: Houdini-style "promote to parameter". Each VOP-side knob
// the user wants to animate is promoted up to a host SOP parameter via
// promoteAttributeVopParam(). The host param then becomes the source of
// truth — animatable through the existing SOP keyframe / dopesheet path —
// and `cookAt(inputs, time)` stamps the time-sampled host value into the
// inner VOP node before the per-point eval. No new keyframe IPC, no
// VOP-side addressing.
//
// v1 deferred items:
//   • Constant-vs-port-input parameter modes (Houdini-style "linked"
//     params) — VOP node parameters are knobs only.
//   • Multi-geometry inputs — only one input geometry.
//   • The frontend canvas/palette/inspector were copied (third copy) rather
//     than unified; refactor is the natural follow-up.
//
// Cook contract:
//   • Single Geometry input, single Geometry output.
//   • Cook clones the input, stamps any promoted host params back into the
//     matching VOP nodes (time-sampled), calls each VopNode::prepare(geo)
//     once so geo_output materialises target attributes, then iterates
//     points and calls VopGraph::evaluatePoint(idx, geo) per point.

#include "attribute_vop_sop.hpp"

#include "../parameter.hpp"
#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include "../../core/parallel.hpp"
#include "../../vops/vop_graph.hpp"
#include "../../vops/vop_node.hpp"
#include "../../vops/vop_registry.hpp"
#include "../../vops/serialization.hpp"
#include "../../vops/codegen/compute_dispatch.hpp"

#include "json.hpp" // nlohmann/json (bundled via deps/tinygltf)

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class AttributeVopSop : public SopNode
            {
            public:
                explicit AttributeVopSop(size_t uid) : SopNode(uid) {}

                std::string kind() const override { return "attribute_vop"; }

                // The hosted VOP kernel can sample time (promoted host params
                // are evaluated per-cook via paramXxxAt), so the same params +
                // inputs can still produce different geometry at different
                // playhead positions.
                bool isTimeDependent() const override { return true; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("in", DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    // Time-independent path; only used when called through
                    // the SopNode::cook fallback (no playhead). Delegates to
                    // cookAt at t=0 so promoted-param stamping still runs
                    // with the constant baseline values.
                    return cookAt(inputs, 0.0);
                }

                Geometry cookAt(std::span<const Geometry *const> inputs,
                                double time) const override
                {
                    if (inputs.empty() || !inputs[0]) return {};
                    Geometry out = *inputs[0];
                    if (!m_vopGraph) return out;

                    // Stamp host-sampled values back into VOP nodes for each
                    // promotion. The worker holds its own deserialized
                    // SopGraph + VopGraph copy per cook, so mutating in place
                    // is safe — the copy is thrown away when the next cook
                    // request lands.
                    if (!m_promotions.empty())
                    {
                        stampPromotedParams(time);
                    }

                    // Run prepare() once before the per-point loop so
                    // geo_output can materialise any attribute it's about
                    // to write. Must happen serially BEFORE the parallel
                    // section: prepare() can `add<>` an attribute, which
                    // reallocates the attribute table's storage. Doing
                    // that concurrently with reads in evaluate() would race.
                    for (const auto &n : m_vopGraph->nodes())
                    {
                        if (auto *vn = dynamic_cast<vops::VopNode *>(n.get()))
                            vn->prepare(out);
                    }
                    // Hoist compile() out of the per-point loop. evaluatePoint
                    // does it lazily but calling it once up-front prevents
                    // the parallel workers from racing into the first compile().
                    m_vopGraph->compile();

                    // Try GPU dispatch first when the editor wired up a
                    // VopComputeDispatcher and the point count justifies
                    // the upload / dispatch overhead. The threshold is
                    // tuned around where CPU loops match a single GPU
                    // dispatch on M-series hardware; below it the host
                    // path is faster. On any failure (unsupported node,
                    // shader compile error, transient GPU issue) we log
                    // once and fall through to the CPU evaluator so the
                    // cook still completes.
                    constexpr size_t kGpuThreshold = 512;
                    auto *dispatcher = vops::codegen::VopComputeDispatcher::getGlobal();
                    if (dispatcher && out.points().size() >= kGpuThreshold)
                    {
                        try
                        {
                            dispatcher->dispatch(*m_vopGraph, out);
                            return out;
                        }
                        catch (const std::exception &e)
                        {
                            std::fprintf(stderr,
                                "[attribute_vop] GPU dispatch failed, CPU fallback: %s\n",
                                e.what());
                        }
                    }

                    // Parallel-for across points via the shared
                    // parallel_for_chunks helper (src/core/parallel.hpp).
                    // Each evaluate() reads + writes attributes at its
                    // own pointIndex, so no aliasing across threads.
                    // Per-thread slot buffer is reused across points in
                    // the chunk to avoid a heap allocation per point.
                    // The helper short-circuits to serial below the
                    // chunk threshold so tiny graphs don't pay thread-
                    // startup cost.
                    tracey::parallel_for_chunks(out.points().size(),
                        [this, &out](size_t begin, size_t end) {
                            std::vector<vops::Value> slots;
                            for (size_t i = begin; i < end; ++i)
                                m_vopGraph->evaluatePoint(i, out, slots);
                        });
                    return out;
                }

                // Walk promotions, sample the host SOP param at `time`, and
                // write the value back into the matching VOP node's
                // parameter. const because m_vopGraph is mutable and the
                // SopNode::cookAt contract is const.
                void stampPromotedParams(double time) const
                {
                    if (!m_vopGraph) return;
                    for (const auto &p : m_promotions)
                    {
                        vops::VopNode *vn = nullptr;
                        for (const auto &n : m_vopGraph->nodes())
                        {
                            if (auto *cand = dynamic_cast<vops::VopNode *>(n.get());
                                cand && cand->uid() == p.vopNodeUid)
                            {
                                vn = cand;
                                break;
                            }
                        }
                        if (!vn) continue;
                        switch (p.paramType)
                        {
                        case ParamType::Float:
                            vn->setParamFloat(p.vopParamName,
                                              paramFloatAt(p.hostParamName, time));
                            break;
                        case ParamType::Int:
                            vn->setParamInt(p.vopParamName,
                                            paramIntAt(p.hostParamName, time));
                            break;
                        case ParamType::Bool:
                            vn->setParamBool(p.vopParamName,
                                             paramBoolAt(p.hostParamName, time));
                            break;
                        case ParamType::Vec3:
                            vn->setParamVec3(p.vopParamName,
                                             paramVec3At(p.hostParamName, time));
                            break;
                        case ParamType::String:
                            // Strings aren't animatable in v1 (channels are
                            // numeric); just pass the current constant
                            // through so changes to the host param still
                            // reach the VOP.
                            vn->setParamString(p.vopParamName,
                                               paramString(p.hostParamName, std::string{}));
                            break;
                        }
                    }
                }

                vops::VopGraph &vopGraph()
                {
                    if (!m_vopGraph) m_vopGraph = makeSeededVopGraph();
                    return *m_vopGraph;
                }
                const vops::VopGraph &vopGraph() const
                {
                    if (!m_vopGraph)
                    {
                        // Lazy alloc on first const access too. Const because
                        // outside callers expect a stable reference; mutable
                        // member makes this safe.
                        m_vopGraph = makeSeededVopGraph();
                    }
                    return *m_vopGraph;
                }

                // Build a fresh VopGraph with a single geo_input → geo_output
                // pair, pre-wired one-to-one for the standard point
                // attributes (P, N, Cd, uv, v, Alpha). Diving into a brand-
                // new attribute_vop drops you into a fully-wired graph
                // that emits the same geometry as it received, so the user
                // can immediately right-click any wire and "insert on wire"
                // to splice in a node between input and output.
                //
                // VOPs flow left-to-right in the UI; one bezier hop per
                // wire is the natural shape. We position geo_input on the
                // left and geo_output on the right, both at the same y.
                //
                // The port indices below must stay in lockstep with the
                // kVecPorts / kFloatPorts tables in geo_io_vops.cpp.
                static std::unique_ptr<vops::VopGraph> makeSeededVopGraph()
                {
                    auto g = std::make_unique<vops::VopGraph>(0);
                    auto &reg = vops::VopRegistry::instance();

                    auto inNode  = reg.create("geo_input",  g->nextUid());
                    auto outNode = reg.create("geo_output", g->nextUid());
                    if (!inNode || !outNode) return g;

                    inNode->setPos( 80.0f, 60.0f);
                    outNode->setPos(360.0f, 60.0f);
                    const size_t inUid  = inNode->uid();
                    const size_t outUid = outNode->uid();
                    g->addNode(std::move(inNode));
                    g->addNode(std::move(outNode));

                    // (port index on geo_input, same index on geo_output).
                    // Both nodes order their vec3 ports identically (P, N,
                    // Cd, uv, v, force) followed by float ports (Alpha,
                    // pscale). We pre-wire the six the user most often
                    // edits; force / pscale stay unconnected with their
                    // default passthrough=true.
                    static constexpr size_t kPassthroughPorts[] = {
                        0, // P
                        1, // N
                        2, // Cd
                        3, // uv
                        4, // v
                        6, // Alpha
                    };
                    for (size_t port : kPassthroughPorts)
                        g->addConnection({inUid, port, outUid, port});

                    return g;
                }

                void setVopGraph(std::unique_ptr<vops::VopGraph> g)
                {
                    m_vopGraph = std::move(g);
                }

                // Round-trip the child VopGraph + promotion list as a single
                // JSON object under the host SopNode's "extra" field. Keeps
                // the entire SOP+VOP tree in one nlohmann::json document.
                std::string serializeExtraJson() const override
                {
                    nlohmann::json out;
                    if (m_vopGraph)
                    {
                        out["vop_graph"] = nlohmann::json::parse(
                            vops::serializeVopGraph(*m_vopGraph));
                    }
                    if (!m_promotions.empty())
                    {
                        nlohmann::json arr = nlohmann::json::array();
                        for (const auto &p : m_promotions)
                        {
                            nlohmann::json pj = {
                                {"vop_node_uid",   p.vopNodeUid},
                                {"vop_param_name", p.vopParamName},
                                {"host_param_name", p.hostParamName},
                                {"type",           paramTypeName(p.paramType)},
                            };
                            // Forward the VOP-side ParamSpec hints so the
                            // host SOP inspector can render sliders /
                            // dropdowns on promoted rows, not just bare
                            // number inputs. Only emitted when set.
                            if (p.rangeMin != p.rangeMax)
                            {
                                pj["range"] = {
                                    {"min",  p.rangeMin},
                                    {"max",  p.rangeMax},
                                    {"step", p.rangeStep},
                                };
                            }
                            if (!p.options.empty()) pj["options"] = p.options;
                            arr.push_back(std::move(pj));
                        }
                        out["promotions"] = std::move(arr);
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
                        m_promotions.clear();
                        if (j.contains("promotions") && j["promotions"].is_array())
                        {
                            for (const auto &pj : j["promotions"])
                            {
                                VopPromotion p;
                                p.vopNodeUid    = pj.value("vop_node_uid", size_t{0});
                                p.vopParamName  = pj.value("vop_param_name", std::string{});
                                p.hostParamName = pj.value("host_param_name", std::string{});
                                const std::string t = pj.value("type", std::string{"float"});
                                if      (t == "float")  p.paramType = ParamType::Float;
                                else if (t == "int")    p.paramType = ParamType::Int;
                                else if (t == "bool")   p.paramType = ParamType::Bool;
                                else if (t == "vec3")   p.paramType = ParamType::Vec3;
                                else                    p.paramType = ParamType::String;
                                if (pj.contains("range") && pj["range"].is_object())
                                {
                                    const auto &rj = pj["range"];
                                    p.rangeMin  = rj.value("min",  0.0);
                                    p.rangeMax  = rj.value("max",  0.0);
                                    p.rangeStep = rj.value("step", 0.0);
                                }
                                if (pj.contains("options") && pj["options"].is_array())
                                {
                                    for (const auto &opt : pj["options"])
                                    {
                                        if (opt.is_string()) p.options.push_back(opt.get<std::string>());
                                    }
                                }
                                // Re-declare the host param slot. promote()
                                // does this at first promotion, but after a
                                // set_sop_graph round-trip the node is built
                                // fresh and the slot would be missing — so
                                // applyParamFromJson's setParamFloat would
                                // silently no-op (findParam returns null)
                                // and slider edits never stick. Declaring
                                // here with a placeholder value lets the
                                // subsequent params block fill the actual
                                // value.
                                Parameter slot;
                                slot.name = p.hostParamName;
                                slot.type = p.paramType;
                                switch (p.paramType)
                                {
                                case ParamType::Float:  slot.value = 0.0f; break;
                                case ParamType::Int:    slot.value = 0;    break;
                                case ParamType::Bool:   slot.value = false; break;
                                case ParamType::Vec3:   slot.value = Vec3(0.0f); break;
                                case ParamType::String: slot.value = std::string{}; break;
                                }
                                declareParam(std::move(slot));
                                m_promotions.push_back(std::move(p));
                            }
                        }
                    }
                    catch (...)
                    {
                        // Bad payload — leave the (possibly null / empty) graph
                        // alone rather than crash the SOP load.
                    }
                }

                // ── Promotion management (called by the editor IPCs) ────────
                const std::vector<VopPromotion> &promotions() const { return m_promotions; }

                // Promote a VOP param to the host SOP. Returns the new host
                // param name (auto-generated, unique against existing host
                // params + promotions). Empty string means "couldn't promote"
                // (no matching VOP node or param). Channels start empty.
                std::string promote(size_t vopUid, const std::string &vopParamName)
                {
                    if (!m_vopGraph) return {};
                    vops::VopNode *vn = nullptr;
                    for (const auto &n : m_vopGraph->nodes())
                    {
                        if (auto *cand = dynamic_cast<vops::VopNode *>(n.get());
                            cand && cand->uid() == vopUid)
                        {
                            vn = cand;
                            break;
                        }
                    }
                    if (!vn) return {};

                    // Find the param + its type/value on the VOP node.
                    const Parameter *vopParam = nullptr;
                    for (const auto &q : vn->parameters())
                    {
                        if (q.name == vopParamName) { vopParam = &q; break; }
                    }
                    if (!vopParam) return {};

                    // Generate a unique host param name. Start with the VOP
                    // node's kind() + "_" + param name (e.g. "noise_vop_frequency"),
                    // append a numeric suffix if it collides with another
                    // promotion or an existing host param.
                    const std::string base = vn->kind() + "_" + vopParamName;
                    std::string hostName = base;
                    int suffix = 1;
                    while (hostParamExists(hostName))
                    {
                        hostName = base + "_" + std::to_string(suffix++);
                    }

                    // Declare the host param with the VOP param's current
                    // value as the constant baseline.
                    Parameter hostParam = *vopParam;
                    hostParam.name = hostName;
                    hostParam.channels.clear();
                    declareParam(std::move(hostParam));

                    // Record the promotion, forwarding any UI hints
                    // (range / options) from the VOP-side ParamSpec so the
                    // host inspector renders matching slider / dropdown
                    // widgets — see VopPromotion comment.
                    VopPromotion p;
                    p.vopNodeUid    = vopUid;
                    p.vopParamName  = vopParamName;
                    p.hostParamName = hostName;
                    p.paramType     = vopParam->type;
                    for (const auto &entry : vops::VopRegistry::instance().catalog())
                    {
                        if (entry.kind != vn->kind()) continue;
                        for (const auto &ps : entry.params)
                        {
                            if (ps.name != vopParamName) continue;
                            p.rangeMin  = ps.rangeMin;
                            p.rangeMax  = ps.rangeMax;
                            p.rangeStep = ps.rangeStep;
                            p.options   = ps.options;
                            break;
                        }
                        break;
                    }
                    m_promotions.push_back(std::move(p));
                    return hostName;
                }

                bool demote(const std::string &hostName)
                {
                    auto it = std::find_if(m_promotions.begin(), m_promotions.end(),
                        [&](const VopPromotion &p) { return p.hostParamName == hostName; });
                    if (it == m_promotions.end()) return false;
                    m_promotions.erase(it);

                    // Strip the host param. (Channels on it, if any, go with
                    // it — the user can re-promote and re-key.)
                    auto &params = parameters();
                    params.erase(std::remove_if(params.begin(), params.end(),
                        [&](const Parameter &p) { return p.name == hostName; }),
                        params.end());
                    return true;
                }

            private:
                bool hostParamExists(const std::string &name) const
                {
                    for (const auto &p : parameters())
                        if (p.name == name) return true;
                    return false;
                }

                // Held by pointer so deserializeExtraJson can replace it
                // wholesale (Graph has move-only members; pointer swap is
                // simpler than synthesising a move assignment up the chain).
                // Mutable so const accessors / cook can lazily allocate it
                // and so compile() can update internal caches.
                mutable std::unique_ptr<vops::VopGraph> m_vopGraph;
                // Promotions are storage-only; the host's declared params
                // (via SopNode::declareParam) are the authoritative source
                // of names/types/values for the inspector + animation. The
                // list lets cookAt walk them efficiently and lets demote()
                // find the host param to strip.
                std::vector<VopPromotion> m_promotions;
            };
        }

        void registerAttributeVopSop()
        {
            SopRegistry::instance().registerType(
                {"attribute_vop", "Attribute VOP", "Modifiers",
                 /*inputs*/ {{"in"}}, /*outputs*/ {{"out"}},
                 /*params*/ {}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<AttributeVopSop>(uid);
                });
        }

        vops::VopGraph *attributeVopGraph(SopNode *node)
        {
            auto *host = dynamic_cast<AttributeVopSop *>(node);
            return host ? &host->vopGraph() : nullptr;
        }

        const vops::VopGraph *attributeVopGraph(const SopNode *node)
        {
            const auto *host = dynamic_cast<const AttributeVopSop *>(node);
            return host ? &host->vopGraph() : nullptr;
        }

        void setAttributeVopGraph(SopNode *node, std::unique_ptr<vops::VopGraph> graph)
        {
            if (auto *host = dynamic_cast<AttributeVopSop *>(node))
            {
                host->setVopGraph(std::move(graph));
            }
        }

        const std::vector<VopPromotion> *attributeVopPromotions(const SopNode *node)
        {
            const auto *host = dynamic_cast<const AttributeVopSop *>(node);
            return host ? &host->promotions() : nullptr;
        }

        std::string promoteAttributeVopParam(SopNode *node,
                                             size_t vopNodeUid,
                                             const std::string &vopParamName)
        {
            auto *host = dynamic_cast<AttributeVopSop *>(node);
            return host ? host->promote(vopNodeUid, vopParamName) : std::string{};
        }

        bool demoteAttributeVopParam(SopNode *node, const std::string &hostParamName)
        {
            auto *host = dynamic_cast<AttributeVopSop *>(node);
            return host && host->demote(hostParamName);
        }

        void syncPromotedHostValuesFromVop(SopNode *node)
        {
            auto *host = dynamic_cast<AttributeVopSop *>(node);
            if (!host) return;
            auto *vopGraph = attributeVopGraph(node);
            if (!vopGraph) return;
            const auto &proms = host->promotions();
            if (proms.empty()) return;
            auto &hostParams = host->parameters();
            for (const auto &p : proms)
            {
                // Find the VOP node by uid.
                vops::VopNode *vn = nullptr;
                for (const auto &n : vopGraph->nodes())
                {
                    if (auto *cand = dynamic_cast<vops::VopNode *>(n.get());
                        cand && cand->uid() == p.vopNodeUid)
                    {
                        vn = cand;
                        break;
                    }
                }
                if (!vn) continue;
                // Find the host SOP param by name.
                Parameter *hp = nullptr;
                for (auto &q : hostParams)
                {
                    if (q.name == p.hostParamName) { hp = &q; break; }
                }
                if (!hp) continue;
                // Copy the VOP-side value into the host param's constant
                // baseline. Channels stay as-is so animated promotions
                // keep their keyframes.
                switch (p.paramType)
                {
                case ParamType::Float:
                    hp->value = vn->paramFloat(p.vopParamName, 0.0f);
                    break;
                case ParamType::Int:
                    hp->value = vn->paramInt(p.vopParamName, 0);
                    break;
                case ParamType::Bool:
                    hp->value = vn->paramBool(p.vopParamName, false);
                    break;
                case ParamType::Vec3:
                    hp->value = vn->paramVec3(p.vopParamName, Vec3(0.0f));
                    break;
                case ParamType::String:
                    hp->value = vn->paramString(p.vopParamName, std::string{});
                    break;
                }
            }
        }
    }
}
