#include "../register_builtins.hpp"
#include "../vop_node.hpp"
#include "../vop_graph.hpp"
#include "../vop_registry.hpp"

#include <cmath>
#include <memory>

namespace tracey
{
    namespace vops
    {
        namespace
        {
            // Helpers for reading inputs as a specific type with a fallback.
            // VOPs read upstream values that arrive as Value (variant). Most
            // math nodes are overloaded for float OR Vec3; we coerce on read.
            float asFloat(const Value &v)
            {
                if (auto *f = std::get_if<float>(&v)) return *f;
                if (auto *vv = std::get_if<Vec3>(&v)) return vv->x;
                if (auto *i = std::get_if<int>(&v)) return static_cast<float>(*i);
                return 0.0f;
            }
            Vec3 asVec3(const Value &v)
            {
                if (auto *vv = std::get_if<Vec3>(&v)) return *vv;
                if (auto *f = std::get_if<float>(&v)) return Vec3(*f);
                if (auto *i = std::get_if<int>(&v)) return Vec3(static_cast<float>(*i));
                return Vec3(0.0f);
            }
            // Pick float or Vec3 path based on which input is "richer". Both
            // a + b being float yields float; any Vec3 input promotes to Vec3.
            bool anyVec3(const Value &a, const Value &b)
            {
                return std::holds_alternative<Vec3>(a) ||
                       std::holds_alternative<Vec3>(b);
            }
        }

        // ── constant_float ───────────────────────────────────────────────────
        class ConstantFloatVop : public VopNode
        {
        public:
            explicit ConstantFloatVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeFloat("value", 0.0f));
            }
            std::string kind() const override { return "constant_float"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                ctx.graph->writeOutput(ctx, uid(), 0, paramFloat("value", 0.0f));
            }
        };

        // ── add / subtract / multiply (each overloaded for float and Vec3) ──
        // Inputs: a, b. Output type follows max(a, b) — Vec3 wins over float.
        // For "asymmetric" uses (e.g. multiply Vec3 by float scalar) we splat
        // the float into a Vec3.
        template <int OP>
        class BinaryMathVop : public VopNode
        {
        public:
            explicit BinaryMathVop(size_t uid) : VopNode(uid) {}
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("a", DataType::Float));
                io.addInput(PortInfo::createInput("b", DataType::Float));
                // Output type is dynamic; we declare Float and let downstream
                // readers coerce. Editor visual types are advisory in v1.
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                auto a = ctx.graph->readInput(ctx, uid(), 0).value_or(Value{0.0f});
                auto b = ctx.graph->readInput(ctx, uid(), 1).value_or(Value{0.0f});
                if (anyVec3(a, b))
                {
                    Vec3 av = asVec3(a), bv = asVec3(b), r;
                    if      constexpr (OP == 0) r = Vec3(av.x + bv.x, av.y + bv.y, av.z + bv.z);
                    else if constexpr (OP == 1) r = Vec3(av.x - bv.x, av.y - bv.y, av.z - bv.z);
                    else                        r = Vec3(av.x * bv.x, av.y * bv.y, av.z * bv.z);
                    ctx.graph->writeOutput(ctx, uid(), 0, r);
                }
                else
                {
                    float af = asFloat(a), bf = asFloat(b), r;
                    if      constexpr (OP == 0) r = af + bf;
                    else if constexpr (OP == 1) r = af - bf;
                    else                        r = af * bf;
                    ctx.graph->writeOutput(ctx, uid(), 0, r);
                }
            }
        };

        class AddVop : public BinaryMathVop<0>
        {
        public:
            using BinaryMathVop::BinaryMathVop;
            std::string kind() const override { return "add"; }
        };
        class SubtractVop : public BinaryMathVop<1>
        {
        public:
            using BinaryMathVop::BinaryMathVop;
            std::string kind() const override { return "subtract"; }
        };
        class MultiplyVop : public BinaryMathVop<2>
        {
        public:
            using BinaryMathVop::BinaryMathVop;
            std::string kind() const override { return "multiply"; }
        };

        // ── mix (lerp) — out = a + (b - a) * t. Vec3 if either a/b is Vec3.
        class MixVop : public VopNode
        {
        public:
            explicit MixVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "mix"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("a", DataType::Float));
                io.addInput(PortInfo::createInput("b", DataType::Float));
                io.addInput(PortInfo::createInput("t", DataType::Float));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                auto a = ctx.graph->readInput(ctx, uid(), 0).value_or(Value{0.0f});
                auto b = ctx.graph->readInput(ctx, uid(), 1).value_or(Value{0.0f});
                auto t = ctx.graph->readInput(ctx, uid(), 2).value_or(Value{0.0f});
                float tf = asFloat(t);
                if (anyVec3(a, b))
                {
                    Vec3 av = asVec3(a), bv = asVec3(b);
                    Vec3 r(av.x + (bv.x - av.x) * tf,
                           av.y + (bv.y - av.y) * tf,
                           av.z + (bv.z - av.z) * tf);
                    ctx.graph->writeOutput(ctx, uid(), 0, r);
                }
                else
                {
                    float af = asFloat(a), bf = asFloat(b);
                    ctx.graph->writeOutput(ctx, uid(), 0, af + (bf - af) * tf);
                }
            }
        };

        // ── length (Vec3 → Float) ────────────────────────────────────────────
        class LengthVop : public VopNode
        {
        public:
            explicit LengthVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "length"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("v", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                Vec3 v = asVec3(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{Vec3(0.0f)}));
                float r = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                ctx.graph->writeOutput(ctx, uid(), 0, r);
            }
        };

        // ── distance (Vec3, Vec3 → Float) ────────────────────────────────────
        class DistanceVop : public VopNode
        {
        public:
            explicit DistanceVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "distance"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("a", DataType::Vec3));
                io.addInput(PortInfo::createInput("b", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                Vec3 a = asVec3(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{Vec3(0.0f)}));
                Vec3 b = asVec3(ctx.graph->readInput(ctx, uid(), 1).value_or(Value{Vec3(0.0f)}));
                float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                ctx.graph->writeOutput(ctx, uid(), 0, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        };

        // ── dot (Vec3, Vec3 → Float) ─────────────────────────────────────────
        class DotVop : public VopNode
        {
        public:
            explicit DotVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "dot"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("a", DataType::Vec3));
                io.addInput(PortInfo::createInput("b", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                Vec3 a = asVec3(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{Vec3(0.0f)}));
                Vec3 b = asVec3(ctx.graph->readInput(ctx, uid(), 1).value_or(Value{Vec3(0.0f)}));
                ctx.graph->writeOutput(ctx, uid(), 0, a.x * b.x + a.y * b.y + a.z * b.z);
            }
        };

        // ── normalize (Vec3 → Vec3) ──────────────────────────────────────────
        class NormalizeVop : public VopNode
        {
        public:
            explicit NormalizeVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "normalize"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("v", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                Vec3 v = asVec3(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{Vec3(0.0f)}));
                float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
                Vec3 r(0.0f);
                if (len2 > 0.0f)
                {
                    float inv = 1.0f / std::sqrt(len2);
                    r = Vec3(v.x * inv, v.y * inv, v.z * inv);
                }
                ctx.graph->writeOutput(ctx, uid(), 0, r);
            }
        };

        namespace
        {
            template <typename T>
            VopRegistry::Factory makeFactory()
            {
                return [](size_t uid) { return std::make_unique<T>(uid); };
            }
        }

        void registerMathVops()
        {
            auto &reg = VopRegistry::instance();
            reg.registerType(
                {"constant_float", "Constant Float", "Constants",
                 /*inputs*/ {}, /*outputs*/ {{"out"}},
                 /*params*/ {{"value", ParamType::Float, "0.0"}}},
                makeFactory<ConstantFloatVop>());

            reg.registerType(
                {"add", "Add", "Math",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<AddVop>());
            reg.registerType(
                {"subtract", "Subtract", "Math",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<SubtractVop>());
            reg.registerType(
                {"multiply", "Multiply", "Math",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<MultiplyVop>());
            reg.registerType(
                {"mix", "Mix (Lerp)", "Math",
                 {{"a"}, {"b"}, {"t"}}, {{"out"}}, {}},
                makeFactory<MixVop>());

            reg.registerType(
                {"length", "Length", "Vector",
                 {{"v"}}, {{"out"}}, {}},
                makeFactory<LengthVop>());
            reg.registerType(
                {"distance", "Distance", "Vector",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<DistanceVop>());
            reg.registerType(
                {"dot", "Dot Product", "Vector",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<DotVop>());
            reg.registerType(
                {"normalize", "Normalize", "Vector",
                 {{"v"}}, {{"out"}}, {}},
                makeFactory<NormalizeVop>());
        }
    }
}
