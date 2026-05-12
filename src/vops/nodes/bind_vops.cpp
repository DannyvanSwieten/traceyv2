#include "../register_builtins.hpp"
#include "../vop_node.hpp"
#include "../vop_graph.hpp"
#include "../vop_registry.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute_table.hpp"

namespace tracey
{
    namespace vops
    {
        // ── bind_in_p ────────────────────────────────────────────────────────
        // Reads the current point's position from the geometry's "P" point
        // attribute. Zero inputs, one Vec3 output.
        class BindInPVop : public VopNode
        {
        public:
            explicit BindInPVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "bind_in_p"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addOutput(PortInfo::createOutput("P", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.geometry || !ctx.graph) return;
                const auto &positions = ctx.geometry->positions();
                Vec3 p = (ctx.pointIndex < positions.size())
                             ? positions[ctx.pointIndex]
                             : Vec3(0.0f);
                ctx.graph->writeOutput(ctx, uid(), 0, p);
            }
        };

        // ── bind_out_p ───────────────────────────────────────────────────────
        // Writes its single Vec3 input back into the geometry's "P" attribute.
        // One input, zero outputs.
        class BindOutPVop : public VopNode
        {
        public:
            explicit BindOutPVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "bind_out_p"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("P", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.geometry || !ctx.graph) return;
                auto in = ctx.graph->readInput(ctx, uid(), 0);
                if (!in) return;
                Vec3 v(0.0f);
                if (auto *p = std::get_if<Vec3>(&*in)) v = *p;
                else if (auto *f = std::get_if<float>(&*in)) v = Vec3(*f);
                else return;

                auto &positions = ctx.geometry->positions();
                if (ctx.pointIndex < positions.size()) positions[ctx.pointIndex] = v;
            }
        };

        // ── constant_vec3 ────────────────────────────────────────────────────
        // Emits its `value` parameter unchanged. Zero inputs, one Vec3 output.
        class ConstantVec3Vop : public VopNode
        {
        public:
            explicit ConstantVec3Vop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeVec3("value", Vec3(0.0f)));
            }
            std::string kind() const override { return "constant_vec3"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addOutput(PortInfo::createOutput("out", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                ctx.graph->writeOutput(ctx, uid(), 0, paramVec3("value", Vec3(0.0f)));
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

        void registerBindVops()
        {
            // Categories split bind nodes into Houdini-style "Input
            // Attributes" / "Output Attributes" groups so the context-menu
            // submenus read naturally — "browse what's available coming in"
            // vs. "what can I write out". Position passthrough (bind_in_p /
            // bind_out_p) is the canonical first wire users add.
            auto &reg = VopRegistry::instance();
            reg.registerType(
                {"bind_in_p", "Position (P)", "Input Attributes",
                 /*inputs*/ {},
                 /*outputs*/ {{"P"}},
                 /*params*/ {}},
                makeFactory<BindInPVop>());
            reg.registerType(
                {"bind_out_p", "Position (P)", "Output Attributes",
                 /*inputs*/ {{"P"}},
                 /*outputs*/ {},
                 /*params*/ {}},
                makeFactory<BindOutPVop>());
            reg.registerType(
                {"constant_vec3", "Constant Vec3", "Constants",
                 /*inputs*/ {},
                 /*outputs*/ {{"out"}},
                 /*params*/ {{"value", ParamType::Vec3, "[0, 0, 0]"}}},
                makeFactory<ConstantVec3Vop>());
        }
    }
}
