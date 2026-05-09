#include "../sop_node.hpp"
#include "../sop_registry.hpp"

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // Terminal node. The graph-side cook() detects nodes whose kind()
            // is "object_output" and synthesises an EmittedActor from this
            // node's parameters + its first input. cook() itself just
            // forwards the input geometry so downstream caching works
            // uniformly (the graph will store result[uid] = inputGeometry).
            class ObjectOutputSop : public SopNode
            {
            public:
                explicit ObjectOutputSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("name", "actor"));
                    declareParam(Parameter::makeVec3("translate", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("rotate_euler_deg", Vec3(0.0f)));
                    declareParam(Parameter::makeVec3("scale", Vec3(1.0f)));
                    declareParam(Parameter::makeString("material_library_name", ""));
                }

                std::string kind() const override { return "object_output"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("in", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    if (!inputs.empty() && inputs[0]) return *inputs[0];
                    return Geometry{};
                }
            };
        }

        void registerObjectOutputSop()
        {
            SopRegistry::instance().registerType(
                {"object_output", "Object Output", "Output",
                 /*inputs*/ {{"in"}}, /*outputs*/ {},
                 /*params*/ {
                     {"name",                  ParamType::String, "\"actor\""},
                     {"translate",             ParamType::Vec3,   "[0, 0, 0]"},
                     {"rotate_euler_deg",      ParamType::Vec3,   "[0, 0, 0]"},
                     {"scale",                 ParamType::Vec3,   "[1, 1, 1]"},
                     {"material_library_name", ParamType::String, "\"\""}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<ObjectOutputSop>(uid);
                });
        }
    }
}
