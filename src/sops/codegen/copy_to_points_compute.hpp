#pragma once

// GPU dispatcher for the copy_to_points SOP.
//
// Houdini-style "copy_to_points" is structurally O(N·M) — for each of the
// N template points, every one of the M stamp vertices gets transformed
// (translate + per-point rotate + per-point scale) and concatenated into
// the output Geometry. The CPU implementation in copy_to_points_sop.cpp
// drives this through a Geometry deep-copy per template point and three
// inner-loop vector transforms; the GPU port collapses the whole thing
// into a single compute dispatch with N·M threads.
//
// Scope (v1):
//   • stamp.points() must carry exactly {P} or {P, N}, nothing else
//   • stamp.vertices() must carry exactly {} or {uv}, nothing else
//   • stamp.primitives() must have no attribute payload (the primitive
//     list itself is preserved; per-prim attrs would need a separate
//     broadcast and aren't supported yet)
//   • stamp.pointCount() == stamp.vertexCount() == vertexToPoint identity
//     (i.e. fromSceneObject-style per-corner-unique geometry — the
//     common case for primitive_cube / primitive_sphere / glTF imports)
//   • template's pscale / N / Cd are honoured if present; other template
//     attrs are ignored (same as the CPU path)
//
// Anything outside that scope returns false → caller falls back to CPU.
//
// Output Geometry on success:
//   • points: P (always), N (iff stamp had N) — both GPU-resident, side=Gpu
//   • vertices: uv (iff stamp had uv), Cd (iff template had Cd) — same
//   • primitive list + vertexToPoint built CPU-side (cheap integer ops)

#include <cstdint>

namespace tracey
{
    class Device;
    class Geometry;

    namespace sops
    {
        namespace codegen
        {
            class CopyToPointsCompute
            {
            public:
                explicit CopyToPointsCompute(Device *device);
                ~CopyToPointsCompute();

                CopyToPointsCompute(const CopyToPointsCompute &) = delete;
                CopyToPointsCompute &operator=(const CopyToPointsCompute &) = delete;

                // Try to dispatch on GPU. Returns true on success (out is
                // populated and GPU-resident), false on scope mismatch or
                // any Vulkan / shaderc error — caller should fall back to
                // the CPU path. Never throws.
                bool dispatch(const Geometry &stamp, const Geometry &tmpl,
                              bool orient_to_normal, Geometry &out) noexcept;

                // ── Process-wide singleton ──
                // Same setGlobal / getGlobal pattern as VopComputeDispatcher:
                // EditorServer registers a dispatcher at startup, the SOP
                // cook calls getGlobal() and silently falls back to CPU
                // when null (headless smoke tests, no compute device).
                static CopyToPointsCompute *getGlobal();
                static void setGlobal(CopyToPointsCompute *dispatcher);

            private:
                struct Impl;
                Impl *m_impl;
            };
        }
    }
}
