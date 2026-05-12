#pragma once

// Helper accessors for the AttributeVopSop host SOP. The class itself stays
// private to attribute_vop_sop.cpp; callers (editor_server, smoke tests)
// reach in through these free functions instead of dynamic_cast'ing a
// fully-declared class.

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
        // attribute_vop SOP, else nullptr. The graph is lazily allocated on
        // first access (an empty AttributeVopSop has none yet) so the
        // returned pointer is always non-null when `node->kind() ==
        // "attribute_vop"`.
        vops::VopGraph *attributeVopGraph(SopNode *node);
        const vops::VopGraph *attributeVopGraph(const SopNode *node);

        // Replace the host's child VopGraph with `graph`. No-op if `node`
        // isn't an attribute_vop SOP. Used by the editor's `set_vop_graph`
        // IPC handler to install a freshly-deserialized graph.
        void setAttributeVopGraph(SopNode *node, std::unique_ptr<vops::VopGraph> graph);

        // A "promoted" VOP parameter: a VOP node's parameter (vopParamName on
        // node `vopNodeUid` inside the host's VopGraph) is exposed as a
        // first-class SOP parameter named `hostParamName` on the host. The
        // host param becomes the source of truth — animatable through the
        // standard keyframe/dopesheet path — and the cook stamps the
        // host-sampled value back into the inner VOP node each tick.
        struct VopPromotion
        {
            size_t      vopNodeUid = 0;
            std::string vopParamName;
            std::string hostParamName;
            ParamType   paramType = ParamType::Float;
            // UI hints copied from the VOP-side ParamSpec at promotion
            // time. Without these, the host SOP inspector renders the
            // promoted param as a bare number input even when the inner
            // VOP param had a range / options. Default to "no hint"
            // (rangeMin == rangeMax) so older promotions stay valid.
            double rangeMin  = 0.0;
            double rangeMax  = 0.0;
            double rangeStep = 0.0;
            std::vector<std::string> options;
        };

        // Read the live list of promotions on an attribute_vop host. Empty
        // when `node` isn't an attribute_vop. The editor uses this to
        // round-trip the UI state and to populate the SOP inspector with the
        // dynamic promoted params (since the catalog only knows the static
        // registry params, which for attribute_vop is empty).
        const std::vector<VopPromotion> *attributeVopPromotions(const SopNode *node);

        // Promote a VOP-side parameter to the host SOP. Returns the new host
        // param name on success (auto-generated, unique within the host's
        // param table), or empty string on failure (host isn't an
        // attribute_vop, no matching VOP node / param, etc.). Channels on the
        // new host param start empty; the user adds keyframes via the
        // existing SOP keyframe IPCs.
        std::string promoteAttributeVopParam(SopNode *node,
                                             size_t vopNodeUid,
                                             const std::string &vopParamName);

        // Remove a promotion + the matching host param. Channels on that
        // host param are discarded. Returns true on success.
        bool demoteAttributeVopParam(SopNode *node, const std::string &hostParamName);

        // Walk this host's promotions and copy each VOP-side param value into
        // the matching host SOP parameter's CONSTANT baseline (channels are
        // preserved — animation keys still own each frame's value at cook
        // time). Used by `set_vop_graph` after the graph swap so an edit to
        // a promoted param's number box in the VOP inspector takes effect:
        // without this, cookAt's stampPromotedParams overwrites the user's
        // new VOP-side value with the host's stale baseline. No-op if the
        // node isn't an attribute_vop.
        void syncPromotedHostValuesFromVop(SopNode *node);
    }
}
