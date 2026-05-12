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
        //
        // createIfMissing() controls the prepare() behaviour: when true (the
        // default, used by the generic bind_out_attr_* nodes), prepare adds
        // a zero-initialised attribute if missing — useful when the user
        // wants to introduce a brand-new attribute. When false (overridden
        // by the preset Houdini-standard subclasses below), prepare leaves
        // the geometry alone, so a default seeded passthrough that wires
        // bind_in_X to bind_out_X for an attribute the input geometry
        // doesn't carry stays as a no-op instead of zero-stamping the
        // attribute. The latter previously broke shading (e.g. Cd=0 made
        // the rasterizer render the sphere black) the moment any edit
        // forced the inner VOP graph to re-cook.
        class BindOutAttrFloatVop : public VopNode
        {
        public:
            explicit BindOutAttrFloatVop(size_t uid) : VopNode(uid)
            {
                declareParam(Parameter::makeString("name", "value"));
            }
            std::string kind() const override { return "bind_out_attr_float"; }
            virtual bool createIfMissing() const { return true; }
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
                if (createIfMissing() && !geo.points().get<float>(n))
                    geo.points().add<float>(n, 0.0f);
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
                // Skip silently if the attribute isn't present (preset
                // passthrough on geometry that doesn't carry it).
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
            virtual bool createIfMissing() const { return true; }
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
                if (createIfMissing() && !geo.points().get<Vec3>(n))
                    geo.points().add<Vec3>(n, Vec3(0.0f));
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

        // ── Preset attribute bindings ────────────────────────────────────────
        // Houdini's Geometry VOP exposes the standard point attributes (N,
        // Cd, Alpha, uv, v, pscale) as fixed slots on its Input/Output
        // nodes. Tracey's per-bind-node model can't do dynamic ports, but
        // we get the same UX by providing dedicated nodes per attribute
        // whose `name` param is baked into the class. Subclass the generic
        // bind nodes so the evaluation logic stays in one place; just
        // re-declare the `name` param (declareParam replaces) and override
        // kind().

        // Input preset macro: subclass the generic bind_in_attr_* and bake
        // in the canonical attribute name. No createIfMissing override —
        // input nodes don't write back, so they can't cause the
        // zero-stamp regression.
#define TRACEY_PRESET_BIND_IN(ClassName, KindStr, AttrName, Base)               \
        class ClassName : public Base                                           \
        {                                                                       \
        public:                                                                 \
            explicit ClassName(size_t uid) : Base(uid)                          \
            {                                                                   \
                declareParam(Parameter::makeString("name", AttrName));          \
            }                                                                   \
            std::string kind() const override { return KindStr; }               \
        };
        // Output preset macro: same as input, plus createIfMissing()
        // returns false so a wired-but-not-modified passthrough on
        // geometry that doesn't carry this attribute is a clean no-op
        // (rather than inserting a zero-stamped attribute that downstream
        // consumers like the rasterizer would render as black).
#define TRACEY_PRESET_BIND_OUT(ClassName, KindStr, AttrName, Base)              \
        class ClassName : public Base                                           \
        {                                                                       \
        public:                                                                 \
            explicit ClassName(size_t uid) : Base(uid)                          \
            {                                                                   \
                declareParam(Parameter::makeString("name", AttrName));          \
            }                                                                   \
            std::string kind() const override { return KindStr; }               \
            bool createIfMissing() const override { return false; }             \
        };

        // Vec3 input presets
        TRACEY_PRESET_BIND_IN(BindInNVop,  "bind_in_N",  "N",  BindInAttrVec3Vop)
        TRACEY_PRESET_BIND_IN(BindInCdVop, "bind_in_Cd", "Cd", BindInAttrVec3Vop)
        TRACEY_PRESET_BIND_IN(BindInUvVop, "bind_in_uv", "uv", BindInAttrVec3Vop)
        TRACEY_PRESET_BIND_IN(BindInVVop,  "bind_in_v",  "v",  BindInAttrVec3Vop)
        // Float input presets
        TRACEY_PRESET_BIND_IN(BindInAlphaVop,  "bind_in_Alpha",  "Alpha",  BindInAttrFloatVop)
        TRACEY_PRESET_BIND_IN(BindInPscaleVop, "bind_in_pscale", "pscale", BindInAttrFloatVop)
        // Vec3 output presets
        TRACEY_PRESET_BIND_OUT(BindOutNVop,  "bind_out_N",  "N",  BindOutAttrVec3Vop)
        TRACEY_PRESET_BIND_OUT(BindOutCdVop, "bind_out_Cd", "Cd", BindOutAttrVec3Vop)
        TRACEY_PRESET_BIND_OUT(BindOutUvVop, "bind_out_uv", "uv", BindOutAttrVec3Vop)
        TRACEY_PRESET_BIND_OUT(BindOutVVop,  "bind_out_v",  "v",  BindOutAttrVec3Vop)
        // Float output presets
        TRACEY_PRESET_BIND_OUT(BindOutAlphaVop,  "bind_out_Alpha",  "Alpha",  BindOutAttrFloatVop)
        TRACEY_PRESET_BIND_OUT(BindOutPscaleVop, "bind_out_pscale", "pscale", BindOutAttrFloatVop)
#undef TRACEY_PRESET_BIND_IN
#undef TRACEY_PRESET_BIND_OUT

        // ── bind_in_ptnum ────────────────────────────────────────────────────
        // Special case: there's no `ptnum` attribute on geometry. The value
        // comes from EvalContext::pointIndex itself, so this node has no
        // attribute lookup and just forwards the loop counter.
        class BindInPtNumVop : public VopNode
        {
        public:
            explicit BindInPtNumVop(size_t uid) : VopNode(uid) {}
            std::string kind() const override { return "bind_in_ptnum"; }
            InputsAndOutputs ports() const override
            {
                InputsAndOutputs io;
                io.addOutput(PortInfo::createOutput("out", DataType::Int));
                return io;
            }
            void evaluate(EvalContext &ctx) const override
            {
                if (!ctx.graph) return;
                ctx.graph->writeOutput(ctx, uid(), 0, static_cast<int>(ctx.pointIndex));
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
            // See bind_vops.cpp for the category-split rationale. The "name"
            // param picks which named attribute the node reads/writes — so
            // users add one node per attribute, name it, and wire it.
            auto &reg = VopRegistry::instance();

            // Generic (user-named) bindings. Still useful for non-standard
            // attribute names — the presets below cover the Houdini-standard
            // set, but anything custom (e.g. "Cdtemp") uses these.
            reg.registerType(
                {"bind_in_attr_float", "Bind In (Float)", "Input Attributes",
                 /*inputs*/ {}, /*outputs*/ {{"out"}},
                 /*params*/ {{"name", ParamType::String, "\"Cd_x\""}}},
                makeFactory<BindInAttrFloatVop>());
            reg.registerType(
                {"bind_in_attr_vec3", "Bind In (Vec3)", "Input Attributes",
                 /*inputs*/ {}, /*outputs*/ {{"out"}},
                 /*params*/ {{"name", ParamType::String, "\"Cd\""}}},
                makeFactory<BindInAttrVec3Vop>());
            reg.registerType(
                {"bind_out_attr_float", "Bind Out (Float)", "Output Attributes",
                 /*inputs*/ {{"in"}}, /*outputs*/ {},
                 /*params*/ {{"name", ParamType::String, "\"value\""}}},
                makeFactory<BindOutAttrFloatVop>());
            reg.registerType(
                {"bind_out_attr_vec3", "Bind Out (Vec3)", "Output Attributes",
                 /*inputs*/ {{"in"}}, /*outputs*/ {},
                 /*params*/ {{"name", ParamType::String, "\"Cd\""}}},
                makeFactory<BindOutAttrVec3Vop>());

            // Houdini-standard input attribute presets. No `params` field on
            // the catalog entries — the name is fixed by the class — so the
            // inspector doesn't show an editable name field for these.
            reg.registerType({"bind_in_N",      "Normal (N)",        "Input Attributes",
                             /*inputs*/ {}, /*outputs*/ {{"out"}}, /*params*/ {}},
                             makeFactory<BindInNVop>());
            reg.registerType({"bind_in_Cd",     "Color (Cd)",        "Input Attributes",
                             /*inputs*/ {}, /*outputs*/ {{"out"}}, /*params*/ {}},
                             makeFactory<BindInCdVop>());
            reg.registerType({"bind_in_Alpha",  "Alpha",             "Input Attributes",
                             /*inputs*/ {}, /*outputs*/ {{"out"}}, /*params*/ {}},
                             makeFactory<BindInAlphaVop>());
            reg.registerType({"bind_in_uv",     "UV (uv)",           "Input Attributes",
                             /*inputs*/ {}, /*outputs*/ {{"out"}}, /*params*/ {}},
                             makeFactory<BindInUvVop>());
            reg.registerType({"bind_in_v",      "Velocity (v)",      "Input Attributes",
                             /*inputs*/ {}, /*outputs*/ {{"out"}}, /*params*/ {}},
                             makeFactory<BindInVVop>());
            reg.registerType({"bind_in_pscale", "Point Scale (pscale)", "Input Attributes",
                             /*inputs*/ {}, /*outputs*/ {{"out"}}, /*params*/ {}},
                             makeFactory<BindInPscaleVop>());
            reg.registerType({"bind_in_ptnum",  "Point Number (ptnum)", "Input Attributes",
                             /*inputs*/ {}, /*outputs*/ {{"out"}}, /*params*/ {}},
                             makeFactory<BindInPtNumVop>());

            // Houdini-standard output attribute presets.
            reg.registerType({"bind_out_N",      "Normal (N)",       "Output Attributes",
                             /*inputs*/ {{"in"}}, /*outputs*/ {}, /*params*/ {}},
                             makeFactory<BindOutNVop>());
            reg.registerType({"bind_out_Cd",     "Color (Cd)",       "Output Attributes",
                             /*inputs*/ {{"in"}}, /*outputs*/ {}, /*params*/ {}},
                             makeFactory<BindOutCdVop>());
            reg.registerType({"bind_out_Alpha",  "Alpha",            "Output Attributes",
                             /*inputs*/ {{"in"}}, /*outputs*/ {}, /*params*/ {}},
                             makeFactory<BindOutAlphaVop>());
            reg.registerType({"bind_out_uv",     "UV (uv)",          "Output Attributes",
                             /*inputs*/ {{"in"}}, /*outputs*/ {}, /*params*/ {}},
                             makeFactory<BindOutUvVop>());
            reg.registerType({"bind_out_v",      "Velocity (v)",     "Output Attributes",
                             /*inputs*/ {{"in"}}, /*outputs*/ {}, /*params*/ {}},
                             makeFactory<BindOutVVop>());
            reg.registerType({"bind_out_pscale", "Point Scale (pscale)", "Output Attributes",
                             /*inputs*/ {{"in"}}, /*outputs*/ {}, /*params*/ {}},
                             makeFactory<BindOutPscaleVop>());
        }
    }
}
