#include "serialization.hpp"
#include "nodes.hpp"

#include "json.hpp"  // nlohmann/json (bundled via deps/tinygltf, private include)

#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace tracey
{
    namespace
    {
        using json = nlohmann::json;

        // ----- Op <-> string ---------------------------------------------------
        const char *opToString(Op op)
        {
            switch (op)
            {
            case Op::Halt:                return "Halt";
            case Op::LoadConst:           return "LoadConst";
            case Op::LoadPosition:        return "LoadPosition";
            case Op::LoadNormal:          return "LoadNormal";
            case Op::LoadTangent:         return "LoadTangent";
            case Op::LoadViewDir:         return "LoadViewDir";
            case Op::LoadUV0:             return "LoadUV0";
            case Op::LoadUV1:             return "LoadUV1";
            case Op::LoadInstanceIndex:   return "LoadInstanceIndex";
            case Op::LoadInputAlbedo:     return "LoadInputAlbedo";
            case Op::LoadInputMetallic:   return "LoadInputMetallic";
            case Op::LoadInputRoughness:  return "LoadInputRoughness";
            case Op::LoadInputEmission:   return "LoadInputEmission";
            case Op::LoadInputNormal:     return "LoadInputNormal";
            case Op::LoadInputTransmission: return "LoadInputTransmission";
            case Op::LoadInputIor:        return "LoadInputIor";
            case Op::LoadInputOpacity:    return "LoadInputOpacity";
            case Op::Add:                 return "Add";
            case Op::Sub:                 return "Sub";
            case Op::Mul:                 return "Mul";
            case Op::Div:                 return "Div";
            case Op::Neg:                 return "Neg";
            case Op::Saturate:            return "Saturate";
            case Op::Mix:                 return "Mix";
            case Op::Clamp:               return "Clamp";
            case Op::Dot3:                return "Dot3";
            case Op::Length3:             return "Length3";
            case Op::Cross:               return "Cross";
            case Op::Normalize3:          return "Normalize3";
            case Op::Splat:               return "Splat";
            case Op::WriteAlbedo:         return "WriteAlbedo";
            case Op::WriteMetallic:       return "WriteMetallic";
            case Op::WriteRoughness:      return "WriteRoughness";
            case Op::WriteEmission:       return "WriteEmission";
            case Op::WriteNormal:         return "WriteNormal";
            case Op::WriteAlpha:          return "WriteAlpha";
            case Op::WriteIor:            return "WriteIor";
            case Op::WriteTransmission:   return "WriteTransmission";
            case Op::LoadParam:           return "LoadParam";
            case Op::Count_:              break;  // not a real op
            }
            throw std::runtime_error("opToString: unknown opcode");
        }

        Op opFromString(std::string_view s)
        {
            // Lazy-built reverse map. Static const so the table is constructed once.
            static const auto table = []() {
                std::unordered_map<std::string, Op> m;
                for (uint16_t i = 0; i < static_cast<uint16_t>(Op::Count_); ++i)
                {
                    Op op = static_cast<Op>(i);
                    m.emplace(opToString(op), op);
                }
                return m;
            }();

            auto it = table.find(std::string(s));
            if (it == table.end())
            {
                throw std::runtime_error("opFromString: unknown op '" + std::string(s) + "'");
            }
            return it->second;
        }

        // ----- ShaderNodeKind <-> string --------------------------------------
        const char *kindToString(ShaderNodeKind k)
        {
            switch (k)
            {
            case ShaderNodeKind::Constant:       return "Constant";
            case ShaderNodeKind::Parameter:      return "Parameter";
            case ShaderNodeKind::MaterialInput:  return "MaterialInput";
            case ShaderNodeKind::MaterialOutput: return "MaterialOutput";
            case ShaderNodeKind::BinaryOp:       return "BinaryOp";
            case ShaderNodeKind::UnaryOp:        return "UnaryOp";
            case ShaderNodeKind::TernaryOp:      return "TernaryOp";
            }
            throw std::runtime_error("kindToString: unknown kind");
        }

        // ----- Node -> JSON ---------------------------------------------------
        json nodeToJson(const ShaderGraphNode &node)
        {
            json j;
            j["uid"] = node.uid();
            j["kind"] = kindToString(node.kind());
            switch (node.kind())
            {
            case ShaderNodeKind::Constant:
            {
                const auto &n = static_cast<const ConstantNode &>(node);
                const Vec4 &v = n.value();
                j["value"] = {v.x, v.y, v.z, v.w};
                break;
            }
            case ShaderNodeKind::Parameter:
            {
                const auto &n = static_cast<const ParameterNode &>(node);
                const Vec4 &d = n.defaultValue();
                j["name"] = n.paramName();
                j["default"] = {d.x, d.y, d.z, d.w};
                break;
            }
            case ShaderNodeKind::MaterialInput:
            case ShaderNodeKind::MaterialOutput:
                // Single-instance terminal nodes: no per-node opcode.
                // Their port order (in nodes.hpp) defines which opcode
                // gets emitted per port. Only `uid`, `kind`, optional
                // `position`, and optional `input_defaults` are stored.
                break;
            case ShaderNodeKind::BinaryOp:
                j["op"] = opToString(static_cast<const BinaryOpNode &>(node).opcode());
                break;
            case ShaderNodeKind::UnaryOp:
                j["op"] = opToString(static_cast<const UnaryOpNode &>(node).opcode());
                break;
            case ShaderNodeKind::TernaryOp:
                j["op"] = opToString(static_cast<const TernaryOpNode &>(node).opcode());
                break;
            }
            // Per-input default constants for unconnected inputs. Keyed
            // by port index (as a string, JSON-object convention) → vec4.
            // Only emitted when at least one default is set, so existing
            // graphs round-trip byte-identically.
            if (!node.inputDefaults().empty())
            {
                json defs = json::object();
                for (const auto &[port, v] : node.inputDefaults())
                {
                    defs[std::to_string(port)] = {v.x, v.y, v.z, v.w};
                }
                j["input_defaults"] = std::move(defs);
            }
            return j;
        }

        // Helper: read a vec4 array from JSON; throws on missing or malformed.
        Vec4 readVec4(const json &arr, const char *fieldName)
        {
            if (!arr.is_array() || arr.size() != 4)
            {
                throw std::runtime_error(std::string("expected 4-element array for field '") + fieldName + "'");
            }
            return Vec4(arr[0].get<float>(), arr[1].get<float>(),
                        arr[2].get<float>(), arr[3].get<float>());
        }

        // Helper: read a required field; throws if missing.
        const json &require(const json &j, const char *field)
        {
            auto it = j.find(field);
            if (it == j.end())
            {
                throw std::runtime_error(std::string("missing required field '") + field + "'");
            }
            return *it;
        }

        // ----- JSON -> Node ---------------------------------------------------
        std::unique_ptr<ShaderGraphNode> nodeFromJson(const json &j)
        {
            const size_t uid = require(j, "uid").get<size_t>();
            const std::string kind = require(j, "kind").get<std::string>();

            if (kind == "Constant")
            {
                return std::make_unique<ConstantNode>(uid, readVec4(require(j, "value"), "value"));
            }
            if (kind == "Parameter")
            {
                return std::make_unique<ParameterNode>(uid,
                                                       require(j, "name").get<std::string>(),
                                                       readVec4(require(j, "default"), "default"));
            }
            if (kind == "MaterialInput")  return std::make_unique<MaterialInputNode>(uid);
            if (kind == "MaterialOutput") return std::make_unique<MaterialOutputNode>(uid);

            const auto opStr = require(j, "op").get<std::string>();
            const Op op = opFromString(opStr);

            if (kind == "BinaryOp")         return std::make_unique<BinaryOpNode>(uid, op);
            if (kind == "UnaryOp")          return std::make_unique<UnaryOpNode>(uid, op);
            if (kind == "TernaryOp")        return std::make_unique<TernaryOpNode>(uid, op);

            throw std::runtime_error("deserializeShaderGraph: unknown kind '" + kind + "'");
        }

        // ----- Build the JSON document for a graph ----------------------------
        json graphToJson(const ShaderGraph &graph)
        {
            json doc;
            doc["version"] = 1;
            doc["uid"] = graph.uid();

            json nodesArr = json::array();
            for (const auto &nodePtr : graph.nodes())
            {
                const auto *sgn = dynamic_cast<const ShaderGraphNode *>(nodePtr.get());
                if (!sgn)
                {
                    throw std::runtime_error("serializeShaderGraph: graph contains non-ShaderGraphNode");
                }
                nodesArr.push_back(nodeToJson(*sgn));
            }
            doc["nodes"] = std::move(nodesArr);

            json connsArr = json::array();
            for (const auto &c : graph.connections())
            {
                json jc;
                jc["from_node"] = c.fromNode;
                jc["from_port"] = c.fromPort;
                jc["to_node"] = c.toNode;
                jc["to_port"] = c.toPort;
                connsArr.push_back(std::move(jc));
            }
            doc["connections"] = std::move(connsArr);

            return doc;
        }
    }

    std::string serializeShaderGraph(const ShaderGraph &graph)
    {
        return graphToJson(graph).dump();
    }

    std::string serializeShaderGraphPretty(const ShaderGraph &graph)
    {
        return graphToJson(graph).dump(2);
    }

    std::unique_ptr<ShaderGraph> deserializeShaderGraph(const std::string &jsonText)
    {
        json doc;
        try
        {
            doc = json::parse(jsonText);
        }
        catch (const json::parse_error &e)
        {
            throw std::runtime_error(std::string("deserializeShaderGraph: parse error: ") + e.what());
        }

        // Version check is forward-compatible: unknown versions are rejected
        // until we have an explicit migration path.
        if (auto it = doc.find("version"); it != doc.end())
        {
            const int v = it->get<int>();
            if (v != 1)
            {
                throw std::runtime_error("deserializeShaderGraph: unsupported version " + std::to_string(v));
            }
        }

        const size_t uid = doc.contains("uid") ? doc["uid"].get<size_t>() : 0;
        auto graph = std::make_unique<ShaderGraph>(uid);

        const auto &nodesArr = require(doc, "nodes");
        if (!nodesArr.is_array())
        {
            throw std::runtime_error("deserializeShaderGraph: 'nodes' must be an array");
        }
        for (const auto &nj : nodesArr)
        {
            auto node = nodeFromJson(nj);
            // Hydrate per-input defaults set via the inspector. Object
            // keys are port indices as strings; values are vec4 arrays.
            if (auto it = nj.find("input_defaults"); it != nj.end() && it->is_object())
            {
                for (auto kv = it->begin(); kv != it->end(); ++kv)
                {
                    const size_t port = std::stoull(kv.key());
                    node->setInputDefault(port, readVec4(kv.value(), "input_defaults"));
                }
            }
            graph->addNode(std::move(node));
        }

        const auto &connsArr = require(doc, "connections");
        if (!connsArr.is_array())
        {
            throw std::runtime_error("deserializeShaderGraph: 'connections' must be an array");
        }
        for (const auto &cj : connsArr)
        {
            graph->createConnection(require(cj, "from_node").get<size_t>(),
                                    require(cj, "from_port").get<size_t>(),
                                    require(cj, "to_node").get<size_t>(),
                                    require(cj, "to_port").get<size_t>());
        }

        return graph;
    }
}
