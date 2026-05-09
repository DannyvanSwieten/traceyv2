#include "compiler.hpp"
#include "nodes.hpp"

#include <map>
#include <queue>
#include <stdexcept>
#include <string>

namespace tracey
{
    namespace
    {
        // Topologically order the graph's ShaderGraphNodes (producers before
        // consumers). Throws on cycle or non-ShaderGraphNode entries.
        std::vector<const ShaderGraphNode *> topologicalOrder(const ShaderGraph &graph)
        {
            std::map<size_t, const ShaderGraphNode *> byUid;
            std::map<size_t, int> inDegree;

            for (const auto &nodePtr : graph.nodes())
            {
                const auto *sgn = dynamic_cast<const ShaderGraphNode *>(nodePtr.get());
                if (!sgn)
                {
                    throw std::runtime_error("compileShaderGraph: graph contains non-ShaderGraphNode");
                }
                byUid[sgn->uid()] = sgn;
                inDegree[sgn->uid()] = 0;
            }

            // Each connection contributes one to the destination's in-degree.
            for (const auto &c : graph.connections())
            {
                auto it = inDegree.find(c.toNode);
                if (it != inDegree.end())
                {
                    it->second += 1;
                }
            }

            std::queue<size_t> ready;
            for (const auto &[uid, deg] : inDegree)
            {
                if (deg == 0) ready.push(uid);
            }

            std::vector<const ShaderGraphNode *> order;
            order.reserve(byUid.size());
            while (!ready.empty())
            {
                const size_t uid = ready.front();
                ready.pop();
                order.push_back(byUid[uid]);

                for (const auto &c : graph.connections())
                {
                    if (c.fromNode == uid)
                    {
                        auto it = inDegree.find(c.toNode);
                        if (it != inDegree.end() && --it->second == 0)
                        {
                            ready.push(c.toNode);
                        }
                    }
                }
            }

            if (order.size() != byUid.size())
            {
                throw std::runtime_error("compileShaderGraph: graph contains a cycle");
            }
            return order;
        }
    }

    MaterialProgram compileShaderGraph(const ShaderGraph &graph)
    {
        const auto order = topologicalOrder(graph);

        MaterialProgramBuilder builder;
        // (nodeUid, outputPortIdx) -> register holding that port's value
        std::map<std::pair<size_t, size_t>, uint16_t> outReg;
        std::vector<Vec4> paramDefaults;

        auto sourceRegFor = [&](size_t consumerUid, size_t inputPort) -> uint16_t {
            auto inc = graph.incomingTo(consumerUid, inputPort);
            if (!inc)
            {
                throw std::runtime_error(
                    "compileShaderGraph: node " + std::to_string(consumerUid) +
                    " has unconnected input port " + std::to_string(inputPort));
            }
            auto it = outReg.find(*inc);
            if (it == outReg.end())
            {
                throw std::runtime_error(
                    "compileShaderGraph: unresolved source for node " +
                    std::to_string(consumerUid));
            }
            return it->second;
        };

        for (const auto *node : order)
        {
            switch (node->kind())
            {
            case ShaderNodeKind::Constant:
            {
                const auto *c = static_cast<const ConstantNode *>(node);
                uint16_t r = builder.loadConst(c->value());
                outReg[{node->uid(), 0}] = r;
                break;
            }
            case ShaderNodeKind::Parameter:
            {
                const auto *p = static_cast<const ParameterNode *>(node);
                uint16_t paramIdx = builder.allocParam();
                paramDefaults.push_back(p->defaultValue());
                uint16_t r = builder.loadParam(paramIdx);
                outReg[{node->uid(), 0}] = r;
                break;
            }
            case ShaderNodeKind::SurfaceAttribute:
            {
                const auto *s = static_cast<const SurfaceAttributeNode *>(node);
                uint16_t r = builder.loadSurface(s->opcode());
                outReg[{node->uid(), 0}] = r;
                break;
            }
            case ShaderNodeKind::InputAttribute:
            {
                const auto *i = static_cast<const InputAttributeNode *>(node);
                // loadSurface is just "alloc reg + emit op with no operands";
                // the LoadInput* opcodes share that calling convention.
                uint16_t r = builder.loadSurface(i->opcode());
                outReg[{node->uid(), 0}] = r;
                break;
            }
            case ShaderNodeKind::BinaryOp:
            {
                const auto *b = static_cast<const BinaryOpNode *>(node);
                uint16_t a = sourceRegFor(node->uid(), 0);
                uint16_t br = sourceRegFor(node->uid(), 1);
                uint16_t dst = builder.allocReg();
                builder.emit(b->opcode(), dst, a, br);
                outReg[{node->uid(), 0}] = dst;
                break;
            }
            case ShaderNodeKind::UnaryOp:
            {
                const auto *u = static_cast<const UnaryOpNode *>(node);
                uint16_t a = sourceRegFor(node->uid(), 0);
                uint16_t dst = builder.allocReg();
                builder.emit(u->opcode(), dst, a);
                outReg[{node->uid(), 0}] = dst;
                break;
            }
            case ShaderNodeKind::TernaryOp:
            {
                const auto *t = static_cast<const TernaryOpNode *>(node);
                uint16_t a = sourceRegFor(node->uid(), 0);
                uint16_t b = sourceRegFor(node->uid(), 1);
                uint16_t c = sourceRegFor(node->uid(), 2);
                uint16_t dst = builder.allocReg();
                builder.emit(t->opcode(), dst, a, b, c);
                outReg[{node->uid(), 0}] = dst;
                break;
            }
            case ShaderNodeKind::Output:
            {
                const auto *o = static_cast<const OutputNode *>(node);
                uint16_t src = sourceRegFor(node->uid(), 0);
                builder.emit(o->opcode(), 0, src);
                break;
            }
            }
        }

        MaterialProgram prog = builder.finalize();
        prog.parameterDefaults = std::move(paramDefaults);
        return prog;
    }
}
