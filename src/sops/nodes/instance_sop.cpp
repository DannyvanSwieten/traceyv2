// Houdini-style "Instance" terminal SOP — Phase B of GPU instancing.
//
// Inputs:
//   0 — stamp:    the geometry to clone.
//   1 — template: a point cloud (only the point table is read).
//
// Unlike `copy_to_points`, `instance` doesn't flat-bake N stamp copies into
// one fat Geometry. It's a terminal (no output): SopGraph::cook detects
// `kind() == "instance"` and emits N EmittedActors per cook — each carrying
// the stamp's Geometry value unchanged plus its own per-point transform and
// `instanceIndex = i`. apply_emitted's Phase-A content-hash dedup then
// collapses all N actors onto ONE SceneObject + one BLAS, and the TLAS ends
// up with N instances pointing at that one BVH. That's true GPU instancing:
// memory is O(stamp_size + N * sizeof(transform)) instead of O(N * stamp_size).
//
// The class itself is a stub — emit logic lives in SopGraph::cook so the
// emission machinery (parent links, cook timing, cache integration) is
// shared with the other terminals (`object_output`, `light`).

#include "../sop_node.hpp"
#include "../sop_registry.hpp"

#include "../../geometry/geometry.hpp"

namespace tracey
{
    namespace sops
    {
        namespace
        {
            class InstanceSop : public SopNode
            {
            public:
                explicit InstanceSop(size_t uid) : SopNode(uid)
                {
                    declareParam(Parameter::makeString("name", "instance"));
                    declareParam(Parameter::makeBool("orient_to_normal", true));
                    declareParam(Parameter::makeString("material_library_name", ""));
                }

                std::string kind() const override { return "instance"; }

                InputsAndOutputs ports() const override
                {
                    InputsAndOutputs io;
                    io.addInput(PortInfo::createInput("stamp",    DataType::Scene3D));
                    io.addInput(PortInfo::createInput("template", DataType::Scene3D));
                    return io;
                }

                Geometry cook(std::span<const Geometry *const>) const override
                {
                    // Terminal: emit happens in SopGraph::cook via the
                    // `kind() == "instance"` branch, not via the returned
                    // Geometry. Returning empty so any accidental downstream
                    // wiring sees a noop.
                    return {};
                }
            };
        }

        void registerInstanceSop()
        {
            SopRegistry::instance().registerType(
                {"instance", "Instance", "Cloners",
                 /*inputs*/ {{"stamp"}, {"template"}},
                 /*outputs*/ {},
                 /*params*/ {
                     {"name",                  ParamType::String, "\"instance\""},
                     {"orient_to_normal",      ParamType::Bool,   "true"},
                     {"material_library_name", ParamType::String, "\"\""}}},
                [](size_t uid) -> std::unique_ptr<SopNode> {
                    return std::make_unique<InstanceSop>(uid);
                });
        }
    }
}
