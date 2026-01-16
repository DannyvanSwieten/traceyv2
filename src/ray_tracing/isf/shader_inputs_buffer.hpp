#pragma once

#include "../ray_tracing_pipeline/data_structure.hpp"
#include "../../device/buffer.hpp"
#include "../../device/device.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>

#include <glm/glm.hpp>

namespace tracey {

/// Manages a uniform buffer for ISF shader inputs with std140 layout.
/// Allows setting individual members by name and handles offset/padding automatically.
class ShaderInputsBuffer {
  public:
    /// Create a shader inputs buffer from a structure layout.
    /// @param device The device to allocate the buffer on
    /// @param layout The structure layout describing the inputs (typically from ISFPipelineBuilder::mergeInputs)
    ShaderInputsBuffer(Device *device, const StructureLayout &layout);
    ~ShaderInputsBuffer() = default;

    // Non-copyable
    ShaderInputsBuffer(const ShaderInputsBuffer &) = delete;
    ShaderInputsBuffer &operator=(const ShaderInputsBuffer &) = delete;

    // Movable
    ShaderInputsBuffer(ShaderInputsBuffer &&) = default;
    ShaderInputsBuffer &operator=(ShaderInputsBuffer &&) = default;

    /// Set a float value
    void setFloat(const std::string &name, float value);

    /// Set an int value
    void setInt(const std::string &name, int value);

    /// Set a uint value
    void setUint(const std::string &name, uint32_t value);

    /// Set a vec2 value
    void setVec2(const std::string &name, const glm::vec2 &value);

    /// Set a vec3 value
    void setVec3(const std::string &name, const glm::vec3 &value);

    /// Set a vec4 value (also used for "color" type)
    void setVec4(const std::string &name, const glm::vec4 &value);

    /// Set a color value (alias for setVec4)
    void setColor(const std::string &name, const glm::vec4 &color) { setVec4(name, color); }

    /// Get the underlying buffer for binding to descriptor sets
    Buffer *buffer() const { return m_buffer.get(); }

    /// Upload the current data to the GPU buffer
    void upload();

    /// Get the total size of the buffer in bytes
    size_t size() const { return m_data.size(); }

    /// Check if a member exists
    bool hasMember(const std::string &name) const;

  private:
    struct MemberInfo {
        size_t offset;
        size_t size;
        std::string type;
    };

    // Calculate std140 alignment for a GLSL type
    static size_t getStd140Alignment(const std::string &type);
    // Calculate std140 size for a GLSL type
    static size_t getStd140Size(const std::string &type);

    // Build the member offset map from the layout
    void buildMemberMap(const StructureLayout &layout);

    // Get member info, throws if not found
    const MemberInfo &getMemberInfo(const std::string &name) const;

    std::unique_ptr<Buffer> m_buffer;
    std::vector<uint8_t> m_data;
    std::unordered_map<std::string, MemberInfo> m_members;
};

} // namespace tracey
