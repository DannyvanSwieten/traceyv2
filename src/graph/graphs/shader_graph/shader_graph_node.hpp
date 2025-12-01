#pragma once

#include "../../node.hpp"
#include "instructions.hpp"

namespace tracey
{
    class ShaderGraphNode : public Node
    {
    public:
        ShaderGraphNode(size_t uid);
        ~ShaderGraphNode() override = default;

        virtual ShaderGraphInstruction instruction() const = 0;
    };
}