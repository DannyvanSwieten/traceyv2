#include "serialization.hpp"
#include "sop_registry.hpp"

#include "json.hpp"  // nlohmann/json (bundled via deps/tinygltf)

#include <algorithm>
#include <stdexcept>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            using json = nlohmann::json;

            json channelToJson(const ScalarChannel &ch)
            {
                json out;
                json keys = json::array();
                for (const auto &k : ch.keys)
                {
                    keys.push_back({
                        {"t",   k.time},
                        {"v",   k.value},
                        {"in",  k.inTangent},
                        {"out", k.outTangent},
                        {"i",   interpName(k.interp)},
                    });
                }
                out["keys"] = std::move(keys);
                out["pre"]  = extrapName(ch.pre);
                out["post"] = extrapName(ch.post);
                return out;
            }

            ScalarChannel channelFromJson(const json &j)
            {
                ScalarChannel ch;
                if (j.contains("keys") && j["keys"].is_array())
                {
                    ch.keys.reserve(j["keys"].size());
                    for (const auto &kj : j["keys"])
                    {
                        ScalarChannel::Key k;
                        k.time       = kj.value("t",   0.0);
                        k.value      = kj.value("v",   0.0f);
                        k.inTangent  = kj.value("in",  0.0f);
                        k.outTangent = kj.value("out", 0.0f);
                        k.interp     = interpFromName(kj.value("i", std::string{"linear"}));
                        ch.keys.push_back(k);
                    }
                    std::sort(ch.keys.begin(), ch.keys.end(),
                        [](const ScalarChannel::Key &a, const ScalarChannel::Key &b) {
                            return a.time < b.time;
                        });
                }
                ch.pre  = extrapFromName(j.value("pre",  std::string{"hold"}));
                ch.post = extrapFromName(j.value("post", std::string{"hold"}));
                return ch;
            }

            // Emit `channels` as a fixed-length array (1 for scalar params, 3
            // for Vec3). Empty channels are emitted as null so the component
            // index is preserved across round-trips.
            json channelsToJson(const Parameter &p)
            {
                int slots = (p.type == ParamType::Vec3) ? 3 : 1;
                json out = json::array();
                for (int i = 0; i < slots; ++i)
                {
                    if (i < int(p.channels.size()) && !p.channels[i].keys.empty())
                        out.push_back(channelToJson(p.channels[i]));
                    else
                        out.push_back(nullptr);
                }
                return out;
            }

            json paramValueToJson(const Parameter &p)
            {
                json out;
                switch (p.type)
                {
                case ParamType::Float:
                    out["type"] = "float";
                    out["value"] = std::get<float>(p.value);
                    break;
                case ParamType::Int:
                    out["type"] = "int";
                    out["value"] = std::get<int>(p.value);
                    break;
                case ParamType::Bool:
                    out["type"] = "bool";
                    out["value"] = std::get<bool>(p.value);
                    break;
                case ParamType::Vec3:
                {
                    const Vec3 &v = std::get<Vec3>(p.value);
                    out["type"] = "vec3";
                    out["value"] = {v.x, v.y, v.z};
                    break;
                }
                case ParamType::String:
                    out["type"] = "string";
                    out["value"] = std::get<std::string>(p.value);
                    break;
                }
                if (p.isAnimated())
                {
                    out["channels"] = channelsToJson(p);
                }
                return out;
            }

            void applyParamFromJson(SopNode &node, const std::string &name, const json &j)
            {
                if (!j.contains("type") || !j.contains("value")) return;
                const std::string t = j.at("type").get<std::string>();
                const auto &v = j.at("value");
                bool ok = false;
                if (t == "float" && v.is_number())
                {
                    node.setParamFloat(name, v.get<float>());
                    ok = true;
                }
                else if (t == "int" && v.is_number_integer())
                {
                    node.setParamInt(name, v.get<int>());
                    ok = true;
                }
                else if (t == "bool" && v.is_boolean())
                {
                    node.setParamBool(name, v.get<bool>());
                    ok = true;
                }
                else if (t == "vec3" && v.is_array() && v.size() == 3)
                {
                    node.setParamVec3(name,
                        Vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>()));
                    ok = true;
                }
                else if (t == "string" && v.is_string())
                {
                    node.setParamString(name, v.get<std::string>());
                    ok = true;
                }
                // Else: silently ignored. Mismatched types between the saved
                // graph and the current node definition shouldn't crash; the
                // node's declared default stays in place.

                if (!ok) return;
                if (!j.contains("channels") || !j["channels"].is_array()) return;

                // Channels are optional. Find the parameter we just wrote to
                // and drop any restored channels onto it.
                Parameter *p = nullptr;
                for (auto &q : node.parameters())
                    if (q.name == name) { p = &q; break; }
                if (!p) return;

                const auto &chArr = j["channels"];
                p->channels.clear();
                p->channels.reserve(chArr.size());
                for (const auto &cj : chArr)
                {
                    if (cj.is_null()) p->channels.emplace_back();
                    else              p->channels.push_back(channelFromJson(cj));
                }
            }

            json graphToJson(const SopGraph &graph);  // forward decl for recursion

            json nodeToJson(const SopNode &node)
            {
                json j;
                j["uid"] = node.uid();
                j["kind"] = node.kind();
                j["pos"] = {node.posX(), node.posY()};

                json params = json::object();
                for (const auto &p : node.parameters())
                {
                    params[p.name] = paramValueToJson(p);
                }
                j["params"] = std::move(params);

                // Subnet (or any future SopNode that wraps a sub-graph) emits
                // its inner graph here. The recursion uses graphToJson so the
                // schema is uniform — the only field that distinguishes a
                // top-level graph from a nested one at load time is location
                // (root file vs. embedded under a node's `subgraph` key).
                if (const SopGraph *inner = node.innerGraph())
                {
                    j["subgraph"] = graphToJson(*inner);
                }

                // Generic extension hook for non-SopGraph child state, e.g.
                // AttributeVopSop's VopGraph. Stored as a parsed JSON value
                // under "extra" so the round-trip stays one nlohmann::json
                // tree (no nested escaped strings on the wire).
                std::string extra = node.serializeExtraJson();
                if (!extra.empty())
                {
                    try { j["extra"] = json::parse(extra); }
                    catch (...) { /* malformed override — drop silently */ }
                }
                return j;
            }

            json graphToJson(const SopGraph &graph)
            {
                json root;
                root["graph_kind"] = "sop";
                root["version"] = 1;
                root["uid"] = graph.uid();
                root["next_uid"] = graph.maxNodeUid() + 1;

                json nodes = json::array();
                for (const auto &n : graph.nodes())
                {
                    if (auto *sn = dynamic_cast<const SopNode *>(n.get()))
                    {
                        nodes.push_back(nodeToJson(*sn));
                    }
                }
                root["nodes"] = std::move(nodes);

                json conns = json::array();
                for (const auto &c : graph.connections())
                {
                    conns.push_back({
                        {"from_node", c.fromNode},
                        {"from_port", c.fromPort},
                        {"to_node",   c.toNode},
                        {"to_port",   c.toPort},
                    });
                }
                root["connections"] = std::move(conns);

                return root;
            }

            std::string serialize_internal(const SopGraph &graph, bool pretty)
            {
                json root = graphToJson(graph);
                return pretty ? root.dump(2) : root.dump();
            }
        }

        std::string serializeSopGraph(const SopGraph &graph)
        {
            return serialize_internal(graph, /*pretty=*/false);
        }
        std::string serializeSopGraphPretty(const SopGraph &graph)
        {
            return serialize_internal(graph, /*pretty=*/true);
        }

        namespace
        {
            // Build a SopGraph from an already-parsed JSON object. Used both
            // for the top-level entry point (deserializeSopGraph) and the
            // recursive subgraph case below — the schema is identical at both
            // levels so the same builder serves both.
            std::unique_ptr<SopGraph> buildGraphFromJson(const json &root)
            {
                if (root.value("graph_kind", std::string{"sop"}) != "sop")
                    throw std::runtime_error("deserializeSopGraph: graph_kind != 'sop'");
                const int version = root.value("version", 1);
                if (version != 1)
                    throw std::runtime_error("deserializeSopGraph: unsupported version "
                        + std::to_string(version));

                auto graph = std::make_unique<SopGraph>(root.value<size_t>("uid", 0));

                if (root.contains("nodes") && root["nodes"].is_array())
                {
                    for (const auto &nj : root["nodes"])
                    {
                        const std::string kind = nj.at("kind").get<std::string>();
                        const size_t uid = nj.at("uid").get<size_t>();
                        auto node = SopRegistry::instance().create(kind, uid);
                        if (!node)
                            throw std::runtime_error("deserializeSopGraph: unknown node kind '" + kind + "'");

                        if (nj.contains("pos") && nj["pos"].is_array() && nj["pos"].size() == 2)
                        {
                            node->setPos(nj["pos"][0].get<float>(), nj["pos"][1].get<float>());
                        }
                        if (nj.contains("params") && nj["params"].is_object())
                        {
                            for (auto it = nj["params"].begin(); it != nj["params"].end(); ++it)
                            {
                                applyParamFromJson(*node, it.key(), it.value());
                            }
                        }
                        // Recurse into nested subgraphs (subnet nodes carry an
                        // inner SopGraph). We attach the inner graph here but
                        // don't wire setRoot until the whole tree is built —
                        // the root pointer is the *outermost* SopGraph, which
                        // doesn't exist yet during this depth-first walk.
                        if (nj.contains("subgraph") && nj["subgraph"].is_object())
                        {
                            auto inner = buildGraphFromJson(nj["subgraph"]);
                            node->setInnerGraph(std::move(inner));
                        }
                        // Pass through the generic "extra" field for nodes
                        // that own non-SopGraph child state (e.g. VopGraph
                        // for AttributeVopSop).
                        if (nj.contains("extra"))
                        {
                            node->deserializeExtraJson(nj["extra"].dump());
                        }
                        graph->addNode(std::move(node));
                    }
                }

                if (root.contains("connections") && root["connections"].is_array())
                {
                    for (const auto &cj : root["connections"])
                    {
                        graph->createConnection(
                            cj.at("from_node").get<size_t>(),
                            cj.at("from_port").get<size_t>(),
                            cj.at("to_node").get<size_t>(),
                            cj.at("to_port").get<size_t>());
                    }
                }

                // Seed the (per-graph) nextUid. For inner subgraphs this gets
                // overridden when setRoot is wired below, but we still set it
                // so a detached subgraph (e.g. used standalone in tests) has
                // a sane allocator.
                const size_t nextUid = std::max<size_t>(
                    root.value<size_t>("next_uid", 0),
                    graph->maxNodeUid() + 1);
                graph->setNextUid(std::max<size_t>(nextUid, 1));

                return graph;
            }

            // Walk every nested SopGraph reachable from `root`, point each
            // inner graph's allocator at `root`, and roll the maximum uid
            // upward so the root's nextUid never collides with a uid already
            // present somewhere in the tree.
            void wireRootAndCollectMaxUid(SopGraph *root, SopGraph *current, size_t &maxUidOut)
            {
                if (!current) return;
                if (current != root) current->setRoot(root);
                for (const auto &n : current->nodes())
                {
                    if (auto *sn = dynamic_cast<SopNode *>(n.get()))
                    {
                        maxUidOut = std::max(maxUidOut, sn->uid());
                        if (auto *inner = sn->innerGraph())
                        {
                            wireRootAndCollectMaxUid(root, inner, maxUidOut);
                        }
                    }
                }
            }
        } // anon

        std::unique_ptr<SopGraph> deserializeSopGraph(const std::string &jsonText)
        {
            json root = json::parse(jsonText, /*cb=*/nullptr, /*allow_exceptions=*/true);
            auto graph = buildGraphFromJson(root);

            // After the full tree is materialised, point every inner graph's
            // nextUid() allocator at the outer root so uids stay globally
            // unique across nesting (matches the live cook-time invariant).
            size_t maxUid = 0;
            wireRootAndCollectMaxUid(graph.get(), graph.get(), maxUid);
            graph->setNextUid(std::max<size_t>(maxUid + 1, graph->maxNodeUid() + 1));

            return graph;
        }
    }
}
