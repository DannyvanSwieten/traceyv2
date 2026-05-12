#include "sop_graph.hpp"

#include "../graph/connection.hpp"

#include <glm/gtc/quaternion.hpp>

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
            // Inner sub-graphs forward to the root allocator so uids stay
            // globally unique across nesting — the keyframe IPC identifies
            // nodes by uid only.
            if (m_root) return m_root->nextUid();
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

        namespace
        {
            // Compose an euler-deg vec3 (Houdini convention: ZYX intrinsic
            // applied in the order Rx then Ry then Rz, which translates to
            // q = qz * qy * qx in glm). Stored on the EmittedActor as a
            // wxyz Vec4 so the cook output stays POD-friendly. Mirrors the
            // identical conversion in nodes/transform_sop.cpp.
            inline Vec4 eulerDegToQuatWxyz(const Vec3 &deg)
            {
                constexpr float kDeg2Rad = 3.1415926535f / 180.0f;
                const Vec3 rad = deg * kDeg2Rad;
                glm::quat qx = glm::angleAxis(rad.x, glm::vec3(1, 0, 0));
                glm::quat qy = glm::angleAxis(rad.y, glm::vec3(0, 1, 0));
                glm::quat qz = glm::angleAxis(rad.z, glm::vec3(0, 0, 1));
                glm::quat q  = qz * qy * qx;
                return Vec4(q.w, q.x, q.y, q.z);
            }
        } // anon

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

        CookResult SopGraph::cook(CookDiagnostic *diag, double time)
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
                    result = node->cookAt(
                        std::span<const Geometry *const>{inputs.data(), inputs.size()},
                        time);
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
                    a.rotation  = eulerDegToQuatWxyz(
                        node->paramVec3("rotate_euler_deg", Vec3(0.0f)));
                    a.scale = node->paramVec3("scale", Vec3(1.0f));
                    a.materialLibraryName = node->paramString("material_library_name", "");
                    if (!inputs.empty() && inputs[0]) a.geometry = *inputs[0];
                    emitted.push_back(std::move(a));
                }
                else if (node->kind() == "light")
                {
                    // Houdini-style /obj light terminal — emits a
                    // transform-only actor with a Light payload. apply_emitted
                    // attaches the component; the SceneCompiler picks it up
                    // into its light list later. No geometry, no instance.
                    EmittedActor a;
                    a.sourceNodeUid = uid;
                    a.isLight = true;
                    a.name = node->paramString("name", "light_" + std::to_string(uid));
                    a.translate = node->paramVec3("translate", Vec3(0.0f));
                    a.rotation  = eulerDegToQuatWxyz(
                        node->paramVec3("rotate_euler_deg", Vec3(0.0f)));
                    a.scale = node->paramVec3("scale", Vec3(1.0f));
                    a.lightType = node->paramInt("type", 0);
                    a.lightColor = node->paramVec3("color", Vec3(1.0f));
                    a.lightIntensity = node->paramFloat("intensity", 1.0f);
                    emitted.push_back(std::move(a));
                }

                m_cache[uid] = std::move(result);
            }

            // ── Subnet pass ────────────────────────────────────────────────
            // After the main topo cook, walk subnet nodes in deterministic
            // (sorted-by-uid) order. For each subnet:
            //   1. Push a marker EmittedActor (transform-only parent — the
            //      editor host creates a live Actor with no SceneInstance).
            //   2. Recursively cook the inner graph and append its emits,
            //      stamping ea.parentNodeUid = subnet->uid() — but only if
            //      it's still 0, so nested subnets keep their innermost
            //      parent (the inner cook already stamped it).
            //
            // The marker actor lands BEFORE its children, which apply_emitted
            // relies on so addChild can find the parent in m_sop_node_to_actor.
            std::vector<size_t> subnetUids;
            for (const auto &n : nodes())
            {
                if (auto *sn = dynamic_cast<SopNode *>(n.get()); sn && sn->kind() == "subnet")
                {
                    subnetUids.push_back(sn->uid());
                }
            }
            std::sort(subnetUids.begin(), subnetUids.end());

            for (size_t uid : subnetUids)
            {
                auto *node = findNode(uid);
                if (!node) continue;

                EmittedActor marker;
                marker.sourceNodeUid = uid;
                marker.isSubnetMarker = true;
                marker.name = node->paramString("name", "subnet_" + std::to_string(uid));
                marker.translate = node->paramVec3("translate", Vec3(0.0f));
                marker.rotation  = eulerDegToQuatWxyz(
                    node->paramVec3("rotate_euler_deg", Vec3(0.0f)));
                marker.scale = node->paramVec3("scale", Vec3(1.0f));
                emitted.push_back(std::move(marker));

                if (auto *inner = node->innerGraph())
                {
                    CookDiagnostic innerDiag;
                    auto innerEmitted = inner->cook(&innerDiag, time);
                    if (!innerDiag.ok)
                    {
                        if (diag) { diag->ok = false; diag->nodeUid = uid; diag->message = innerDiag.message; }
                        return {};
                    }
                    for (auto &child : innerEmitted)
                    {
                        if (child.parentNodeUid == 0) child.parentNodeUid = uid;
                        emitted.push_back(std::move(child));
                    }
                }
            }

            return emitted;
        }
    }
}
