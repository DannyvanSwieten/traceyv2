#pragma once
#include "shader_graph_node.hpp"
#include "../../../core/types.hpp"
#include "../../../shading/material_program/opcodes.hpp"

#include <string>

namespace tracey
{
    // A literal vec4 value baked into the program at compile time.
    // Compiler emits Op::LoadConst.
    class ConstantNode : public ShaderGraphNode
    {
    public:
        ConstantNode(size_t uid, const Vec4 &value)
            : ShaderGraphNode(uid), m_value(value) {}

        ShaderNodeKind kind() const override { return ShaderNodeKind::Constant; }

        InputsAndOutputs ports() const override
        {
            InputsAndOutputs io;
            io.addOutput(PortInfo::createOutput("value", DataType::Vec4));
            return io;
        }

        const Vec4 &value() const { return m_value; }

    private:
        Vec4 m_value;
    };

    // A runtime-mutable parameter slot (the graph's animation surface).
    // Compiler allocates a slot index and emits Op::LoadParam; defaultValue
    // is carried into MaterialProgram::parameterDefaults so the host can
    // pre-populate the parameters buffer.
    class ParameterNode : public ShaderGraphNode
    {
    public:
        ParameterNode(size_t uid, std::string name, const Vec4 &defaultValue)
            : ShaderGraphNode(uid), m_name(std::move(name)), m_default(defaultValue) {}

        ShaderNodeKind kind() const override { return ShaderNodeKind::Parameter; }

        InputsAndOutputs ports() const override
        {
            InputsAndOutputs io;
            io.addOutput(PortInfo::createOutput("value", DataType::Vec4));
            return io;
        }

        const std::string &paramName() const { return m_name; }
        const Vec4 &defaultValue() const { return m_default; }

    private:
        std::string m_name;
        Vec4 m_default;
    };

    // Geometry attribute (position/normal/...). m_op must be one of the
    // surface load opcodes (Op::LoadPosition through Op::LoadUV1).
    class SurfaceAttributeNode : public ShaderGraphNode
    {
    public:
        SurfaceAttributeNode(size_t uid, Op surfaceOp)
            : ShaderGraphNode(uid), m_op(surfaceOp) {}

        ShaderNodeKind kind() const override { return ShaderNodeKind::SurfaceAttribute; }

        InputsAndOutputs ports() const override
        {
            InputsAndOutputs io;
            io.addOutput(PortInfo::createOutput("value", DataType::Vec4));
            return io;
        }

        Op opcode() const { return m_op; }

    private:
        Op m_op;
    };

    // Pre-fetched material attribute (albedo/metallic/...). m_op must be
    // one of Op::LoadInputAlbedo through Op::LoadInputNormal.
    class InputAttributeNode : public ShaderGraphNode
    {
    public:
        InputAttributeNode(size_t uid, Op inputOp)
            : ShaderGraphNode(uid), m_op(inputOp) {}

        ShaderNodeKind kind() const override { return ShaderNodeKind::InputAttribute; }

        InputsAndOutputs ports() const override
        {
            InputsAndOutputs io;
            io.addOutput(PortInfo::createOutput("value", DataType::Vec4));
            return io;
        }

        Op opcode() const { return m_op; }

    private:
        Op m_op;
    };

    // Two-input math op (Add, Sub, Mul, Div, Dot3, Cross).
    // Input ports: 0 = a (srcA), 1 = b (srcB). Output port: 0 = result.
    class BinaryOpNode : public ShaderGraphNode
    {
    public:
        BinaryOpNode(size_t uid, Op op)
            : ShaderGraphNode(uid), m_op(op) {}

        ShaderNodeKind kind() const override { return ShaderNodeKind::BinaryOp; }

        InputsAndOutputs ports() const override
        {
            InputsAndOutputs io;
            io.addInput(PortInfo::createInput("a", DataType::Vec4));
            io.addInput(PortInfo::createInput("b", DataType::Vec4));
            io.addOutput(PortInfo::createOutput("result", DataType::Vec4));
            return io;
        }

        Op opcode() const { return m_op; }

    private:
        Op m_op;
    };

    // One-input op (Neg, Saturate, Normalize3, Length3, Splat).
    class UnaryOpNode : public ShaderGraphNode
    {
    public:
        UnaryOpNode(size_t uid, Op op)
            : ShaderGraphNode(uid), m_op(op) {}

        ShaderNodeKind kind() const override { return ShaderNodeKind::UnaryOp; }

        InputsAndOutputs ports() const override
        {
            InputsAndOutputs io;
            io.addInput(PortInfo::createInput("a", DataType::Vec4));
            io.addOutput(PortInfo::createOutput("result", DataType::Vec4));
            return io;
        }

        Op opcode() const { return m_op; }

    private:
        Op m_op;
    };

    // Three-input op (Mix, Clamp). For Mix: a, b are operands and c.x is the
    // blend factor; for Clamp: a is the value, b/c are min/max.
    class TernaryOpNode : public ShaderGraphNode
    {
    public:
        TernaryOpNode(size_t uid, Op op)
            : ShaderGraphNode(uid), m_op(op) {}

        ShaderNodeKind kind() const override { return ShaderNodeKind::TernaryOp; }

        InputsAndOutputs ports() const override
        {
            InputsAndOutputs io;
            io.addInput(PortInfo::createInput("a", DataType::Vec4));
            io.addInput(PortInfo::createInput("b", DataType::Vec4));
            io.addInput(PortInfo::createInput("c", DataType::Vec4));
            io.addOutput(PortInfo::createOutput("result", DataType::Vec4));
            return io;
        }

        Op opcode() const { return m_op; }

    private:
        Op m_op;
    };

    // Sink node: writes its single input into a slot of MaterialEvalResult.
    // m_op must be one of Op::WriteAlbedo through Op::WriteTransmission.
    class OutputNode : public ShaderGraphNode
    {
    public:
        OutputNode(size_t uid, Op writeOp)
            : ShaderGraphNode(uid), m_op(writeOp) {}

        ShaderNodeKind kind() const override { return ShaderNodeKind::Output; }

        InputsAndOutputs ports() const override
        {
            InputsAndOutputs io;
            io.addInput(PortInfo::createInput("value", DataType::Vec4));
            return io;
        }

        Op opcode() const { return m_op; }

    private:
        Op m_op;
    };
}
