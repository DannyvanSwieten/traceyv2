#include "vop_graph.hpp"

#include "../graph/connection.hpp"

#include <algorithm>
#include <climits>
#include <limits>

namespace tracey
{
    namespace vops
    {
        VopGraph::VopGraph(size_t uid) : Graph(uid) {}

        VopNode *VopGraph::findNode(size_t uid)
        {
            for (const auto &n : nodes())
            {
                if (n->uid() == uid) return dynamic_cast<VopNode *>(n.get());
            }
            return nullptr;
        }

        const VopNode *VopGraph::findNode(size_t uid) const
        {
            for (const auto &n : nodes())
            {
                if (n->uid() == uid) return dynamic_cast<const VopNode *>(n.get());
            }
            return nullptr;
        }

        std::optional<std::pair<size_t, size_t>>
        VopGraph::incomingTo(size_t nodeUid, size_t inputPortIdx) const
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

        size_t VopGraph::nextUid() { return m_nextUid++; }

        size_t VopGraph::maxNodeUid() const
        {
            size_t m = 0;
            for (const auto &n : nodes()) m = std::max(m, n->uid());
            return m;
        }

        size_t VopGraph::slotIndex(size_t nodeUid, size_t outputPort) const
        {
            auto it = m_slotIndex.find(makeSlotKey(nodeUid, outputPort));
            if (it == m_slotIndex.end()) return std::numeric_limits<size_t>::max();
            return it->second;
        }

        // ── Topo sort (Kahn's, mirrors src/sops/sop_graph.cpp) ──────────────
        namespace
        {
            std::vector<size_t> topoSort(const VopGraph &g, bool *cycleOut)
            {
                std::unordered_map<size_t, int> inDeg;
                std::unordered_map<size_t, std::vector<size_t>> outEdges;
                inDeg.reserve(g.nodes().size());

                for (const auto &n : g.nodes()) inDeg[n->uid()] = 0;

                for (const auto &c : g.connections())
                {
                    if (!inDeg.contains(c.fromNode) || !inDeg.contains(c.toNode)) continue;
                    ++inDeg[c.toNode];
                    outEdges[c.fromNode].push_back(c.toNode);
                }

                std::vector<size_t> ready;
                for (const auto &[uid, d] : inDeg) if (d == 0) ready.push_back(uid);
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
                    std::sort(outs.begin(), outs.end());
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
        }

        void VopGraph::compile() const
        {
            if (!m_dirty) return;

            bool cycle = false;
            m_topoOrder = topoSort(*this, &cycle);
            if (cycle)
            {
                // Cycle: leave caches empty so evaluatePoint becomes a no-op.
                m_topoOrder.clear();
                m_slotCount = 0;
                m_slotIndex.clear();
                m_dirty = false;
                return;
            }

            // Allocate one slot per (node, outputPort).
            m_slotIndex.clear();
            size_t cursor = 0;
            for (size_t uid : m_topoOrder)
            {
                const auto *n = findNode(uid);
                if (!n) continue;
                const auto outs = n->ports().outputs();
                for (size_t p = 0; p < outs.size(); ++p)
                {
                    m_slotIndex[makeSlotKey(uid, p)] = cursor++;
                }
            }
            m_slotCount = cursor;

            // Type-inference pass piggybacks on the same compile()
            // trigger so callers don't have to remember to invoke it
            // separately. Both the CPU evaluator and the GLSL emitter
            // read from m_types — see typing.hpp for the rationale.
            m_types = inferGraphTypes(*this);

            m_dirty = false;
        }

        TypeKind VopGraph::portType(size_t nodeUid, size_t port, bool isOutput) const
        {
            compile();  // ensure m_types is current
            return m_types.portType(nodeUid, static_cast<uint32_t>(port), isOutput);
        }

        void VopGraph::evaluatePoint(size_t pointIdx, Geometry &geo) const
        {
            std::vector<Value> slots;
            evaluatePoint(pointIdx, geo, slots);
        }

        void VopGraph::evaluatePoint(size_t pointIdx, Geometry &geo,
                                     std::vector<Value> &slots) const
        {
            compile();
            if (m_topoOrder.empty()) return;

            slots.assign(m_slotCount, Value{});
            EvalContext ctx;
            ctx.pointIndex = pointIdx;
            ctx.geometry = &geo;
            ctx.slots = &slots;
            ctx.graph = this;

            for (size_t uid : m_topoOrder)
            {
                const auto *node = findNode(uid);
                if (!node) continue;
                node->evaluate(ctx);
            }
        }

        std::optional<Value> VopGraph::readInput(const EvalContext &ctx,
                                                 size_t nodeUid,
                                                 size_t inputPortIdx) const
        {
            auto src = incomingTo(nodeUid, inputPortIdx);
            if (src)
            {
                const size_t slot = slotIndex(src->first, src->second);
                if (slot == std::numeric_limits<size_t>::max() || !ctx.slots) return std::nullopt;
                if (slot >= ctx.slots->size()) return std::nullopt;
                return (*ctx.slots)[slot];
            }
            // No wire — fall back to the node's per-port stored constant.
            // Keeps the value_or(Value{0.0f}) safety net in nodes happy when
            // the user hasn't set a default either; we just return nullopt
            // and let the node decide what to do.
            if (const auto *node = findNode(nodeUid))
            {
                return node->inputDefault(inputPortIdx);
            }
            return std::nullopt;
        }

        void VopGraph::writeOutput(EvalContext &ctx, size_t nodeUid,
                                   size_t outputPortIdx, Value v) const
        {
            const size_t slot = slotIndex(nodeUid, outputPortIdx);
            if (slot == std::numeric_limits<size_t>::max() || !ctx.slots) return;
            if (slot >= ctx.slots->size()) return;
            (*ctx.slots)[slot] = v;
        }
    }
}
