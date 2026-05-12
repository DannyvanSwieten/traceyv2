#include "../sop_graph.hpp"
#include "../sop_node.hpp"
#include "../sop_registry.hpp"

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // Houdini /obj-style light terminal. Lives in the SOP graph
            // alongside `object_output`; the cook recognises it by
            // kind() == "light" and synthesises an EmittedActor with the
            // `isLight` flag so apply_emitted attaches a Light component
            // instead of building a SceneInstance + material.
            //
            // No inputs / no outputs — lights are parameter-driven,
            // not part of the geometry flow DAG.
            class LightSop : public SopNode
            {
            public:
                explicit LightSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("name", "light"));
                    declareParam(Parameter::makeVec3("translate", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("rotate_euler_deg", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("scale", Vec3(1.0f)));
                    // 0 = point, 1 = distant. String would be more readable
                    // but the SOP param schema doesn't have an enum type; an
                    // int with a known mapping keeps the wire format simple.
                    declareParam(Parameter::makeInt("type", 0));
                    declareParam(Parameter::makeVec3("color", Vec3(1.0f, 1.0f, 1.0f)));
                    declareParam(Parameter::makeFloat("intensity", 1.0f));
                }

                std::string kind() const override { return "light"; }

                InputsAndOutputs ports() const override { return {}; }

                Geometry cook(std::span<const Geometry *const> /*inputs*/) const override
                {
                    // Terminal node — produces no geometry. The cook's
                    // top-level loop synthesises the EmittedActor from the
                    // params after this returns.
                    return Geometry{};
                }
            };
        }

        void registerLightSop()
        {
            SopRegistry::instance().registerType(
                {"light", "Light", "Lights",
                 /*inputs*/ {}, /*outputs*/ {},
                 /*params*/ {
                     {"name",             ParamType::String, "\"light\""},
                     {"translate",        ParamType::Vec3,   "[0, 0, 0]"},
                     {"rotate_euler_deg", ParamType::Vec3,   "[0, 0, 0]"},
                     {"scale",            ParamType::Vec3,   "[1, 1, 1]"},
                     {"type",             ParamType::Int,    "0"},
                     {"color",            ParamType::Vec3,   "[1, 1, 1]"},
                     {"intensity",        ParamType::Float,  "1.0"}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<LightSop>(uid);
                });
        }
    }
}
