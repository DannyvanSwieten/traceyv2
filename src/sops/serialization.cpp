#include "serialization.hpp"
#include "sop_registry.hpp"

#include "json.hpp"  // nlohmann/json (bundled via deps/tinygltf)

#include <stdexcept>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            using json = nlohmann::json;

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
                return out;
            }

            void applyParamFromJson(SopNode &node, const std::string &name, const json &j)
            {
                if (!j.contains("type") || !j.contains("value")) return;
                const std::string t = j.at("type").get<std::string>();
                const auto &v = j.at("value");
                if (t == "float" && v.is_number())
                {
                    node.setParamFloat(name, v.get<float>());
                }
                else if (t == "int" && v.is_number_integer())
                {
                    node.setParamInt(name, v.get<int>());
                }
                else if (t == "bool" && v.is_boolean())
                {
                    node.setParamBool(name, v.get<bool>());
                }
                else if (t == "vec3" && v.is_array() && v.size() == 3)
                {
                    node.setParamVec3(name,
                        Vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>()));
                }
                else if (t == "string" && v.is_string())
                {
                    node.setParamString(name, v.get<std::string>());
                }
                // Else: silently ignored. Mismatched types between the saved
                // graph and the current node definition shouldn't crash; the
                // node's declared default stays in place.
            }

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
                return j;
            }

            std::string serialize_internal(const SopGraph &graph, bool pretty)
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

        std::unique_ptr<SopGraph> deserializeSopGraph(const std::string &jsonText)
        {
            json root = json::parse(jsonText, /*cb=*/nullptr, /*allow_exceptions=*/true);

            if (root.value("graph_kind", std::string{"sop"}) != "sop")
            {
                throw std::runtime_error("deserializeSopGraph: graph_kind != 'sop'");
            }
            const int version = root.value("version", 1);
            if (version != 1)
            {
                throw std::runtime_error("deserializeSopGraph: unsupported version "
                    + std::to_string(version));
            }

            auto graph = std::make_unique<SopGraph>(root.value<size_t>("uid", 0));

            if (root.contains("nodes") && root["nodes"].is_array())
            {
                for (const auto &nj : root["nodes"])
                {
                    const std::string kind = nj.at("kind").get<std::string>();
                    const size_t uid = nj.at("uid").get<size_t>();
                    auto node = SopRegistry::instance().create(kind, uid);
                    if (!node)
                    {
                        throw std::runtime_error("deserializeSopGraph: unknown node kind '" + kind + "'");
                    }

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

            // Seed the next-uid allocator after the highest already-loaded uid.
            const size_t nextUid = std::max<size_t>(
                root.value<size_t>("next_uid", 0),
                graph->maxNodeUid() + 1);
            graph->setNextUid(std::max<size_t>(nextUid, 1));

            return graph;
        }
    }
}
