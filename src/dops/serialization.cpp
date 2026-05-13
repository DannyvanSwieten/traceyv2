#include "serialization.hpp"
#include "dop_registry.hpp"

#include "json.hpp" // nlohmann/json bundled via deps/tinygltf

#include <algorithm>
#include <stdexcept>

// Mirrors src/vops/serialization.cpp. Same schema, same parameter encoding,
// same connection encoding — only the registry queried at deserialize time
// and the `graph_kind` literal differ. Keep the three serialization files
// (sops/, vops/, dops/) in sync.

namespace tracey
{
    namespace dops
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
                        {"i",   tracey::sops::interpName(k.interp)},
                    });
                }
                out["keys"] = std::move(keys);
                out["pre"]  = tracey::sops::extrapName(ch.pre);
                out["post"] = tracey::sops::extrapName(ch.post);
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
                        k.interp     = tracey::sops::interpFromName(
                            kj.value("i", std::string{"linear"}));
                        ch.keys.push_back(k);
                    }
                    std::sort(ch.keys.begin(), ch.keys.end(),
                        [](const ScalarChannel::Key &a, const ScalarChannel::Key &b) {
                            return a.time < b.time;
                        });
                }
                ch.pre  = tracey::sops::extrapFromName(j.value("pre",  std::string{"hold"}));
                ch.post = tracey::sops::extrapFromName(j.value("post", std::string{"hold"}));
                return ch;
            }

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

            void applyParamFromJson(DopNode &node, const std::string &name, const json &j)
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

                if (!ok) return;
                if (!j.contains("channels") || !j["channels"].is_array()) return;

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

            json nodeToJson(const DopNode &node)
            {
                json j;
                j["uid"]  = node.uid();
                j["kind"] = node.kind();
                j["pos"]  = {node.posX(), node.posY()};

                json params = json::object();
                for (const auto &p : node.parameters())
                {
                    params[p.name] = paramValueToJson(p);
                }
                j["params"] = std::move(params);

                // Per-node extension hook — pop_force uses this to round-
                // trip its embedded VopGraph + promotion list. Empty
                // string means "nothing extra" (most nodes).
                const std::string extra = node.serializeExtraJson();
                if (!extra.empty())
                {
                    j["extra"] = json::parse(extra);
                }
                return j;
            }

            json graphToJson(const DopGraph &graph)
            {
                json root;
                root["graph_kind"] = "dop";
                root["version"] = 1;
                root["uid"] = graph.uid();
                root["next_uid"] = graph.maxNodeUid() + 1;

                json nodes = json::array();
                for (const auto &n : graph.nodes())
                {
                    if (auto *dn = dynamic_cast<const DopNode *>(n.get()))
                    {
                        nodes.push_back(nodeToJson(*dn));
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

            std::string serialize_internal(const DopGraph &graph, bool pretty)
            {
                json root = graphToJson(graph);
                return pretty ? root.dump(2) : root.dump();
            }
        }

        std::string serializeDopGraph(const DopGraph &graph)
        {
            return serialize_internal(graph, /*pretty=*/false);
        }
        std::string serializeDopGraphPretty(const DopGraph &graph)
        {
            return serialize_internal(graph, /*pretty=*/true);
        }

        std::unique_ptr<DopGraph> deserializeDopGraph(const std::string &jsonText)
        {
            json root = json::parse(jsonText, /*cb=*/nullptr, /*allow_exceptions=*/true);
            if (root.value("graph_kind", std::string{"dop"}) != "dop")
                throw std::runtime_error("deserializeDopGraph: graph_kind != 'dop'");
            const int version = root.value("version", 1);
            if (version != 1)
                throw std::runtime_error("deserializeDopGraph: unsupported version "
                    + std::to_string(version));

            auto graph = std::make_unique<DopGraph>(root.value<size_t>("uid", 0));

            if (root.contains("nodes") && root["nodes"].is_array())
            {
                for (const auto &nj : root["nodes"])
                {
                    const std::string kind = nj.at("kind").get<std::string>();
                    const size_t uid = nj.at("uid").get<size_t>();
                    auto node = DopRegistry::instance().create(kind, uid);
                    if (!node)
                        throw std::runtime_error(
                            "deserializeDopGraph: unknown node kind '" + kind + "'");

                    if (nj.contains("pos") && nj["pos"].is_array() && nj["pos"].size() == 2)
                    {
                        node->setPos(nj["pos"][0].get<float>(), nj["pos"][1].get<float>());
                    }
                    // `extra` ahead of `params` so that subclasses which
                    // declare host params via deserializeExtraJson (mirror
                    // of attribute_vop_sop's promotion handling) have the
                    // param slots in place before the params block fills
                    // them with the saved values.
                    if (nj.contains("extra"))
                    {
                        node->deserializeExtraJson(nj["extra"].dump());
                    }
                    if (nj.contains("params") && nj["params"].is_object())
                    {
                        for (auto it = nj["params"].begin(); it != nj["params"].end(); ++it)
                        {
                            applyParamFromJson(*node, it.key(), it.value());
                        }
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

            const size_t nextUid = std::max<size_t>(
                root.value<size_t>("next_uid", 0),
                graph->maxNodeUid() + 1);
            graph->setNextUid(std::max<size_t>(nextUid, 1));
            graph->markDirty();
            return graph;
        }
    }
}
