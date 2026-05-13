#pragma once

// Helper accessors for the PopForceDop — mirrors src/sops/nodes/
// attribute_vop_sop.hpp. The class itself stays private to pop_force.cpp;
// EditorServer reaches in through these free functions instead of
// dynamic_cast'ing a fully-declared class.

#include <memory>

namespace tracey
{
    namespace vops { class VopGraph; }

    namespace dops
    {
        class DopNode;

        // Returns the host's child VopGraph if `node` is a pop_force DOP,
        // else nullptr. Lazily allocated on first access so the returned
        // pointer is always non-null when `node->kind() == "pop_force"`.
        vops::VopGraph *popForceVopGraph(DopNode *node);
        const vops::VopGraph *popForceVopGraph(const DopNode *node);

        // Replace the host's child VopGraph. No-op when `node` isn't a
        // pop_force. Used by the IPC `set_vop_graph` handler after
        // deserializing the edited subnet.
        void setPopForceVopGraph(DopNode *node, std::unique_ptr<vops::VopGraph> graph);
    }
}
