// SwitchSop — pick one of N inputs by an integer index. The classic
// "if/else in a graph" — handy for A/B comparisons during scene editing,
// for animated variants (the index can be promoted into an attribute_vop
// or driven by a Parameter node on a parent), and as a flexible debug-viz
// toggle (wire a hide-points / show-points pair through it).
//
// Inputs are named `in0`…`in3`. Out-of-range indices clamp to the
// available inputs, so a graph that's only wired up to `in0` and `in1`
// with select=2 produces `in1`'s output rather than an empty geometry —
// matches Houdini's clamp-to-last behaviour.

#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include "../../geometry/geometry.hpp"

#include <algorithm>
#include <string>

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class SwitchSop : public SopNode
            {
            public:
                explicit SwitchSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeInt("select", 0));
                }
                std::string kind() const override { return "switch"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput (PortInfo::createInput ("in0", DataType::Scene3D));
                    io.addInput (PortInfo::createInput ("in1", DataType::Scene3D));
                    io.addInput (PortInfo::createInput ("in2", DataType::Scene3D));
                    io.addInput (PortInfo::createInput ("in3", DataType::Scene3D));
                    io.addOutput(PortInfo::createOutput("out", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const> inputs) const override
                {
                    if (inputs.empty()) return Geometry{};
                    // Clamp to the highest input that's actually wired up.
                    // The cook framework hands us nulls for unconnected
                    // slots, so the "max connected index" we'll honour is
                    // determined by walking from the end.
                    int maxConnected = -1;
                    for (int i = static_cast<int>(inputs.size()) - 1; i >= 0; --i)
                    {
                        if (inputs[i] != nullptr) { maxConnected = i; break; }
                    }
                    if (maxConnected < 0) return Geometry{};

                    int idx = paramInt("select", 0);
                    if (idx < 0) idx = 0;
                    if (idx > maxConnected) idx = maxConnected;
                    if (!inputs[idx])
                    {
                        // Selected slot is empty but a later one isn't —
                        // fall forward to the next connected input. This
                        // matches "select=2 with only in0+in3 wired"
                        // staying useful instead of producing nothing.
                        for (int i = idx + 1; i <= maxConnected; ++i)
                            if (inputs[i]) { idx = i; break; }
                    }
                    if (!inputs[idx]) return Geometry{};
                    return *inputs[idx];
                }
            };
        }  // anon

        void registerSwitchSop()
        {
            SopRegistry::instance().registerType(
                {"switch", "Switch", "Modifiers",
                 /*inputs*/  {{"in0"}, {"in1"}, {"in2"}, {"in3"}},
                 /*outputs*/ {{"out"}},
                 /*params*/  {{"select", ParamType::Int, "0"}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<SwitchSop>(uid);
                });
        }
    }
}
