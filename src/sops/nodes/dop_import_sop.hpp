#pragma once

// Helper accessor for the DopImportSop — the SOP that reads geometry out
// of a DOP graph each frame. EditorServer "stamps" the current frame's
// SimState geometry into the SOP via setDopImportGeometry() before
// posting a cook request; the SOP itself stays simple — its cook() just
// emits its stamped buffer.
//
// This shape mirrors attributeVopGraph()/setAttributeVopGraph() — the
// SOP class stays private to dop_import_sop.cpp; callers reach in via
// these free functions.

#include "../../geometry/geometry.hpp"

namespace tracey
{
    namespace sops
    {
        class SopNode;

        // Replace the SOP's stamped geometry. No-op if `node` isn't a
        // dop_import SOP. The next cook() emits this geometry as the
        // node's output.
        void setDopImportGeometry(SopNode *node, Geometry geo);

        // Read-only view of the currently-stamped geometry. Returns
        // nullptr when `node` isn't a dop_import SOP. Used by tests /
        // diagnostics — the cook reads this directly.
        const Geometry *dopImportGeometry(const SopNode *node);
    }
}
