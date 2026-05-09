#include "sop_graph.hpp"

#include "../graph/connection.hpp"

#include <algorithm>
#include <unordered_set>

namespace tracey
{
    namespace sops
    {
        SopGraph::SopGraph(size_t uid) : Graph(uid) {}

        SopNode *SopGraph::findNode(size_t uid)
        {
            for (const auto &n : nodes())
            {
                if (n->uid() == uid) return dynamic_cast<SopNode *>(n.get());
            }
            return nullptr;
        }

        const SopNode *SopGraph::findNode(size_t uid) const
        {
            for (const auto &n : nodes())
            {
                if (n->uid() == uid) return dynamic_cast<const SopNode *>(n.get());
            }
            return nullptr;
        }

        std::optional<std::pair<size_t, size_t>>
        SopGraph::incomingTo(size_t nodeUid, size_t inputPortIdx) const
        {
            for (const auto &c : connections())
            {
                if (c.toNode == nodeUid && c.toPort == inputPortIdx)
                {
                    return std::make_pair(c.fromNode, c.fromPort);
                }
            }
            return std::nullopt;
        }

        size_t SopGraph::nextUid()
        {
            return m_nextUid++;
        }

        size_t SopGraph::maxNodeUid() const
        {
            size_t m = 0;
            for (const auto &n : nodes()) m = std::max(m, n->uid());
            return m;
        }

        void SopGraph::invalidate()
        {
            m_cache.clear();
        }

        // ── Topological sort ───────────────────────────────────────────────
        namespace
        {
            // Kahn's algorithm. Returns ordered uids, or empty vector on cycle.
            std::vector<size_t> topoSort(const SopGraph &g, bool *cycleOut)
            {
                // Map uid → in-degree, plus an out-edge list so we can decrement
                // downstream in-degrees as nodes leave the queue.
                std::unordered_map<size_t, int> inDeg;
                std::unordered_map<size_t, std::vector<size_t>> outEdges;
                inDeg.reserve(g.nodes().size());

                for (const auto &n : g.nodes()) inDeg[n->uid()] = 0;

                for (const auto &c : g.connections())
                {
                    // Only count edges where both endpoints exist as nodes in
                    // this graph (defensive; bad data shouldn't crash cook).
                    if (!inDeg.contains(c.fromNode) || !inDeg.contains(c.toNode)) continue;
                    ++inDeg[c.toNode];
                    outEdges[c.fromNode].push_back(c.toNode);
                }

                std::vector<size_t> ready;
                for (const auto &[uid, d] : inDeg) if (d == 0) ready.push_back(uid);
                // Process in stable order so identical graphs cook identically.
                std::sort(ready.begin(), ready.end());

                std::vector<size_t> order;
                order.reserve(g.nodes().size());

                while (!ready.empty())
                {
                    size_t u = ready.back();
                    ready.pop_back();
                    order.push_back(u);

                    auto it = outEdges.find(u);
                    if (it == outEdges.end()) continue;
                    auto &outs = it->second;
                    std::sort(outs.begin(), outs.end()); // stable
                    for (size_t v : outs)
                    {
                        if (--inDeg[v] == 0) ready.push_back(v);
                    }
                }

                if (order.size() != g.nodes().size())
                {
                    if (cycleOut) *cycleOut = true;
                    return {};
                }
                if (cycleOut) *cycleOut = false;
                return order;
            }
        } // anon

        CookResult SopGraph::cook(CookDiagnostic *diag)
        {
            if (diag) *diag = {};
            invalidate();

            bool cycle = false;
            auto order = topoSort(*this, &cycle);
            if (cycle)
            {
                if (diag) { diag->ok = false; diag->message = "graph contains a cycle"; }
                return {};
            }

            CookResult emitted;

            // Pre-build vector<Geometry*> per node so cook() can take a span
            // by const&. Lifetimes: each entry points into m_cache, which is
            // stable for the duration of this cook() call.
            for (size_t uid : order)
            {
                auto *node = findNode(uid);
                if (!node) continue;
                const InputsAndOutputs ports = node->ports();
                std::vector<const Geometry *> inputs;
                inputs.reserve(ports.inputs().size());
                for (size_t i = 0; i < ports.inputs().size(); ++i)
                {
                    auto src = incomingTo(uid, i);
                    if (!src.has_value()) { inputs.push_back(nullptr); continue; }
                    auto it = m_cache.find(src->first);
                    inputs.push_back(it == m_cache.end() ? nullptr : &it->second);
                }

                Geometry result;
                try
                {
                    result = node->cook(std::span<const Geometry *const>{inputs.data(), inputs.size()});
                }
                catch (const std::exception &e)
                {
                    if (diag) { diag->ok = false; diag->nodeUid = uid; diag->message = e.what(); }
                    return {};
                }

                // Terminal nodes (object_output) are detected by kind() rather
                // than by port shape so we can be explicit about which nodes
                // contribute to the emitted-actor list. The terminal's cook()
                // is responsible for stashing the EmittedActor in its
                // result via a side channel — but we want pure cooks. So:
                // ObjectOutput.cook() returns the input geometry unchanged,
                // and we synthesize the EmittedActor here by reading its
                // parameters + first input directly.
                if (node->kind() == "object_output")
                {
                    EmittedActor a;
                    a.sourceNodeUid = uid;
                    a.name = node->paramString("name", "actor_" + std::to_string(uid));
                    a.translate = node->paramVec3("translate", Vec3(0.0f));
                    Vec3 rotEuler = node->paramVec3("rotate_euler_deg", Vec3(0.0f));
                    // Treat rotation as identity unless someone wires a
                    // proper quaternion path later. For now we expose euler
                    // params and ignore them at emission time so transforms
                    // remain Vec3-only. (Plan note: will revisit when a
                    // dedicated TransformParam type lands.)
                    (void)rotEuler;
                    a.scale = node->paramVec3("scale", Vec3(1.0f));
                    a.materialLibraryName = node->paramString("material_library_name", "");
                    if (!inputs.empty() && inputs[0]) a.geometry = *inputs[0];
                    emitted.push_back(std::move(a));
                }

                m_cache[uid] = std::move(result);
            }

            return emitted;
        }
    }
}
