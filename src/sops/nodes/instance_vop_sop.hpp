#pragma once

// Helper accessors for the InstanceVopSop host SOP. The class itself stays
// private to instance_vop_sop.cpp; callers (editor_server, smoke tests,
// the sop_graph emit loop) reach in through these free functions instead
// of dynamic_cast'ing a fully-declared class.
//
// instance_vop is a terminal SOP: like its sibling `instance`, it doesn't
// return a Geometry — it emits one EmittedActor with N InstanceEntries
// straight into the cook's emit list. The wrapping VopGraph runs once
// per template point and writes per-instance transform + tint by
// mutating P / N / pscale / Cd on a synthetic per-instance Geometry that
// the dispatcher consumes through the normal VOP path.

#include "../parameter.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tracey
{
    namespace sops { class SopNode; }
    namespace vops { class VopGraph; }

    namespace sops
    {
        // Returns a pointer to the host's child VopGraph if `node` is an
        // instance_vop SOP, else nullptr. The graph is lazily allocated on
        // first access (a brand-new InstanceVopSop has none yet).
        vops::VopGraph *instanceVopGraph(SopNode *node);
        const vops::VopGraph *instanceVopGraph(const SopNode *node);

        // Replace the host's child VopGraph with `graph`. No-op when `node`
        // isn't an instance_vop. Used by the editor's `set_vop_graph` IPC
        // to install a freshly-deserialized graph after a frontend edit.
        void setInstanceVopGraph(SopNode *node, std::unique_ptr<vops::VopGraph> graph);

        // Promotion records — same shape as the attribute_vop ones. Reused
        // verbatim so the editor's promote/demote IPCs can target both
        // host types through the same code path.
        struct InstanceVopPromotion
        {
            size_t      vopNodeUid = 0;
            std::string vopParamName;
            std::string hostParamName;
            ParamType   paramType = ParamType::Float;
            double rangeMin  = 0.0;
            double rangeMax  = 0.0;
            double rangeStep = 0.0;
            std::vector<std::string> options;
        };

        const std::vector<InstanceVopPromotion> *instanceVopPromotions(const SopNode *node);
        std::string promoteInstanceVopParam(SopNode *node,
                                            size_t vopNodeUid,
                                            const std::string &vopParamName);
        bool demoteInstanceVopParam(SopNode *node, const std::string &hostParamName);
        void syncPromotedInstanceVopValues(SopNode *node);
    }
}
