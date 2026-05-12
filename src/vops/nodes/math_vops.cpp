#include "../register_builtins.hpp"
#include "../vop_node.hpp"
#include "../vop_graph.hpp"
#include "../vop_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

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
            bool anyVec3(const Value &a, const Value &b, const Value &c)
            {
                return std::holds_alternative<Vec3>(a) ||
                       std::holds_alternative<Vec3>(b) ||
                       std::holds_alternative<Vec3>(c);
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

        namespace
        {
            // Scalar-level binary op kernels. The template below dispatches
            // here per-component for Vec3 inputs and once for floats.
            // Division/modulo are defensive — a 0 divisor yields 0 rather
            // than NaN so a bad input doesn't silently corrupt geometry
            // through the rest of the cook.
            template <int OP>
            float binaryScalarOp(float a, float b)
            {
                if      constexpr (OP == 0) return a + b;
                else if constexpr (OP == 1) return a - b;
                else if constexpr (OP == 2) return a * b;
                else if constexpr (OP == 3) return b != 0.0f ? a / b : 0.0f;
                else if constexpr (OP == 4) return b != 0.0f ? std::fmod(a, b) : 0.0f;
                else if constexpr (OP == 5) return std::pow(a, b);
                else if constexpr (OP == 6) return std::min(a, b);
                else                        return std::max(a, b);
            }
        }

        // ── Binary math ──────────────────────────────────────────────────────
        // Inputs: a, b. Output type follows max(a, b) — Vec3 wins over float.
        // For "asymmetric" uses (e.g. multiply Vec3 by float scalar) we splat
        // the float into a Vec3. The OP template parameter picks which scalar
        // kernel binaryScalarOp<OP> runs per component.
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
                    Vec3 av = asVec3(a), bv = asVec3(b);
                    Vec3 r(binaryScalarOp<OP>(av.x, bv.x),
                           binaryScalarOp<OP>(av.y, bv.y),
                           binaryScalarOp<OP>(av.z, bv.z));
                    ctx.graph->writeOutput(ctx, uid(), 0, r);
                }
                else
                {
                    ctx.graph->writeOutput(ctx, uid(), 0,
                        binaryScalarOp<OP>(asFloat(a), asFloat(b)));
                }
            }
        };

        class AddVop      : public BinaryMathVop<0> { public: using BinaryMathVop::BinaryMathVop;
                                                       std::string kind() const override { return "add"; } };
        class SubtractVop : public BinaryMathVop<1> { public: using BinaryMathVop::BinaryMathVop;
                                                       std::string kind() const override { return "subtract"; } };
        class MultiplyVop : public BinaryMathVop<2> { public: using BinaryMathVop::BinaryMathVop;
                                                       std::string kind() const override { return "multiply"; } };
        class DivideVop   : public BinaryMathVop<3> { public: using BinaryMathVop::BinaryMathVop;
                                                       std::string kind() const override { return "divide"; } };
        class ModuloVop   : public BinaryMathVop<4> { public: using BinaryMathVop::BinaryMathVop;
                                                       std::string kind() const override { return "modulo"; } };
        class PowerVop    : public BinaryMathVop<5> { public: using BinaryMathVop::BinaryMathVop;
                                                       std::string kind() const override { return "power"; } };
        class MinVop      : public BinaryMathVop<6> { public: using BinaryMathVop::BinaryMathVop;
                                                       std::string kind() const override { return "min"; } };
        class MaxVop      : public BinaryMathVop<7> { public: using BinaryMathVop::BinaryMathVop;
                                                       std::string kind() const override { return "max"; } };

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

        // ── clamp (value, min, max → value) ──────────────────────────────────
        // Float and Vec3 both supported. The output picks the input's flavor
        // — wire a Vec3 in and get a per-component-clamped Vec3 back; wire a
        // Float and get a Float. Lets a noise output get bounded into a
        // useful range without the user reaching for a math expression.
        class ClampVop : public VopNode
        {
        public:
            explicit ClampVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "clamp"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("value", DataType::Float));
                io.addInput(PortInfo::createInput("min",   DataType::Float));
                io.addInput(PortInfo::createInput("max",   DataType::Float));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                auto v   = ctx.graph->readInput(ctx, uid(), 0).value_or(Value{0.0f});
                auto lo  = ctx.graph->readInput(ctx, uid(), 1).value_or(Value{0.0f});
                auto hi  = ctx.graph->readInput(ctx, uid(), 2).value_or(Value{1.0f});
                if (anyVec3(v, lo, hi))
                {
                    Vec3 vv = asVec3(v), lv = asVec3(lo), hv = asVec3(hi);
                    Vec3 r(std::min(std::max(vv.x, lv.x), hv.x),
                           std::min(std::max(vv.y, lv.y), hv.y),
                           std::min(std::max(vv.z, lv.z), hv.z));
                    ctx.graph->writeOutput(ctx, uid(), 0, r);
                }
                else
                {
                    float fv = asFloat(v), fl = asFloat(lo), fh = asFloat(hi);
                    ctx.graph->writeOutput(ctx, uid(), 0,
                        std::min(std::max(fv, fl), fh));
                }
            }
        };

        // ── fit (value, src_min, src_max, dst_min, dst_max → value) ──────────
        // Linear remap from [src_min..src_max] to [dst_min..dst_max]. NOT
        // clamped — caller composes with `clamp` when they want fit01-style
        // bounded output. Houdini's `fit` is the same plus an extrap clamp;
        // we keep them separate so the wiring stays explicit.
        class FitVop : public VopNode
        {
        public:
            explicit FitVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "fit"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("value",   DataType::Float));
                io.addInput(PortInfo::createInput("src_min", DataType::Float));
                io.addInput(PortInfo::createInput("src_max", DataType::Float));
                io.addInput(PortInfo::createInput("dst_min", DataType::Float));
                io.addInput(PortInfo::createInput("dst_max", DataType::Float));
                io.addOutput(PortInfo::createOutput("out",   DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const float v = asFloat(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{0.0f}));
                const float sa = asFloat(ctx.graph->readInput(ctx, uid(), 1).value_or(Value{0.0f}));
                const float sb = asFloat(ctx.graph->readInput(ctx, uid(), 2).value_or(Value{1.0f}));
                const float da = asFloat(ctx.graph->readInput(ctx, uid(), 3).value_or(Value{0.0f}));
                const float db = asFloat(ctx.graph->readInput(ctx, uid(), 4).value_or(Value{1.0f}));
                const float span = sb - sa;
                const float t = (std::abs(span) > 1e-12f) ? (v - sa) / span : 0.0f;
                ctx.graph->writeOutput(ctx, uid(), 0, da + (db - da) * t);
            }
        };

        // ── rand (seed → [0,1) float) ────────────────────────────────────────
        // Per-point pseudo-random. Mixes the input `seed` (typically @ptnum
        // exposed via a `bind_attr` reader, or just a constant for static
        // randomness) with a Weyl-sequence-style hash. Stable and seed-
        // dependent, so the same seed always produces the same value —
        // critical for scatter / instance setups where you want the
        // distribution to look "designed" rather than re-rolled every cook.
        class RandVop : public VopNode
        {
        public:
            explicit RandVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "rand"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("seed", DataType::Float));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const float seed = asFloat(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{0.0f}));
                // xorshift32-style hash on the bit pattern of the seed,
                // chained twice for decent statistical spread. Output is
                // in [0,1) — multiply/fit downstream when you need a
                // larger range.
                uint32_t bits;
                std::memcpy(&bits, &seed, sizeof(bits));
                bits = bits * 2654435761u + 374761393u;
                bits ^= bits >> 13;
                bits *= 0x85ebca6bu;
                bits ^= bits >> 16;
                const float r = static_cast<float>(bits & 0x00ffffffu) /
                                static_cast<float>(0x01000000u);
                ctx.graph->writeOutput(ctx, uid(), 0, r);
            }
        };

        namespace
        {
            // Scalar-level unary op kernels. Sqrt is defensive on negative
            // input (returns 0 rather than NaN), matching the divide-by-zero
            // policy on binaryScalarOp — keeps a single bad value from
            // poisoning the rest of the cook.
            template <int OP>
            float unaryScalarOp(float v)
            {
                if      constexpr (OP == 0) return std::abs(v);
                else if constexpr (OP == 1) return -v;
                else if constexpr (OP == 2) return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
                else if constexpr (OP == 3) return std::floor(v);
                else if constexpr (OP == 4) return std::ceil(v);
                else if constexpr (OP == 5) return std::round(v);
                else if constexpr (OP == 6) return v - std::floor(v);
                else if constexpr (OP == 7) return v >= 0.0f ? std::sqrt(v) : 0.0f;
                else if constexpr (OP == 8) return std::sin(v);
                else                        return std::cos(v);
            }
        }

        // ── Unary math ───────────────────────────────────────────────────────
        // Single input/output; output type matches input (float stays float,
        // Vec3 is processed per-component). Trig nodes treat input as radians
        // — matches glm / GLSL convention. Use Multiply to scale by π/180 if
        // you want to write angles in degrees.
        template <int OP>
        class UnaryMathVop : public VopNode
        {
        public:
            explicit UnaryMathVop(size_t uid) : VopNode(uid) {}
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("in",   DataType::Float));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                auto in = ctx.graph->readInput(ctx, uid(), 0).value_or(Value{0.0f});
                if (std::holds_alternative<Vec3>(in))
                {
                    Vec3 v = asVec3(in);
                    Vec3 r(unaryScalarOp<OP>(v.x),
                           unaryScalarOp<OP>(v.y),
                           unaryScalarOp<OP>(v.z));
                    ctx.graph->writeOutput(ctx, uid(), 0, r);
                }
                else
                {
                    ctx.graph->writeOutput(ctx, uid(), 0,
                        unaryScalarOp<OP>(asFloat(in)));
                }
            }
        };

        class AbsVop    : public UnaryMathVop<0> { public: using UnaryMathVop::UnaryMathVop;
                                                    std::string kind() const override { return "abs"; } };
        class NegateVop : public UnaryMathVop<1> { public: using UnaryMathVop::UnaryMathVop;
                                                    std::string kind() const override { return "negate"; } };
        class SignVop   : public UnaryMathVop<2> { public: using UnaryMathVop::UnaryMathVop;
                                                    std::string kind() const override { return "sign"; } };
        class FloorVop  : public UnaryMathVop<3> { public: using UnaryMathVop::UnaryMathVop;
                                                    std::string kind() const override { return "floor"; } };
        class CeilVop   : public UnaryMathVop<4> { public: using UnaryMathVop::UnaryMathVop;
                                                    std::string kind() const override { return "ceil"; } };
        class RoundVop  : public UnaryMathVop<5> { public: using UnaryMathVop::UnaryMathVop;
                                                    std::string kind() const override { return "round"; } };
        class FractVop  : public UnaryMathVop<6> { public: using UnaryMathVop::UnaryMathVop;
                                                    std::string kind() const override { return "fract"; } };
        class SqrtVop   : public UnaryMathVop<7> { public: using UnaryMathVop::UnaryMathVop;
                                                    std::string kind() const override { return "sqrt"; } };
        class SinVop    : public UnaryMathVop<8> { public: using UnaryMathVop::UnaryMathVop;
                                                    std::string kind() const override { return "sin"; } };
        class CosVop    : public UnaryMathVop<9> { public: using UnaryMathVop::UnaryMathVop;
                                                    std::string kind() const override { return "cos"; } };

        // ── atan2 (y, x → radians) ───────────────────────────────────────────
        // Two-argument arctangent. Float only — Vec3 inputs are coerced to
        // their x component, which matches the semantic that atan2 returns
        // an angle, not a vector.
        class Atan2Vop : public VopNode
        {
        public:
            explicit Atan2Vop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "atan2"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("y",   DataType::Float));
                io.addInput(PortInfo::createInput("x",   DataType::Float));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const float y = asFloat(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{0.0f}));
                const float x = asFloat(ctx.graph->readInput(ctx, uid(), 1).value_or(Value{0.0f}));
                ctx.graph->writeOutput(ctx, uid(), 0, std::atan2(y, x));
            }
        };

        // ── cross (Vec3 × Vec3 → Vec3) ───────────────────────────────────────
        // Right-handed cross product. Inputs are coerced to Vec3 (a float is
        // splatted, matching the rest of the math nodes). Output is always
        // Vec3 since the geometric meaning collapses if one input is scalar.
        class CrossVop : public VopNode
        {
        public:
            explicit CrossVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "cross"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("a",   DataType::Vec3));
                io.addInput(PortInfo::createInput("b",   DataType::Vec3));
                io.addOutput(PortInfo::createOutput("out", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                Vec3 a = asVec3(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{Vec3(0.0f)}));
                Vec3 b = asVec3(ctx.graph->readInput(ctx, uid(), 1).value_or(Value{Vec3(0.0f)}));
                ctx.graph->writeOutput(ctx, uid(), 0,
                    Vec3(a.y * b.z - a.z * b.y,
                         a.z * b.x - a.x * b.z,
                         a.x * b.y - a.y * b.x));
            }
        };

        // ── make_vec3 (x, y, z → Vec3) ───────────────────────────────────────
        // Compose a Vec3 from three scalars. The dual of split_vec3, and the
        // only way (besides constant_vec3) to build a vector from per-axis
        // math results.
        class MakeVec3Vop : public VopNode
        {
        public:
            explicit MakeVec3Vop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "make_vec3"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("x",   DataType::Float));
                io.addInput(PortInfo::createInput("y",   DataType::Float));
                io.addInput(PortInfo::createInput("z",   DataType::Float));
                io.addOutput(PortInfo::createOutput("out", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const float x = asFloat(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{0.0f}));
                const float y = asFloat(ctx.graph->readInput(ctx, uid(), 1).value_or(Value{0.0f}));
                const float z = asFloat(ctx.graph->readInput(ctx, uid(), 2).value_or(Value{0.0f}));
                ctx.graph->writeOutput(ctx, uid(), 0, Vec3(x, y, z));
            }
        };

        // ── split_vec3 (Vec3 → x, y, z) ──────────────────────────────────────
        // Pull the three components out as separate floats so each can be
        // operated on independently. Three output ports; downstream nodes
        // wire to whichever component they care about.
        class SplitVec3Vop : public VopNode
        {
        public:
            explicit SplitVec3Vop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "split_vec3"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("in", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("x", DataType::Float));
                io.addOutput(PortInfo::createOutput("y", DataType::Float));
                io.addOutput(PortInfo::createOutput("z", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                Vec3 v = asVec3(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{Vec3(0.0f)}));
                ctx.graph->writeOutput(ctx, uid(), 0, v.x);
                ctx.graph->writeOutput(ctx, uid(), 1, v.y);
                ctx.graph->writeOutput(ctx, uid(), 2, v.z);
            }
        };

        // ── compare (a, b → 0/1) ─────────────────────────────────────────────
        // Outputs 1.0 when the relation holds, 0.0 otherwise. The `op`
        // string param picks the comparison: "lt", "le", "eq", "ne", "ge",
        // "gt". Float-only (Vec3 inputs coerce to .x) — comparing whole
        // vectors with a single boolean output rarely matches user intent.
        class CompareVop : public VopNode
        {
        public:
            explicit CompareVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeString("op", "lt"));
            }
            std::string kind() const override { return "compare"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("a",   DataType::Float));
                io.addInput(PortInfo::createInput("b",   DataType::Float));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const float a = asFloat(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{0.0f}));
                const float b = asFloat(ctx.graph->readInput(ctx, uid(), 1).value_or(Value{0.0f}));
                const std::string op = paramString("op", "lt");
                bool r = false;
                if      (op == "lt") r = a <  b;
                else if (op == "le") r = a <= b;
                else if (op == "eq") r = a == b;
                else if (op == "ne") r = a != b;
                else if (op == "ge") r = a >= b;
                else if (op == "gt") r = a >  b;
                ctx.graph->writeOutput(ctx, uid(), 0, r ? 1.0f : 0.0f);
            }
        };

        // ── switch (a, b, cond → a or b) ─────────────────────────────────────
        // Two-way switch. Picks `b` when cond > 0.5, else `a`. The output
        // type follows the selected branch (so wiring two Vec3s through a
        // boolean cond yields a Vec3; wiring two floats yields a float).
        // Both branches are still evaluated upstream — the switch just
        // selects which value reaches downstream.
        class SwitchVop : public VopNode
        {
        public:
            explicit SwitchVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "switch"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("a",    DataType::Float));
                io.addInput(PortInfo::createInput("b",    DataType::Float));
                io.addInput(PortInfo::createInput("cond", DataType::Float));
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                auto a    = ctx.graph->readInput(ctx, uid(), 0).value_or(Value{0.0f});
                auto b    = ctx.graph->readInput(ctx, uid(), 1).value_or(Value{0.0f});
                auto cond = ctx.graph->readInput(ctx, uid(), 2).value_or(Value{0.0f});
                const bool pickB = asFloat(cond) > 0.5f;
                ctx.graph->writeOutput(ctx, uid(), 0, pickB ? b : a);
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

            reg.registerType(
                {"divide", "Divide", "Math",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<DivideVop>());
            reg.registerType(
                {"modulo", "Modulo", "Math",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<ModuloVop>());
            reg.registerType(
                {"power", "Power", "Math",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<PowerVop>());
            reg.registerType(
                {"min", "Min", "Math",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<MinVop>());
            reg.registerType(
                {"max", "Max", "Math",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<MaxVop>());

            reg.registerType(
                {"clamp", "Clamp", "Math",
                 {{"value"}, {"min"}, {"max"}}, {{"out"}}, {}},
                makeFactory<ClampVop>());
            reg.registerType(
                {"fit", "Fit Range", "Math",
                 {{"value"}, {"src_min"}, {"src_max"}, {"dst_min"}, {"dst_max"}},
                 {{"out"}}, {}},
                makeFactory<FitVop>());
            reg.registerType(
                {"rand", "Random", "Math",
                 {{"seed"}}, {{"out"}}, {}},
                makeFactory<RandVop>());

            // Unary math — kept under "Math" so the right-click "Add Node"
            // submenu groups everything in one place. ptnum → @sin(ptnum)
            // patterns are the common case.
            reg.registerType({"abs",    "Abs",    "Math", {{"in"}}, {{"out"}}, {}}, makeFactory<AbsVop>());
            reg.registerType({"negate", "Negate", "Math", {{"in"}}, {{"out"}}, {}}, makeFactory<NegateVop>());
            reg.registerType({"sign",   "Sign",   "Math", {{"in"}}, {{"out"}}, {}}, makeFactory<SignVop>());
            reg.registerType({"floor",  "Floor",  "Math", {{"in"}}, {{"out"}}, {}}, makeFactory<FloorVop>());
            reg.registerType({"ceil",   "Ceil",   "Math", {{"in"}}, {{"out"}}, {}}, makeFactory<CeilVop>());
            reg.registerType({"round",  "Round",  "Math", {{"in"}}, {{"out"}}, {}}, makeFactory<RoundVop>());
            reg.registerType({"fract",  "Fract",  "Math", {{"in"}}, {{"out"}}, {}}, makeFactory<FractVop>());
            reg.registerType({"sqrt",   "Sqrt",   "Math", {{"in"}}, {{"out"}}, {}}, makeFactory<SqrtVop>());
            reg.registerType({"sin",    "Sin (rad)", "Math", {{"in"}}, {{"out"}}, {}}, makeFactory<SinVop>());
            reg.registerType({"cos",    "Cos (rad)", "Math", {{"in"}}, {{"out"}}, {}}, makeFactory<CosVop>());
            reg.registerType(
                {"atan2", "Atan2 (y, x)", "Math",
                 {{"y"}, {"x"}}, {{"out"}}, {}},
                makeFactory<Atan2Vop>());

            // Vector construction / decomposition. Put these next to the
            // existing Vector category so the dual {make_vec3, split_vec3}
            // pair is easy to find.
            reg.registerType(
                {"cross", "Cross Product", "Vector",
                 {{"a"}, {"b"}}, {{"out"}}, {}},
                makeFactory<CrossVop>());
            reg.registerType(
                {"make_vec3", "Make Vec3", "Vector",
                 {{"x"}, {"y"}, {"z"}}, {{"out"}}, {}},
                makeFactory<MakeVec3Vop>());
            reg.registerType(
                {"split_vec3", "Split Vec3", "Vector",
                 {{"in"}}, {{"x"}, {"y"}, {"z"}}, {}},
                makeFactory<SplitVec3Vop>());

            // Logic — the first non-arithmetic primitives. compare emits
            // 0/1; switch picks between two values based on the same.
            reg.registerType(
                {"compare", "Compare", "Logic",
                 {{"a"}, {"b"}}, {{"out"}},
                 {{"op", ParamType::String, "\"lt\""}}},
                makeFactory<CompareVop>());
            reg.registerType(
                {"switch", "Switch (Two-Way)", "Logic",
                 {{"a"}, {"b"}, {"cond"}}, {{"out"}}, {}},
                makeFactory<SwitchVop>());
        }
    }
}
