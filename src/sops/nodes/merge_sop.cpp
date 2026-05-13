#include "../sop_node.hpp"
#include "../sop_registry.hpp"
#include "../codegen/merge_compute.hpp"

#include "../../geometry/geometry.hpp"

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
                    // GPU fast path. Handles the common "two GPU-stage
                    // outputs concatenated" case — vkCmdCopyBuffer per
                    // attribute, no per-element CPU memcpy. The
                    // dispatcher silently returns false for unsupported
                    // scope (anything outside the standard P/N/uv/Cd
                    // set, non-identity vertexToPoint, primitive
                    // attributes) and we fall through. Skipped when no
                    // dispatcher is registered (headless smoke tests).
                    if (inputs.size() >= 2 && inputs[0] && inputs[1])
                    {
                        if (auto *gpu = codegen::MergeCompute::getGlobal())
                        {
                            Geometry out;
                            if (gpu->dispatch(*inputs[0], *inputs[1], out))
                            {
                                return out;
                            }
                        }
                    }

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
