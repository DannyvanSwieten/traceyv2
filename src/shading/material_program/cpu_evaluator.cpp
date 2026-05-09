#include "cpu_evaluator.hpp"
#include <array>
#include <stdexcept>

namespace tracey
{
    MaterialEvalResult evaluateMaterialProgramCPU(const MaterialProgram &program,
                                                  const SurfaceData &surface,
                                                  const MaterialInputs &inputs,
                                                  const MaterialParameters &parameters)
    {
        std::array<Vec4, kMaxRegisters> r{};
        MaterialEvalResult result{};

        auto reg = [&](uint16_t idx) -> Vec4 & {
            if (idx >= kMaxRegisters)
            {
                throw std::runtime_error("MaterialProgram: register index out of range");
            }
            return r[idx];
        };

        for (const Instruction &inst : program.code)
        {
            const Op op = static_cast<Op>(inst.op);
            switch (op)
            {
            case Op::Halt:
                return result;

            case Op::LoadConst:
            {
                if (inst.imm >= program.constants.size())
                {
                    throw std::runtime_error("MaterialProgram: constant index out of range");
                }
                reg(inst.dst) = program.constants[inst.imm];
                break;
            }

            case Op::LoadPosition: reg(inst.dst) = Vec4(surface.worldPosition, 0.0f); break;
            case Op::LoadNormal:   reg(inst.dst) = Vec4(surface.worldNormal, 0.0f);   break;
            case Op::LoadTangent:  reg(inst.dst) = Vec4(surface.worldTangent, 0.0f);  break;
            case Op::LoadViewDir:  reg(inst.dst) = Vec4(surface.viewDir, 0.0f);       break;
            case Op::LoadUV0:      reg(inst.dst) = Vec4(surface.uv0, 0.0f, 0.0f);     break;
            case Op::LoadUV1:      reg(inst.dst) = Vec4(surface.uv1, 0.0f, 0.0f);     break;

            case Op::LoadInputAlbedo:    reg(inst.dst) = Vec4(inputs.albedo, 0.0f);   break;
            case Op::LoadInputMetallic:  reg(inst.dst) = Vec4(inputs.metallic);       break;
            case Op::LoadInputRoughness: reg(inst.dst) = Vec4(inputs.roughness);      break;
            case Op::LoadInputEmission:  reg(inst.dst) = Vec4(inputs.emission, 0.0f); break;
            case Op::LoadInputNormal:    reg(inst.dst) = Vec4(inputs.normal, 0.0f);   break;

            case Op::Add: reg(inst.dst) = reg(inst.srcA) + reg(inst.srcB); break;
            case Op::Sub: reg(inst.dst) = reg(inst.srcA) - reg(inst.srcB); break;
            case Op::Mul: reg(inst.dst) = reg(inst.srcA) * reg(inst.srcB); break;
            case Op::Div: reg(inst.dst) = reg(inst.srcA) / reg(inst.srcB); break;
            case Op::Neg: reg(inst.dst) = -reg(inst.srcA); break;

            case Op::Saturate: reg(inst.dst) = tracey::saturate(reg(inst.srcA)); break;

            case Op::Mix:
            {
                const Vec4 a = reg(inst.srcA);
                const Vec4 b = reg(inst.srcB);
                const float t = reg(inst.srcC).x;
                reg(inst.dst) = tracey::mix(a, b, t);
                break;
            }

            case Op::Clamp:
            {
                reg(inst.dst) = tracey::clamp(reg(inst.srcA), reg(inst.srcB), reg(inst.srcC));
                break;
            }

            case Op::Dot3:
            {
                const Vec3 a(reg(inst.srcA));
                const Vec3 b(reg(inst.srcB));
                reg(inst.dst) = Vec4(tracey::dot(a, b));
                break;
            }
            case Op::Length3:
            {
                const Vec3 a(reg(inst.srcA));
                reg(inst.dst) = Vec4(glm::length(a));
                break;
            }
            case Op::Cross:
            {
                const Vec3 a(reg(inst.srcA));
                const Vec3 b(reg(inst.srcB));
                reg(inst.dst) = Vec4(tracey::cross(a, b), 0.0f);
                break;
            }
            case Op::Normalize3:
            {
                const Vec3 a(reg(inst.srcA));
                reg(inst.dst) = Vec4(tracey::normalize(a), 0.0f);
                break;
            }
            case Op::Splat:
            {
                const float s = reg(inst.srcA).x;
                reg(inst.dst) = Vec4(s);
                break;
            }

            case Op::WriteAlbedo:       result.albedo = Vec3(reg(inst.srcA));       break;
            case Op::WriteMetallic:     result.metallic = reg(inst.srcA).x;         break;
            case Op::WriteRoughness:    result.roughness = reg(inst.srcA).x;        break;
            case Op::WriteEmission:     result.emission = Vec3(reg(inst.srcA));     break;
            case Op::WriteNormal:       result.normal = Vec3(reg(inst.srcA));       break;
            case Op::WriteAlpha:        result.alpha = reg(inst.srcA).x;            break;
            case Op::WriteIor:          result.ior = reg(inst.srcA).x;              break;
            case Op::WriteTransmission: result.transmission = reg(inst.srcA).x;     break;

            case Op::LoadParam:
            {
                if (inst.imm >= parameters.values.size())
                {
                    throw std::runtime_error("MaterialProgram: parameter index out of range");
                }
                reg(inst.dst) = parameters.values[inst.imm];
                break;
            }

            case Op::Count_:
            default:
                throw std::runtime_error("MaterialProgram: unknown opcode");
            }
        }

        return result;
    }
}
