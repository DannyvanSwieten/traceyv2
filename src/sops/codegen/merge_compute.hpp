#pragma once

// GPU dispatcher for the merge SOP's attribute-concat pass.
//
// Merge is structurally O(Na + Nb) data movement: take input[0]'s
// attributes, allocate room for input[1]'s, copy both into the
// destination. The CPU implementation goes through Geometry's
// operator=  + mergeFrom, both of which run per-attribute memcpys
// across the CPU vectors. For large attributes that already live in
// GPU buffers (after a copy_to_points / VOP / transform stage), this
// forces a download to clone, then a download to mergeFrom — and a
// re-upload at the next GPU stage.
//
// This dispatcher concatenates input[0] + input[1] directly through
// vkCmdCopyBuffer calls between Attribute<T> GPU buffers — no shader
// involved, just two DMA copies per attribute. Output stays
// GPU-resident.
//
// Scope (v1):
//   • Both inputs' point attrs ⊆ {P, N}
//   • Both inputs' vertex attrs ⊆ {uv, Cd}
//   • Both inputs have no primitive attribute payloads
//   • Both inputs have identity vertexToPoint
//   • Topology (vertex / primitive lists) is built CPU-side; only the
//     attribute payloads use the GPU path
//
// Anything outside the envelope returns false → CPU mergeFrom.

#include <cstdint>

namespace tracey
{
    class Device;
    class Geometry;

    namespace sops
    {
        namespace codegen
        {
            class MergeCompute
            {
            public:
                explicit MergeCompute(Device *device);
                ~MergeCompute();

                MergeCompute(const MergeCompute &) = delete;
                MergeCompute &operator=(const MergeCompute &) = delete;

                // Concat `a` and `b` into `out` (`out` is reset to empty
                // before population). Returns true when the GPU fast path
                // ran end-to-end, false on scope mismatch / GPU failure —
                // caller should fall back to the CPU merge path. Never
                // throws.
                bool dispatch(const Geometry &a, const Geometry &b,
                              Geometry &out) noexcept;

                static MergeCompute *getGlobal();
                static void setGlobal(MergeCompute *dispatcher);

            private:
                struct Impl;
                Impl *m_impl;
            };
        }
    }
}
