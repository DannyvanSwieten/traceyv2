#include "material_program.hpp"
#include <stdexcept>

namespace tracey
{
    uint16_t MaterialProgramBuilder::allocReg()
    {
        if (m_nextReg >= kMaxRegisters)
        {
            throw std::runtime_error("MaterialProgramBuilder: register file exhausted");
        }
        return m_nextReg++;
    }

    uint16_t MaterialProgramBuilder::addConstant(const Vec4 &c)
    {
        m_constants.push_back(c);
        return static_cast<uint16_t>(m_constants.size() - 1);
    }

    void MaterialProgramBuilder::emit(Op op,
                                      uint16_t dst,
                                      uint16_t srcA,
                                      uint16_t srcB,
                                      uint16_t srcC,
                                      uint16_t imm)
    {
        if (m_code.size() >= kMaxInstructions)
        {
            throw std::runtime_error("MaterialProgramBuilder: instruction cap reached");
        }
        Instruction inst{};
        inst.op = static_cast<uint16_t>(op);
        inst.dst = dst;
        inst.srcA = srcA;
        inst.srcB = srcB;
        inst.srcC = srcC;
        inst.imm = imm;
        inst.aux = 0u;
        m_code.push_back(inst);
    }

    uint16_t MaterialProgramBuilder::loadSurface(Op surfaceOp)
    {
        uint16_t r = allocReg();
        emit(surfaceOp, r);
        return r;
    }

    uint16_t MaterialProgramBuilder::allocParam()
    {
        return m_nextParam++;
    }

    uint16_t MaterialProgramBuilder::loadParam(uint16_t paramIdx)
    {
        uint16_t r = allocReg();
        emit(Op::LoadParam, r, 0, 0, 0, paramIdx);
        return r;
    }

    MaterialProgram MaterialProgramBuilder::finalize()
    {
        if (m_code.empty() || static_cast<Op>(m_code.back().op) != Op::Halt)
        {
            emit(Op::Halt);
        }
        MaterialProgram p;
        p.code = std::move(m_code);
        p.constants = std::move(m_constants);
        p.registerCount = static_cast<uint8_t>(m_nextReg);
        p.parameterCount = m_nextParam;
        return p;
    }

    MaterialProgram makePassthroughProgram()
    {
        MaterialProgramBuilder b;
        uint16_t r;

        r = b.allocReg(); b.emit(Op::LoadInputAlbedo,    r);    b.emit(Op::WriteAlbedo,    0, r);
        r = b.allocReg(); b.emit(Op::LoadInputMetallic,  r);    b.emit(Op::WriteMetallic,  0, r);
        r = b.allocReg(); b.emit(Op::LoadInputRoughness, r);    b.emit(Op::WriteRoughness, 0, r);
        r = b.allocReg(); b.emit(Op::LoadInputEmission,  r);    b.emit(Op::WriteEmission,  0, r);
        r = b.allocReg(); b.emit(Op::LoadInputNormal,    r);    b.emit(Op::WriteNormal,    0, r);
        // Transparency / IOR / opacity pass straight through from the
        // GPUMaterial factors so plain (non-graph) materials can be glass.
        r = b.allocReg(); b.emit(Op::LoadInputTransmission, r); b.emit(Op::WriteTransmission, 0, r);
        r = b.allocReg(); b.emit(Op::LoadInputIor,          r); b.emit(Op::WriteIor,          0, r);
        r = b.allocReg(); b.emit(Op::LoadInputOpacity,      r); b.emit(Op::WriteAlpha,        0, r);

        return b.finalize();
    }

    uint32_t MaterialProgramBuffer::addProgram(const MaterialProgram &p)
    {
        Header h{};
        h.codeOffset = static_cast<uint32_t>(m_code.size());
        h.codeLength = static_cast<uint32_t>(p.code.size());
        h.constOffset = static_cast<uint32_t>(m_constants.size());
        h.constLength = static_cast<uint32_t>(p.constants.size());
        h.paramOffset = static_cast<uint32_t>(m_parameters.size());
        h.paramCount = p.parameterCount;

        m_code.insert(m_code.end(), p.code.begin(), p.code.end());
        m_constants.insert(m_constants.end(), p.constants.begin(), p.constants.end());

        // Seed parameter slots. Use defaults from the program if it supplied
        // a complete set; otherwise zero-initialise and let the host populate.
        if (p.parameterDefaults.size() == p.parameterCount)
        {
            m_parameters.insert(m_parameters.end(),
                                p.parameterDefaults.begin(),
                                p.parameterDefaults.end());
        }
        else
        {
            m_parameters.insert(m_parameters.end(), p.parameterCount, Vec4(0.0f));
        }

        const uint32_t programId = static_cast<uint32_t>(m_headers.size());
        m_headers.push_back(h);
        return programId;
    }

    void MaterialProgramBuffer::clear()
    {
        m_code.clear();
        m_constants.clear();
        m_headers.clear();
        m_parameters.clear();
    }
}
