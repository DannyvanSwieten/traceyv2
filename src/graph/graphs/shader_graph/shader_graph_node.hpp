#pragma once
#include "../../node.hpp"
#include "../../port_info.hpp"
#include "../../../core/types.hpp"

#include <optional>
#include <unordered_map>

namespace tracey
{
    // Coarse classification of shader graph nodes; the compiler dispatches on
    // this and downcasts to the concrete subclass to read its data.
    enum class ShaderNodeKind
    {
        Constant,         // ConstantNode    — embedded vec4 literal (LoadConst)
        Parameter,        // ParameterNode   — runtime-mutable slot   (LoadParam)
        SurfaceAttribute, // SurfaceAttrNode — Position, Normal, ViewDir, UV0, ...
        InputAttribute,   // InputAttrNode   — pre-fetched material albedo/metallic/...
        BinaryOp,         // BinaryOpNode    — Add, Sub, Mul, Div, Dot3, Cross
        UnaryOp,          // UnaryOpNode     — Neg, Saturate, Normalize3, Length3, Splat
        TernaryOp,        // TernaryOpNode   — Mix, Clamp
        Output,           // OutputNode      — WriteAlbedo, WriteMetallic, ...
    };

    class ShaderGraphNode : public Node
    {
    public:
        ShaderGraphNode(size_t uid);
        ~ShaderGraphNode() override = default;

        virtual ShaderNodeKind kind() const = 0;

        // Port layout. Caller may use this to discover the number/types of
        // inputs and outputs without dispatching on kind. Inputs are positional:
        // input port index N maps to the Nth source operand of the emitted op.
        virtual InputsAndOutputs ports() const = 0;

        // Per-input default vec4 — used by the compiler when an input has
        // no incoming wire. Lets the user dial in a constant from the
        // inspector instead of dropping a ConstantNode and wiring it up.
        // Missing key → the compiler errors as before (preserves "did
        // you forget to wire something?" feedback during graph editing).
        const std::unordered_map<size_t, Vec4> &inputDefaults() const { return m_inputDefaults; }
        std::optional<Vec4> inputDefault(size_t portIdx) const
        {
            auto it = m_inputDefaults.find(portIdx);
            if (it == m_inputDefaults.end()) return std::nullopt;
            return it->second;
        }
        void setInputDefault(size_t portIdx, Vec4 v) { m_inputDefaults[portIdx] = v; }
        void clearInputDefault(size_t portIdx) { m_inputDefaults.erase(portIdx); }

    private:
        std::unordered_map<size_t, Vec4> m_inputDefaults;
    };
}
