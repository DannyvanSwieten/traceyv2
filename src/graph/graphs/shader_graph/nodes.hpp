#pragma once
#include "shader_graph_node.hpp"
#include "../../../core/types.hpp"
#include "../../../shading/material_program/opcodes.hpp"

#include <array>
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

    // ── Unified Material I/O terminals (Houdini-style) ───────────────────
    //
    // MaterialInputNode  exposes ALL surface attributes + pre-fetched
    //                    material inputs as named output ports on a single
    //                    node. The compiler emits the matching LoadX op
    //                    only for ports that actually feed a downstream
    //                    consumer, so an unused port costs nothing.
    //
    // MaterialOutputNode collects ALL material slot writes (Albedo,
    //                    Metallic, Roughness, etc.) as named input ports
    //                    on a single node. The compiler emits a WriteX
    //                    op only for ports that have an incoming wire (or
    //                    an inline `input_defaults` literal set via the
    //                    inspector); unconnected ports leave the slot at
    //                    whatever the host MaterialInputs pre-populated.
    //
    // Port order on each node is part of the wire-format contract — the
    // serializer / editor / compiler all key off these indices. Don't
    // reorder; only append.

    // ── MaterialInput port spec ──
    // (portName, opcode emitted when the port has a consumer).
    struct MaterialInputPortSpec
    {
        const char *name;
        Op op;
    };
    inline const std::array<MaterialInputPortSpec, 12> &materialInputPorts()
    {
        static const std::array<MaterialInputPortSpec, 12> kPorts = {{
            // Surface attributes (driven by the host's per-shading-point
            // SurfaceData).
            {"P",          Op::LoadPosition},
            {"N",          Op::LoadNormal},
            {"T",          Op::LoadTangent},
            {"V",          Op::LoadViewDir},
            {"uv0",        Op::LoadUV0},
            {"uv1",        Op::LoadUV1},
            {"InstanceID", Op::LoadInstanceIndex},
            // Pre-fetched material inputs (host fetches textures + factors
            // and feeds them in via MaterialInputs; the graph passes
            // through unchanged unless it overrides via a WriteX).
            {"Albedo",     Op::LoadInputAlbedo},
            {"Metallic",   Op::LoadInputMetallic},
            {"Roughness",  Op::LoadInputRoughness},
            {"Emission",   Op::LoadInputEmission},
            {"InNormal",   Op::LoadInputNormal},
        }};
        return kPorts;
    }

    class MaterialInputNode : public ShaderGraphNode
    {
    public:
        explicit MaterialInputNode(size_t uid) : ShaderGraphNode(uid) {}

        ShaderNodeKind kind() const override { return ShaderNodeKind::MaterialInput; }

        InputsAndOutputs ports() const override
        {
            InputsAndOutputs io;
            for (const auto &p : materialInputPorts())
                io.addOutput(PortInfo::createOutput(p.name, DataType::Vec4));
            return io;
        }
    };

    // ── MaterialOutput port spec ──
    // (portName, opcode emitted when the port is wired).
    struct MaterialOutputPortSpec
    {
        const char *name;
        Op op;
    };
    inline const std::array<MaterialOutputPortSpec, 8> &materialOutputPorts()
    {
        static const std::array<MaterialOutputPortSpec, 8> kPorts = {{
            {"Albedo",       Op::WriteAlbedo},
            {"Metallic",     Op::WriteMetallic},
            {"Roughness",    Op::WriteRoughness},
            {"Emission",     Op::WriteEmission},
            {"Normal",       Op::WriteNormal},
            {"Alpha",        Op::WriteAlpha},
            {"IOR",          Op::WriteIor},
            {"Transmission", Op::WriteTransmission},
        }};
        return kPorts;
    }

    class MaterialOutputNode : public ShaderGraphNode
    {
    public:
        explicit MaterialOutputNode(size_t uid) : ShaderGraphNode(uid) {}

        ShaderNodeKind kind() const override { return ShaderNodeKind::MaterialOutput; }

        InputsAndOutputs ports() const override
        {
            InputsAndOutputs io;
            for (const auto &p : materialOutputPorts())
                io.addInput(PortInfo::createInput(p.name, DataType::Vec4));
            return io;
        }
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
}
