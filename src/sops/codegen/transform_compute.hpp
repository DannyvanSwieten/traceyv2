#pragma once

// GPU dispatcher for the transform SOP's per-point SRT pass.
//
// The CPU implementation loops over every P (and N if present) applying
// `R * (p * s) + t` for positions and `normalize(R * n)` for normals.
// For particle / instance geometry that grew through a GPU stage
// (copy_to_points / VOP / future GPU merge) the data is already living
// on the GPU as an Attribute<Vec3> SSBO — this dispatcher transforms
// it in place there instead of round-tripping through the CPU loop.
//
// Each Attribute<Vec3> is mutated in place. The dispatcher honours the
// usual lazy-sync contract: `Attribute<T>::buffer()` is called for
// write access, which uploads any pending CPU data, runs the kernel
// against the SSBO, and leaves side=Gpu + generation bumped so the
// next consumer reads from GPU directly.
//
// Scope: only Vec3 point/vertex attributes. Strings / matrices /
// floats are out of scope (no transform meaning, or different
// element types).
//
// Failure handling: any GPU/shader error returns false — caller falls
// back to CPU and continues. Never throws.

#include "../../core/types.hpp"

namespace tracey
{
    class Device;
    template <typename T> class Attribute;

    namespace sops
    {
        namespace codegen
        {
            class TransformCompute
            {
            public:
                explicit TransformCompute(Device *device);
                ~TransformCompute();

                TransformCompute(const TransformCompute &) = delete;
                TransformCompute &operator=(const TransformCompute &) = delete;

                // Mode controls how the per-element vec3 is interpreted:
                //   Position — affine, `M * vec4(v, 1)` (full SRT).
                //   Normal   — linear,  `M * vec4(v, 0)`, then renormalised.
                //              Caller is expected to pass a pure-rotation
                //              matrix (no scale) for parity with the CPU
                //              path's "ignore non-uniform scale" behaviour.
                enum class Mode { Position, Normal };

                // Transform `attr` in place. `m` is a column-major 4x4.
                // Returns true on success (attr is GPU-resident with the
                // new values), false on scope / GPU failure.
                bool dispatch(Attribute<Vec3> &attr,
                              const float m[16],
                              Mode mode) noexcept;

                static TransformCompute *getGlobal();
                static void setGlobal(TransformCompute *dispatcher);

            private:
                struct Impl;
                Impl *m_impl;
            };
        }
    }
}
