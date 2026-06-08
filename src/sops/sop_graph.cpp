#include "sop_graph.hpp"

#include "cook_cache.hpp"
#include "parameter.hpp"
#include "../graph/connection.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
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

        CookResult SopGraph::cook(CookDiagnostic *diag, double time,
                                  std::vector<NodeCookTiming> *timings)
        {
            // Legacy uncached entry point — used by smoke tests and any
            // headless path that constructs a SopGraph directly.
            return cook(diag, time, nullptr, timings);
        }

        CookResult SopGraph::cook(CookDiagnostic *diag, double time,
                                  CookCache *cache,
                                  std::vector<NodeCookTiming> *timings)
        {
            if (diag) *diag = {};
            // m_cache is only used by the uncached path. With a CookCache,
            // downstream nodes pull their inputs from CookCache::Entry::output
            // directly, so we don't write anything into m_cache.
            if (!cache) invalidate();

            bool cycle = false;
            auto order = topoSort(*this, &cycle);
            if (cycle)
            {
                if (diag) { diag->ok = false; diag->message = "graph contains a cycle"; }
                return {};
            }

            CookResult emitted;

            for (size_t uid : order)
            {
                auto *node = findNode(uid);
                if (!node) continue;
                const InputsAndOutputs ports = node->ports();
                std::vector<const Geometry *> inputs;
                inputs.reserve(ports.inputs().size());
                // Per-input src uid + port index + upstream cookId, used to
                // build this node's cache key. Upstream cookId stays stable
                // across cache hits, so a clean subtree produces a constant
                // key and the chain short-circuits all the way down.
                std::vector<std::tuple<size_t, uint32_t, uint64_t>> inputSrcs;
                inputSrcs.reserve(ports.inputs().size());
                bool anyUpstreamTimeDep = false;
                for (size_t i = 0; i < ports.inputs().size(); ++i)
                {
                    auto src = incomingTo(uid, i);
                    if (!src.has_value())
                    {
                        inputs.push_back(nullptr);
                        inputSrcs.emplace_back(0u, 0u, 0u);
                        continue;
                    }
                    if (cache)
                    {
                        auto *up = cache->find(src->first);
                        if (up && up->valid)
                        {
                            inputs.push_back(&up->output);
                            inputSrcs.emplace_back(src->first, src->second, up->cookId);
                            if (up->timeDependent) anyUpstreamTimeDep = true;
                        }
                        else
                        {
                            inputs.push_back(nullptr);
                            inputSrcs.emplace_back(src->first, src->second, 0u);
                        }
                    }
                    else
                    {
                        auto it = m_cache.find(src->first);
                        inputs.push_back(it == m_cache.end() ? nullptr : &it->second);
                        inputSrcs.emplace_back(src->first, src->second, 0u);
                    }
                }

                // Time-dependence: v1 conservative rule — `attribute_vop`
                // potentially reads @Time (VOPs can sample time inside their
                // generated kernel), and time-dependence propagates strictly
                // downstream. Other SOP cooks are pure functions of params +
                // inputs.
                const bool ownTimeDep = (node->kind() == "attribute_vop");
                const bool timeDep    = ownTimeDep || anyUpstreamTimeDep;

                // Compute this node's input key. Mixed with FNV-1a so the
                // composition stays explicit and predictable.
                uint64_t inputKey = 0;
                if (cache)
                {
                    constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
                    constexpr uint64_t kFnvPrime  = 0x00000100000001b3ULL;
                    auto mix = [&](const void *p, size_t n) {
                        const auto *b = static_cast<const unsigned char *>(p);
                        for (size_t i = 0; i < n; ++i)
                        {
                            inputKey ^= b[i];
                            inputKey *= kFnvPrime;
                        }
                    };
                    inputKey = kFnvOffset;
                    const std::string k = node->kind();
                    mix(k.data(), k.size());
                    const uint64_t ph = hashParameters(node->parameters());
                    mix(&ph, sizeof(ph));
                    // Mix in any non-Parameter state the node carries — most
                    // importantly the attribute_vop's inner VopGraph. Without
                    // this the cache would only see the host SOP's params
                    // (translate/rotate/scale) change and miss every VOP-side
                    // edit, so a freshly tweaked noise expression would
                    // silently reuse the previous Geometry.
                    const std::string extra = node->serializeExtraJson();
                    if (!extra.empty()) mix(extra.data(), extra.size());
                    for (const auto &[srcUid, srcPort, srcCookId] : inputSrcs)
                    {
                        mix(&srcUid, sizeof(srcUid));
                        mix(&srcPort, sizeof(srcPort));
                        mix(&srcCookId, sizeof(srcCookId));
                    }
                    if (timeDep) mix(&time, sizeof(time));
                }

                // Cache lookup: hit when the key matches what produced the
                // stored output. Miss → fall through to a fresh cook.
                CookCache::Entry *entry = cache ? &cache->upsert(uid) : nullptr;
                const bool cacheHit = entry && entry->valid && entry->inputKey == inputKey;

                const Geometry *outputPtr = nullptr;
                const auto tStart = std::chrono::steady_clock::now();
                if (cacheHit)
                {
                    // Refresh time-dependence in case the rule changed (e.g.
                    // upstream became time-dep but this node was previously
                    // cached as static). Then reuse the cached output.
                    entry->timeDependent = timeDep;
                    outputPtr = &entry->output;
                }
                else
                {
                    Geometry result;
                    // Bypass: skip the node's cook and forward the first
                    // input's geometry. If no input is connected (or the
                    // upstream cook produced nothing), the bypassed node
                    // contributes an empty Geometry. Matches Houdini's
                    // "flagged-bypassed SOP" semantics — wires stay, the
                    // transformation just stops applying.
                    if (node->bypass())
                    {
                        if (!inputs.empty() && inputs[0]) result = *inputs[0];
                    }
                    else
                    {
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
                    }
                    if (cache)
                    {
                        entry->cookId++;
                        entry->inputKey = inputKey;
                        entry->timeDependent = timeDep;
                        entry->valid = true;
                        entry->output = std::move(result);
                        outputPtr = &entry->output;
                    }
                    else
                    {
                        m_cache[uid] = std::move(result);
                        outputPtr = &m_cache[uid];
                    }
                }
                if (timings)
                {
                    const auto tEnd = std::chrono::steady_clock::now();
                    NodeCookTiming nct;
                    nct.nodeUid       = uid;
                    nct.parentNodeUid = 0;  // subnet recursion overwrites for inner nodes
                    nct.kind          = node->kind();
                    nct.name          = node->paramString("name", "");
                    nct.ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
                    timings->push_back(std::move(nct));
                }
                // Object_output / light terminals consume inputs[0] below;
                // outputPtr keeps the cooked Geometry alive for the rest of
                // this loop iteration without needing m_cache.
                (void)outputPtr;

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
                    if (!inputs.empty() && inputs[0])
                        a.geometry = std::make_shared<const Geometry>(*inputs[0]);
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
                else if (node->kind() == "instance")
                {
                    // Real GPU-instancing terminal: input 0 is the stamp
                    // (the geometry to clone), input 1 is the template
                    // point cloud. For each template point we emit ONE
                    // EmittedActor that carries the stamp's Geometry value
                    // unchanged + its own transform built from the template
                    // point's P, optional pscale, and optional N. apply_emitted's
                    // Phase-A content-hash dedup then collapses all N actors
                    // onto ONE SceneObject + BLAS, so the path tracer ends
                    // up with N TLAS instances pointing at one BVH instead
                    // of N flat-baked copies of the vertex data.
                    const Geometry *stamp = inputs.size() > 0 ? inputs[0] : nullptr;
                    const Geometry *tmpl  = inputs.size() > 1 ? inputs[1] : nullptr;
                    if (stamp && tmpl)
                    {
                        const auto &tplP  = tmpl->positions();
                        const auto *tplPs = tmpl->points().get<float>("pscale");
                        const auto *tplN  = tmpl->points().get<Vec3>("N");
                        const auto *tplCd = tmpl->points().get<Vec3>("Cd");
                        const bool useN   = node->paramBool("orient_to_normal", true);
                        const std::string baseName =
                            node->paramString("name", "instance_" + std::to_string(uid));

                        // Emit ONE EmittedActor with N per-instance entries.
                        // apply_emitted turns this into a single Scene Actor
                        // whose `instances()` list grows to N SceneInstances
                        // — each with its own per-instance TRS and tint. The
                        // win over the old "N EmittedActors" model is that
                        // the same Actor stays alive across cooks and
                        // particle birth/death is just an in-place resize +
                        // overwrite of the array, not 3000 Actor allocations.
                        EmittedActor a;
                        a.sourceNodeUid = uid;
                        a.instanceIndex = 0;
                        a.name          = baseName;
                        a.geometry      = std::make_shared<const Geometry>(*stamp);
                        a.materialLibraryName =
                            node->paramString("material_library_name", "");
                        a.instances.reserve(tplP.size());

                        for (size_t i = 0; i < tplP.size(); ++i)
                        {
                            EmittedActor::InstanceEntry e;
                            e.translate = tplP[i];
                            const float s = (tplPs && i < tplPs->data().size())
                                                ? tplPs->data()[i]
                                                : 1.0f;
                            e.scale = Vec3(s, s, s);
                            if (tplCd && i < tplCd->data().size())
                            {
                                e.tint = tplCd->data()[i];
                                e.hasTint = true;
                            }
                            if (useN && tplN && i < tplN->data().size())
                            {
                                // Build a rotation that maps stamp's +Z to N,
                                // with +Y as the up reference. Mirrors the
                                // copy_to_points helper.
                                const Vec3 &n = tplN->data()[i];
                                glm::vec3 forward(n.x, n.y, n.z);
                                const float fl2 = glm::dot(forward, forward);
                                if (fl2 >= 1e-12f)
                                {
                                    forward = forward * (1.0f / std::sqrt(fl2));
                                    glm::vec3 right = glm::cross(glm::vec3(0, 1, 0), forward);
                                    const float rl2 = glm::dot(right, right);
                                    if (rl2 >= 1e-12f)
                                    {
                                        right = right * (1.0f / std::sqrt(rl2));
                                        glm::vec3 up = glm::cross(forward, right);
                                        glm::mat3 R(right, up, forward);
                                        glm::quat q = glm::quat_cast(R);
                                        e.rotation = Vec4(q.w, q.x, q.y, q.z);
                                    }
                                }
                            }
                            a.instances.push_back(e);
                        }
                        emitted.push_back(std::move(a));
                    }
                }
                // Output is already stored — either in cache->upsert(uid)->output
                // for the cached path, or in m_cache[uid] for the legacy path.
                // No additional m_cache writes here.
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
                    // Collect inner timings into a local vec; we stamp the
                    // parent uid on each before merging into the outer list
                    // so the profiler can group rows by subnet without
                    // re-walking the graph.
                    std::vector<NodeCookTiming> innerTimings;
                    // Recurse with the same cache — uids are globally unique
                    // (root allocator) so a single CookCache covers every
                    // node in the subnet tree.
                    auto innerEmitted = inner->cook(
                        &innerDiag, time, cache,
                        timings ? &innerTimings : nullptr);
                    if (!innerDiag.ok)
                    {
                        if (diag) { diag->ok = false; diag->nodeUid = uid; diag->message = innerDiag.message; }
                        return {};
                    }
                    if (timings)
                    {
                        for (auto &it : innerTimings)
                        {
                            if (it.parentNodeUid == 0) it.parentNodeUid = uid;
                            timings->push_back(std::move(it));
                        }
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
