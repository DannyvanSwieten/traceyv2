#include "../sop_node.hpp"
#include "../sop_registry.hpp"

namespace tracey
{
    namespace sops
    {
        namespace
        {
            // Two-input merge for v1. (Houdini's merge supports N inputs;
            // adding more here is a one-liner per slot, but two covers most
            // composition patterns and matches the simple-port-fan-in shape
            // the canvas already renders comfortably.)
            class MergeSop : public SopNode
            {
            public:
                explicit MergeSop(size_t uid) : SopNode(uid) {}

                std::string kind() const override { return "merge"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("a", DataType::Scene3D));
                    io.addInput(PortInfo::createInput("b", DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    Geometry out;
                    if (!inputs.empty() && inputs[0]) out = *inputs[0];
                    if (inputs.size() >= 2 && inputs[1])
                    {
                        // mergeFrom needs `out` to already declare any
                        // attributes it wants preserved from `b`. For v1 we
                        // just merge with whatever `out` already has; missing
                        // attributes from `b` are quietly dropped.
                        out.mergeFrom(*inputs[1]);
                    }
                    return out;
                }
            };
        }

        void registerMergeSop()
        {
            SopRegistry::instance().registerType(
                {"merge", "Merge", "Combiners",
                 /*inputs*/ {{"a"}, {"b"}}, /*outputs*/ {{"out"}},
                 /*params*/ {}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<MergeSop>(uid);
                });
        }
    }
}
