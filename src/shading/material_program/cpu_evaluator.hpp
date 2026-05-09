#pragma once
#include "material_program.hpp"

namespace tracey
{
    // Run a MaterialProgram on the CPU at a single shading point. Produces the
    // PBR attribute set; BRDF sampling is then a separate step on the result.
    //
    // The `parameters` slice supplies values for Op::LoadParam (the program's
    // animation surface). It must hold at least program.parameterCount entries
    // or LoadParam ops will throw out-of-range.
    //
    // This is the correctness oracle for the GPU VM: if outputs diverge between
    // CPU and GPU evaluation of the same program, the GPU implementation is wrong.
    //
    // Throws std::runtime_error on malformed programs (unknown opcode, register
    // or constant/parameter index out of range).
    MaterialEvalResult evaluateMaterialProgramCPU(const MaterialProgram &program,
                                                  const SurfaceData &surface,
                                                  const MaterialInputs &inputs = {},
                                                  const MaterialParameters &parameters = {});
}
