#pragma once
#include "../shader_graph_node.hpp"
namespace tracey
{
    class BinaryOperatorNode : public ShaderGraphNode
    {
    public:
        BinaryOperatorNode(size_t uid, ShaderGraphInstruction instr)
            : ShaderGraphNode(uid), m_instruction(instr)
        {
        }

        ShaderGraphInstruction instruction() const override
        {
            return m_instruction;
        }

    private:
        ShaderGraphInstruction m_instruction;
    };
}