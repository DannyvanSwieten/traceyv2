// InstanceVopSop — terminal SOP that wraps `instance` with an inner VopGraph
// driving per-instance transform + tint. Houdini's "Instance VOP" analogue.
//
// Structurally a sibling of `attribute_vop` (it owns a VopGraph and exposes
// the same promote/demote/serialise plumbing), but the emit happens in
// SopGraph::cook (kind() == "instance_vop" branch) — same pattern as the
// plain `instance` terminal — because the VOP runs against a *synthetic*
// per-instance Geometry rather than the input geometry.
//
// Cook contract (handled by SopGraph::cook):
//   1. Input 0 = stamp Geometry; input 1 = template point cloud.
//   2. Synthesise a Geometry whose point table carries one point per
//      template entry, pre-populated with P / N / Cd / pscale (and age /
//      life / ptnum from the template if present).
//   3. Dispatch the inner VopGraph against that synthetic Geometry via
//      VopComputeDispatcher::getGlobal() — GPU path only; no CPU fallback
//      (the kernel is the hot loop).
//   4. Read the (possibly mutated) attrs back and pack them into one
//      EmittedActor with N InstanceEntries:
//         translate = P[i]
//         rotation  = orientFromNormal(N[i])  // if orient_to_normal=true
//         scale     = Vec3(pscale[i])
//         tint      = Cd[i]
//   5. Push the EmittedActor; apply_emitted turns it into one Scene Actor
//      with N SceneInstances + a shared BLAS.

#include "instance_vop_sop.hpp"

#include "../parameter.hpp"
#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include "../../vops/vop_graph.hpp"
#include "../../vops/vop_node.hpp"
#include "../../vops/vop_registry.hpp"
#include "../../vops/serialization.hpp"

#include "json.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class InstanceVopSop : public SopNode
            {
            public:
                explicit InstanceVopSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("name", "instance"));
                    declareParam(Parameter::makeBool("orient_to_normal", true));
                    declareParam(Parameter::makeString("material_library_name", ""));
                }

                std::string kind() const override { return "instance_vop"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("stamp",    DataType::Scene3D));
                    io.addInput(PortInfo::createInput("template", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const>) const override
                {
                    // Terminal — emit happens in SopGraph::cook. Same shape
                    // as the plain `instance` SOP.
                    return {};
                }

                // ── Inner VopGraph access ────────────────────────────────
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

                // Seed: geo_input → geo_output passthrough on P / N / Cd /
                // pscale. The user lands in a "every instance is just the
                // template point, untouched" graph and immediately sees
                // their stamps at the right places — then they insert
                // nodes on the wires (e.g. multiply Cd by noise, add to P)
                // to drive per-instance variation.
                static std::unique_ptr<vops::VopGraph> makeSeededVopGraph()
                {
                    auto g = std::make_unique<vops::VopGraph>(0);
                    auto &reg = vops::VopRegistry::instance();
                    auto in  = reg.create("geo_input",  g->nextUid());
                    auto out = reg.create("geo_output", g->nextUid());
                    if (!in || !out) return g;
                    in->setPos( 80.0f, 60.0f);
                    out->setPos(360.0f, 60.0f);
                    const size_t inUid  = in->uid();
                    const size_t outUid = out->uid();
                    g->addNode(std::move(in));
                    g->addNode(std::move(out));
                    // Port indices match geo_io_vops.cpp's kVecPorts /
                    // kFloatPorts order:
                    //   0=P 1=N 2=Cd 3=uv 4=v 5=force | 6=Alpha 7=pscale.
                    static constexpr size_t kPassthroughPorts[] = {
                        0, // P    → instance translate
                        1, // N    → instance orient
                        2, // Cd   → instance tint
                        7, // pscale → instance uniform scale
                    };
                    for (size_t port : kPassthroughPorts)
                        g->addConnection({inUid, port, outUid, port});
                    return g;
                }

                // ── Promotions (identical to attribute_vop_sop) ──────────
                const std::vector<InstanceVopPromotion> &promotions() const { return m_promotions; }

                std::string promote(size_t vopUid, const std::string &vopParamName)
                {
                    if (!m_vopGraph) return {};
                    vops::VopNode *vn = nullptr;
                    for (const auto &n : m_vopGraph->nodes())
                    {
                        if (auto *c = dynamic_cast<vops::VopNode *>(n.get());
                            c && c->uid() == vopUid) { vn = c; break; }
                    }
                    if (!vn) return {};
                    const Parameter *vopParam = nullptr;
                    for (const auto &q : vn->parameters())
                        if (q.name == vopParamName) { vopParam = &q; break; }
                    if (!vopParam) return {};

                    const std::string base = vn->kind() + "_" + vopParamName;
                    std::string hostName = base;
                    int suffix = 1;
                    while (hostParamExists(hostName))
                        hostName = base + "_" + std::to_string(suffix++);

                    Parameter hostParam = *vopParam;
                    hostParam.name = hostName;
                    hostParam.channels.clear();
                    declareParam(std::move(hostParam));

                    InstanceVopPromotion p;
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
                        [&](const InstanceVopPromotion &p) { return p.hostParamName == hostName; });
                    if (it == m_promotions.end()) return false;
                    m_promotions.erase(it);
                    auto &params = parameters();
                    params.erase(std::remove_if(params.begin(), params.end(),
                        [&](const Parameter &p) { return p.name == hostName; }),
                        params.end());
                    return true;
                }

                void stampPromotedParams(double time) const
                {
                    if (!m_vopGraph) return;
                    for (const auto &p : m_promotions)
                    {
                        vops::VopNode *vn = nullptr;
                        for (const auto &n : m_vopGraph->nodes())
                        {
                            if (auto *c = dynamic_cast<vops::VopNode *>(n.get());
                                c && c->uid() == p.vopNodeUid) { vn = c; break; }
                        }
                        if (!vn) continue;
                        switch (p.paramType)
                        {
                        case ParamType::Float:
                            vn->setParamFloat(p.vopParamName, paramFloatAt(p.hostParamName, time)); break;
                        case ParamType::Int:
                            vn->setParamInt(p.vopParamName, paramIntAt(p.hostParamName, time)); break;
                        case ParamType::Bool:
                            vn->setParamBool(p.vopParamName, paramBoolAt(p.hostParamName, time)); break;
                        case ParamType::Vec3:
                            vn->setParamVec3(p.vopParamName, paramVec3At(p.hostParamName, time)); break;
                        case ParamType::String:
                            vn->setParamString(p.vopParamName, paramString(p.hostParamName, std::string{})); break;
                        }
                    }
                }

                // ── Serialization ────────────────────────────────────────
                std::string serializeExtraJson() const override
                {
                    nlohmann::json out;
                    if (m_vopGraph)
                        out["vop_graph"] = nlohmann::json::parse(
                            vops::serializeVopGraph(*m_vopGraph));
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
                            if (p.rangeMin != p.rangeMax)
                                pj["range"] = {{"min", p.rangeMin}, {"max", p.rangeMax}, {"step", p.rangeStep}};
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
                            m_vopGraph = vops::deserializeVopGraph(j["vop_graph"].dump());
                        m_promotions.clear();
                        if (j.contains("promotions") && j["promotions"].is_array())
                        {
                            for (const auto &pj : j["promotions"])
                            {
                                InstanceVopPromotion p;
                                p.vopNodeUid    = pj.value("vop_node_uid", size_t{0});
                                p.vopParamName  = pj.value("vop_param_name", std::string{});
                                p.hostParamName = pj.value("host_param_name", std::string{});
                                const std::string t = pj.value("type", std::string{"float"});
                                if      (t == "float") p.paramType = ParamType::Float;
                                else if (t == "int")   p.paramType = ParamType::Int;
                                else if (t == "bool")  p.paramType = ParamType::Bool;
                                else if (t == "vec3")  p.paramType = ParamType::Vec3;
                                else                   p.paramType = ParamType::String;
                                if (pj.contains("range") && pj["range"].is_object())
                                {
                                    const auto &rj = pj["range"];
                                    p.rangeMin  = rj.value("min",  0.0);
                                    p.rangeMax  = rj.value("max",  0.0);
                                    p.rangeStep = rj.value("step", 0.0);
                                }
                                if (pj.contains("options") && pj["options"].is_array())
                                    for (const auto &o : pj["options"])
                                        if (o.is_string()) p.options.push_back(o.get<std::string>());
                                Parameter slot;
                                slot.name = p.hostParamName;
                                slot.type = p.paramType;
                                switch (p.paramType)
                                {
                                case ParamType::Float:  slot.value = 0.0f; break;
                                case ParamType::Int:    slot.value = 0; break;
                                case ParamType::Bool:   slot.value = false; break;
                                case ParamType::Vec3:   slot.value = Vec3(0.0f); break;
                                case ParamType::String: slot.value = std::string{}; break;
                                }
                                declareParam(std::move(slot));
                                m_promotions.push_back(std::move(p));
                            }
                        }
                    }
                    catch (...) { /* swallow — leave host empty */ }
                }

            private:
                bool hostParamExists(const std::string &name) const
                {
                    for (const auto &p : parameters())
                        if (p.name == name) return true;
                    return false;
                }

                mutable std::unique_ptr<vops::VopGraph> m_vopGraph;
                std::vector<InstanceVopPromotion>       m_promotions;
            };
        }

        void registerInstanceVopSop()
        {
            SopRegistry::instance().registerType(
                {"instance_vop", "Instance VOP", "Cloners",
                 /*inputs*/  {{"stamp"}, {"template"}},
                 /*outputs*/ {},
                 /*params*/ {
                     {"name",                  ParamType::String, "\"instance\""},
                     {"orient_to_normal",      ParamType::Bool,   "true"},
                     {"material_library_name", ParamType::String, "\"\""}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<InstanceVopSop>(uid);
                });
        }

        vops::VopGraph *instanceVopGraph(SopNode *node)
        {
            auto *h = dynamic_cast<InstanceVopSop *>(node);
            return h ? &h->vopGraph() : nullptr;
        }

        const vops::VopGraph *instanceVopGraph(const SopNode *node)
        {
            const auto *h = dynamic_cast<const InstanceVopSop *>(node);
            return h ? &h->vopGraph() : nullptr;
        }

        void setInstanceVopGraph(SopNode *node, std::unique_ptr<vops::VopGraph> graph)
        {
            if (auto *h = dynamic_cast<InstanceVopSop *>(node))
                h->setVopGraph(std::move(graph));
        }

        const std::vector<InstanceVopPromotion> *instanceVopPromotions(const SopNode *node)
        {
            const auto *h = dynamic_cast<const InstanceVopSop *>(node);
            return h ? &h->promotions() : nullptr;
        }

        std::string promoteInstanceVopParam(SopNode *node, size_t vopNodeUid,
                                            const std::string &vopParamName)
        {
            auto *h = dynamic_cast<InstanceVopSop *>(node);
            return h ? h->promote(vopNodeUid, vopParamName) : std::string{};
        }

        bool demoteInstanceVopParam(SopNode *node, const std::string &hostParamName)
        {
            auto *h = dynamic_cast<InstanceVopSop *>(node);
            return h && h->demote(hostParamName);
        }

        void syncPromotedInstanceVopValues(SopNode *node)
        {
            // Mirrors attribute_vop's syncPromotedHostValuesFromVop —
            // copies VOP-side param values back into the host SOP's
            // constant baseline so post-VOP-edit cooks see the new value.
            auto *h = dynamic_cast<InstanceVopSop *>(node);
            if (!h) return;
            const auto *graph = instanceVopGraph(node);
            if (!graph) return;
            const auto &proms = h->promotions();
            if (proms.empty()) return;
            auto &hostParams = h->parameters();
            for (const auto &p : proms)
            {
                vops::VopNode *vn = nullptr;
                for (const auto &n : graph->nodes())
                {
                    if (auto *c = dynamic_cast<vops::VopNode *>(n.get());
                        c && c->uid() == p.vopNodeUid) { vn = c; break; }
                }
                if (!vn) continue;
                Parameter *hp = nullptr;
                for (auto &q : hostParams)
                    if (q.name == p.hostParamName) { hp = &q; break; }
                if (!hp) continue;
                switch (p.paramType)
                {
                case ParamType::Float:  hp->value = vn->paramFloat (p.vopParamName, 0.0f);  break;
                case ParamType::Int:    hp->value = vn->paramInt   (p.vopParamName, 0);     break;
                case ParamType::Bool:   hp->value = vn->paramBool  (p.vopParamName, false); break;
                case ParamType::Vec3:   hp->value = vn->paramVec3  (p.vopParamName, Vec3(0.0f)); break;
                case ParamType::String: hp->value = vn->paramString(p.vopParamName, std::string{}); break;
                }
            }
        }
    }
}
