#include "../register_builtins.hpp"
#include "../vop_node.hpp"
#include "../vop_graph.hpp"
#include "../vop_registry.hpp"

#include "../../geometry/geometry.hpp"
#include "../../geometry/attribute.hpp"
#include "../../geometry/attribute_table.hpp"

#include <memory>

namespace tracey
{
    namespace vops
    {
        // ── bind_in_attr_float ───────────────────────────────────────────────
        // Reads a named float point-attribute. If the attribute doesn't exist
        // or has the wrong type, emits 0.
        class BindInAttrFloatVop : public VopNode
        {
        public:
            explicit BindInAttrFloatVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeString("name", "Cd_x"));
            }
            std::string kind() const override { return "bind_in_attr_float"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addOutput(PortInfo::createOutput("out", DataType::Float));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.geometry || !ctx.graph) return;
                const std::string n = paramString("name", "");
                float v = 0.0f;
                if (const auto *a = ctx.geometry->points().get<float>(n))
                {
                    if (ctx.pointIndex < a->data().size()) v = a->data()[ctx.pointIndex];
                }
                ctx.graph->writeOutput(ctx, uid(), 0, v);
            }
        };

        // ── bind_in_attr_vec3 ────────────────────────────────────────────────
        class BindInAttrVec3Vop : public VopNode
        {
        public:
            explicit BindInAttrVec3Vop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeString("name", "Cd"));
            }
            std::string kind() const override { return "bind_in_attr_vec3"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addOutput(PortInfo::createOutput("out", DataType::Vec3));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.geometry || !ctx.graph) return;
                const std::string n = paramString("name", "");
                Vec3 v(0.0f);
                if (const auto *a = ctx.geometry->points().get<Vec3>(n))
                {
                    if (ctx.pointIndex < a->data().size()) v = a->data()[ctx.pointIndex];
                }
                ctx.graph->writeOutput(ctx, uid(), 0, v);
            }
        };

        // ── bind_out_attr_float ──────────────────────────────────────────────
        // prepare() ensures the float attribute exists once per cook before
        // the per-point loop. evaluate() writes the input value at pointIndex.
        class BindOutAttrFloatVop : public VopNode
        {
        public:
            explicit BindOutAttrFloatVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeString("name", "value"));
            }
            std::string kind() const override { return "bind_out_attr_float"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("in", DataType::Float));
                return io;
            }
            void prepare(Geometry &geo) const override
            {
                const std::string n = paramString("name", "");
                if (n.empty()) return;
                if (!geo.points().get<float>(n)) geo.points().add<float>(n, 0.0f);
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.geometry || !ctx.graph) return;
                const std::string n = paramString("name", "");
                if (n.empty()) return;
                auto in = ctx.graph->readInput(ctx, uid(), 0);
                if (!in) return;
                float v = 0.0f;
                if (auto *f = std::get_if<float>(&*in)) v = *f;
                else if (auto *p = std::get_if<Vec3>(&*in)) v = p->x;
                else if (auto *i = std::get_if<int>(&*in)) v = static_cast<float>(*i);
                if (auto *a = ctx.geometry->points().get<float>(n))
                {
                    if (ctx.pointIndex < a->data().size()) a->data()[ctx.pointIndex] = v;
                }
            }
        };

        // ── bind_out_attr_vec3 ───────────────────────────────────────────────
        class BindOutAttrVec3Vop : public VopNode
        {
        public:
            explicit BindOutAttrVec3Vop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeString("name", "Cd"));
            }
            std::string kind() const override { return "bind_out_attr_vec3"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addInput(PortInfo::createInput("in", DataType::Vec3));
                return io;
            }
            void prepare(Geometry &geo) const override
            {
                const std::string n = paramString("name", "");
                if (n.empty()) return;
                if (!geo.points().get<Vec3>(n)) geo.points().add<Vec3>(n, Vec3(0.0f));
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.geometry || !ctx.graph) return;
                const std::string n = paramString("name", "");
                if (n.empty()) return;
                auto in = ctx.graph->readInput(ctx, uid(), 0);
                if (!in) return;
                Vec3 v(0.0f);
                if (auto *p = std::get_if<Vec3>(&*in)) v = *p;
                else if (auto *f = std::get_if<float>(&*in)) v = Vec3(*f);
                else if (auto *i = std::get_if<int>(&*in)) v = Vec3(static_cast<float>(*i));
                if (auto *a = ctx.geometry->points().get<Vec3>(n))
                {
                    if (ctx.pointIndex < a->data().size()) a->data()[ctx.pointIndex] = v;
                }
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

        void registerBindAttrVops()
        {
            auto &reg = VopRegistry::instance();
            reg.registerType(
                {"bind_in_attr_float", "Bind In (Float)", "Bind",
                 /*inputs*/ {}, /*outputs*/ {{"out"}},
                 /*params*/ {{"name", ParamType::String, "\"Cd_x\""}}},
                makeFactory<BindInAttrFloatVop>());
            reg.registerType(
                {"bind_in_attr_vec3", "Bind In (Vec3)", "Bind",
                 /*inputs*/ {}, /*outputs*/ {{"out"}},
                 /*params*/ {{"name", ParamType::String, "\"Cd\""}}},
                makeFactory<BindInAttrVec3Vop>());
            reg.registerType(
                {"bind_out_attr_float", "Bind Out (Float)", "Bind",
                 /*inputs*/ {{"in"}}, /*outputs*/ {},
                 /*params*/ {{"name", ParamType::String, "\"value\""}}},
                makeFactory<BindOutAttrFloatVop>());
            reg.registerType(
                {"bind_out_attr_vec3", "Bind Out (Vec3)", "Bind",
                 /*inputs*/ {{"in"}}, /*outputs*/ {},
                 /*params*/ {{"name", ParamType::String, "\"Cd\""}}},
                makeFactory<BindOutAttrVec3Vop>());
        }
    }
}
