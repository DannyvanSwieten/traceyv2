// CPU-side view of the ShaderInputs uniform block. Backends that don't
// consume the std140 buffer on the GPU (Metal, CPU) read the camera and
// render state back out of it with this helper.
//
// Offsets follow std140 for the member order declared in
// PathTracer::createShaderInputs(): fov, cameraPosition, cameraForward,
// cameraRight, cameraUp, maxDepth, currentSample, lightCount.

#pragma once

#include "shader_inputs_buffer.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <cstring>

namespace tracey
{
    struct ShaderInputsView
    {
        float fov = 45.0f;          // degrees
        glm::vec3 cameraPosition{0.0f};
        glm::vec3 cameraForward{0.0f, 0.0f, -1.0f};
        glm::vec3 cameraRight{1.0f, 0.0f, 0.0f};
        glm::vec3 cameraUp{0.0f, 1.0f, 0.0f};
        uint32_t maxDepth = 8;
        int32_t currentSample = 1;
        uint32_t lightCount = 0;
        float aperture = 0.0f;       // thin-lens radius (0 = pinhole)
        float focalDistance = 5.0f;  // in-focus distance along the view dir
    };

    inline ShaderInputsView readShaderInputs(const ShaderInputsBuffer &inputs)
    {
        const auto *bytes = static_cast<const uint8_t *>(inputs.buffer()->mapForReading());
        auto readVec3 = [&](size_t off) {
            glm::vec3 v;
            std::memcpy(&v, bytes + off, sizeof(v));
            return v;
        };
        ShaderInputsView out;
        std::memcpy(&out.fov, bytes + 0, sizeof(float));
        out.cameraPosition = readVec3(16);
        out.cameraForward = readVec3(32);
        out.cameraRight = readVec3(48);
        out.cameraUp = readVec3(64);
        std::memcpy(&out.maxDepth, bytes + 80, sizeof(uint32_t));
        std::memcpy(&out.currentSample, bytes + 84, sizeof(int32_t));
        std::memcpy(&out.lightCount, bytes + 88, sizeof(uint32_t));
        std::memcpy(&out.aperture, bytes + 92, sizeof(float));
        std::memcpy(&out.focalDistance, bytes + 96, sizeof(float));
        inputs.buffer()->unmap();
        return out;
    }
} // namespace tracey
