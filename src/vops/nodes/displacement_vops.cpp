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
            // Same coercion shape as the math/noise nodes: accept whichever
            // Value flavour the upstream wire happens to carry and fall back
            // to a sensible default for a disconnected port.
            Vec3 asVec3(const Value &v)
            {
                if (auto *vv = std::get_if<Vec3>(&v)) return *vv;
                if (auto *f  = std::get_if<float>(&v)) return Vec3(*f);
                if (auto *i  = std::get_if<int>(&v))   return Vec3(static_cast<float>(*i));
                return Vec3(0.0f);
            }
            float asFloat(const Value &v)
            {
                if (auto *f  = std::get_if<float>(&v)) return *f;
                if (auto *vv = std::get_if<Vec3>(&v)) return vv->x;
                if (auto *i  = std::get_if<int>(&v))   return static_cast<float>(*i);
                return 0.0f;
            }
        }

        // ── displace_along_normal ────────────────────────────────────────────
        // out = P + normalize(N) * amount. The classic scalar-displacement
        // node: wire a noise into `amount`, get bumps along the surface
        // normal. `N` is normalized internally so users can plug raw
        // (possibly unnormalized) normals without surprises. A zero-length
        // normal degrades gracefully — the node passes P through.
        class DisplaceAlongNormalVop : public VopNode
        {
        public:
            explicit DisplaceAlongNormalVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "displace_along_normal"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("P",      DataType::Vec3));
                io.addInput(PortInfo::createInput("N",      DataType::Vec3));
                io.addInput(PortInfo::createInput("amount", DataType::Float));
                io.addOutput(PortInfo::createOutput("P", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const Vec3  p      = asVec3 (ctx.graph->readInput(ctx, uid(), 0).value_or(Value{Vec3(0.0f)}));
                const Vec3  n      = asVec3 (ctx.graph->readInput(ctx, uid(), 1).value_or(Value{Vec3(0.0f)}));
                const float amount = asFloat(ctx.graph->readInput(ctx, uid(), 2).value_or(Value{0.0f}));
                const float len2 = n.x * n.x + n.y * n.y + n.z * n.z;
                Vec3 out = p;
                if (len2 > 0.0f)
                {
                    const float inv = 1.0f / std::sqrt(len2);
                    out = Vec3(p.x + n.x * inv * amount,
                               p.y + n.y * inv * amount,
                               p.z + n.z * inv * amount);
                }
                ctx.graph->writeOutput(ctx, uid(), 0, out);
            }
        };

        // ── displace ─────────────────────────────────────────────────────────
        // out = P + offset. Generic vector displacement; pair with noise_vec3
        // or curl noise for fluid-looking jitter, or with any user-built Vec3
        // expression. No normalization, no scaling — what you wire is what
        // you add. (Use a Multiply upstream to scale the offset.)
        class DisplaceVop : public VopNode
        {
        public:
            explicit DisplaceVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "displace"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("P",      DataType::Vec3));
                io.addInput(PortInfo::createInput("offset", DataType::Vec3));
                io.addOutput(PortInfo::createOutput("P", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                const Vec3 p      = asVec3(ctx.graph->readInput(ctx, uid(), 0).value_or(Value{Vec3(0.0f)}));
                const Vec3 offset = asVec3(ctx.graph->readInput(ctx, uid(), 1).value_or(Value{Vec3(0.0f)}));
                ctx.graph->writeOutput(ctx, uid(), 0,
                    Vec3(p.x + offset.x, p.y + offset.y, p.z + offset.z));
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

        void registerDisplacementVops()
        {
            auto &reg = VopRegistry::instance();
            reg.registerType(
                {"displace_along_normal", "Displace Along Normal", "Displacement",
                 {{"P"}, {"N"}, {"amount"}}, {{"P"}}, {}},
                makeFactory<DisplaceAlongNormalVop>());
            reg.registerType(
                {"displace", "Displace (Vector)", "Displacement",
                 {{"P"}, {"offset"}}, {{"P"}}, {}},
                makeFactory<DisplaceVop>());
        }
    }
}
