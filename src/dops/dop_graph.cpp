#include "dop_graph.hpp"

#include "../graph/connection.hpp"

#include <algorithm>
#include <unordered_map>

namespace tracey
{
    namespace dops
    {
        DopGraph::DopGraph(size_t uid) : Graph(uid)
        {
            // Frame 0 is the implicit pre-sim baseline: empty geometry, no
            // header advance. Seeding the cache with it means cookOneFrame
            // can always assume a valid `prev` and the editor's
            // "cached_to_frame" status starts at 0 rather than -1.
            m_frameCache.emplace_back();
            m_frameCache.back().header.frame = 0;
        }

        DopNode *DopGraph::findNode(size_t uid)
        {
            for (const auto &n : nodes())
            {
                if (n->uid() == uid) return dynamic_cast<DopNode *>(n.get());
            }
            return nullptr;
        }
        const DopNode *DopGraph::findNode(size_t uid) const
        {
            for (const auto &n : nodes())
            {
                if (n->uid() == uid) return dynamic_cast<const DopNode *>(n.get());
            }
            return nullptr;
        }

        size_t DopGraph::nextUid() { return m_nextUid++; }
        size_t DopGraph::maxNodeUid() const
        {
            size_t m = 0;
            for (const auto &n : nodes()) m = std::max(m, n->uid());
            return m;
        }

        void DopGraph::markDirty()
        {
            m_dirty = true;
            clearCache();
        }

        void DopGraph::clearCache()
        {
            // Keep frame-0 (the empty baseline). Without it, the next
            // cookToFrame call would have nothing to read as `prev`.
            m_frameCache.resize(1);
        }

        int DopGraph::cachedToFrame() const
        {
            // m_frameCache[0] is the baseline (frame 0). The "newest cooked
            // frame" is the index of the last element when size > 1.
            return m_frameCache.empty() ? 0
                                        : static_cast<int>(m_frameCache.size() - 1);
        }

        const SimState *DopGraph::frame(int frameIdx) const
        {
            if (frameIdx < 0) return nullptr;
            if (static_cast<size_t>(frameIdx) >= m_frameCache.size()) return nullptr;
            return &m_frameCache[frameIdx];
        }

        // ── Topo sort (Kahn's; identical to VopGraph's) ────────────────────
        namespace
        {
            std::vector<size_t> topoSort(const DopGraph &g, bool *cycleOut)
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

        void DopGraph::compile() const
        {
            if (!m_dirty) return;
            bool cycle = false;
            m_topoOrder = topoSort(*this, &cycle);
            if (cycle) m_topoOrder.clear();
            m_dirty = false;
        }

        SimState DopGraph::cookOneFrame(const SimState &prev,
                                        int frameIdx,
                                        double fps,
                                        int substepsPerFrame) const
        {
            SimState next = prev;  // carry geometry forward as the starting point
            const int nsub = std::max(1, substepsPerFrame);
            const double dt = (fps > 0.0) ? (1.0 / fps) / static_cast<double>(nsub) : 0.0;

            // prepare() once per frame (not per substep) — nodes use it to
            // ensure point attributes exist. Cheap if they already do.
            for (size_t uid : m_topoOrder)
            {
                const auto *node = findNode(uid);
                if (!node || node->bypass()) continue;
                node->prepare(next);
            }

            for (int sub = 0; sub < nsub; ++sub)
            {
                next.header.frame = frameIdx;
                next.header.time = (fps > 0.0)
                                    ? (static_cast<double>(frameIdx - 1) / fps)
                                          + (static_cast<double>(sub) * dt)
                                    : 0.0;
                next.header.dt = dt;
                next.header.substepIdx = sub;
                next.header.substepsPerFrame = nsub;

                DopEvalContext ctx;
                ctx.state = &next;
                ctx.graph = this;
                ctx.sopProvider = m_sopProvider;
                for (size_t uid : m_topoOrder)
                {
                    const auto *node = findNode(uid);
                    if (!node || node->bypass()) continue;
                    node->cookFrame(ctx);
                }
            }
            // Pin the header to the end-of-frame state so consumers reading
            // frame N see substepIdx == nsub-1 (the last completed step).
            return next;
        }

        void DopGraph::cookToFrame(int target, double fps, int substepsPerFrame)
        {
            if (target <= 0) return;
            compile();
            if (m_topoOrder.empty())
            {
                // Empty graph or a cycle — leave the cache alone so consumers
                // get the empty baseline.
                return;
            }
            for (int f = static_cast<int>(m_frameCache.size()); f <= target; ++f)
            {
                const SimState &prev = m_frameCache.back();
                m_frameCache.push_back(cookOneFrame(prev, f, fps, substepsPerFrame));
            }
        }
    }
}
