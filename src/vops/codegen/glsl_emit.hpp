#pragma once

// VopGraph → GLSL compute shader code generator.
//
// Walks a (compiled) VopGraph in topo order and emits a single GLSL
// `compute` shader whose main() processes one point per invocation.
// Each VOP node turns into a few lines of GLSL operating on locally-
// named slot variables; the geo_input / geo_output terminals read /
// write SSBO-backed point attributes.
//
// Phase 1 scope: emit only — no GPU dispatch. The caller compiles the
// returned source via tracey::ShaderCompiler and inspects the
// binding tables to set up SSBOs + uniform/param buffer. Phase 2
// (dispatch) hooks attribute_vop_sop and pop_force to this.
//
// Parameter strategy: every VOP node parameter packs into one vec4 of
// a single std430 SSBO bound after the attribute buffers. The CPU
// side rewrites that buffer per cook (param edits don't recompile
// the pipeline); only structural graph edits change the GLSL itself.
//
// The emitted shader assumes:
//   • One vec3 SSBO per touched point attribute (geo_input / geo_output ports).
//   • One SSBO holding the param vec4 array (placed after the attrs).
//   • A push-constant block with `pointCount` so the kernel can bail
//     past the array end (workgroup count rounds up).

#include "../vop_graph.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tracey
{
    namespace vops
    {
        namespace codegen
        {
            // Wire/attribute types the emitter understands. Maps directly
            // to GLSL `float`, `vec3`, `int`. Bool / string / vec2/vec4
            // are not (yet) emittable — node kinds that need them are
            // skipped by emitGlsl (see `unsupported`).
            enum class GpuType { Float, Vec3, Int };

            // One named point-attribute the kernel touches. Each gets its
            // own std430 SSBO at `binding`. `read`/`write` reflect whether
            // any port on geo_input / geo_output referenced it — the
            // dispatcher uses this to skip readback for write-only attrs.
            struct AttrBinding
            {
                std::string name;     // e.g. "P", "v", "force", "Cd"
                GpuType type;         // Vec3 for most, Float for age/life/pscale
                uint32_t binding;     // SSBO descriptor binding index
                bool read = false;    // referenced by a geo_input output port
                bool write = false;   // referenced by a geo_output input port
            };

            // One VOP-node parameter packed into the param SSBO. Each
            // param consumes one vec4 slot — float in .x, int in .x
            // (as float), vec3 in .xyz. The CPU side writes these vec4s
            // per cook from the live VopNode's `parameters()`.
            struct ParamSlot
            {
                size_t nodeUid = 0;
                std::string paramName;
                GpuType type;
                uint32_t slot = 0;   // index into params.data[]
            };

            struct EmitResult
            {
                // The full GLSL source — pass to ShaderCompiler::compileComputeShader.
                std::string glsl;

                // Tables the dispatcher uses to set up SSBOs + uploads.
                std::vector<AttrBinding> attrs;
                std::vector<ParamSlot> params;

                // SSBO binding index for the param buffer. Always assigned
                // AFTER all attribute bindings so attribute binding indices
                // form a contiguous prefix [0..attrs.size()).
                uint32_t paramsBinding = 0;
                // Total vec4 slots in the param SSBO. Zero when no node
                // declared any parameter the emitter could pack.
                uint32_t paramSlotCount = 0;

                // local_size_x baked into the shader. The dispatcher rounds
                // the workgroup count up: ceil(pointCount / localSizeX).
                uint32_t localSizeX = 64;

                // Node kinds the emitter doesn't know how to translate.
                // Non-empty means the graph contains at least one node
                // that would silently no-op on the GPU — surface this to
                // the user (or fall back to the CPU evaluator).
                std::vector<std::string> unsupported;
            };

            // Emit a compute shader for the given graph. Calls
            // graph.compile() internally so the topo order + slot table
            // are guaranteed up to date. The graph is treated as const.
            //
            // Throws nothing; on unsupported nodes the emitter records
            // them in `result.unsupported` and emits a comment in their
            // place so the rest of the kernel still compiles.
            EmitResult emitGlsl(const VopGraph &graph);

            // 64-bit FNV-1a of the emitted GLSL. The dispatcher caches
            // compiled compute pipelines by this hash — graph edits that
            // shift only param values keep the same hash (params are in
            // the SSBO, not the source).
            uint64_t hashGlsl(const std::string &source);
        }
    }
}
