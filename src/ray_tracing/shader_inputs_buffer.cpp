#include "shader_inputs_buffer.hpp"

#include <stdexcept>

namespace tracey {

size_t ShaderInputsBuffer::getStd140Alignment(const std::string &type) {
    if (type == "float" || type == "int" || type == "uint" || type == "bool")
        return 4;
    if (type == "vec2" || type == "ivec2" || type == "uvec2")
        return 8;
    if (type == "vec3" || type == "ivec3" || type == "uvec3")
        return 16; // std140: vec3 is 16-byte aligned
    if (type == "vec4" || type == "ivec4" || type == "uvec4")
        return 16;
    if (type == "mat3")
        return 16;
    if (type == "mat4")
        return 16;
    return 16; // Default for unknown types
}

size_t ShaderInputsBuffer::getStd140Size(const std::string &type) {
    if (type == "float" || type == "int" || type == "uint" || type == "bool")
        return 4;
    if (type == "vec2" || type == "ivec2" || type == "uvec2")
        return 8;
    if (type == "vec3" || type == "ivec3" || type == "uvec3")
        return 16; // std140: vec3 takes 16 bytes (with padding)
    if (type == "vec4" || type == "ivec4" || type == "uvec4")
        return 16;
    if (type == "mat3")
        return 48; // 3 columns * 16 bytes each
    if (type == "mat4")
        return 64; // 4 columns * 16 bytes each
    return 16;
}

void ShaderInputsBuffer::buildMemberMap(const StructureLayout &layout) {
    size_t currentOffset = 0;

    fprintf(stderr, "\n=== ShaderInputsBuffer layout ===\n");
    for (const auto &field : layout.fields()) {
        size_t alignment = getStd140Alignment(field.type);
        size_t fieldSize = getStd140Size(field.type);

        // Calculate aligned offset
        size_t alignedOffset = (currentOffset + alignment - 1) & ~(alignment - 1);

        fprintf(stderr, "  Field '%s' (type=%s): offset=%zu, size=%zu, alignment=%zu\n",
                field.name.c_str(), field.type.c_str(), alignedOffset, fieldSize, alignment);

        // Store member info
        m_members[field.name] = MemberInfo{alignedOffset, fieldSize, field.type};

        currentOffset = alignedOffset + fieldSize;
    }

    // Allocate CPU-side storage (rounded up to 16-byte alignment for the whole struct)
    size_t totalSize = (currentOffset + 15) & ~15;
    m_data.resize(totalSize, 0);
    fprintf(stderr, "  Total size: %zu bytes\n", totalSize);
    fprintf(stderr, "=================================\n\n");
}

ShaderInputsBuffer::ShaderInputsBuffer(Device *device, const StructureLayout &layout) {
    buildMemberMap(layout);

    // Create the GPU buffer
    m_buffer.reset(device->createBuffer(static_cast<uint32_t>(m_data.size()), BufferUsage::UniformBuffer));
}

const ShaderInputsBuffer::MemberInfo &ShaderInputsBuffer::getMemberInfo(const std::string &name) const {
    auto it = m_members.find(name);
    if (it == m_members.end()) {
        throw std::runtime_error("ShaderInputsBuffer: Unknown member '" + name + "'");
    }
    return it->second;
}

bool ShaderInputsBuffer::hasMember(const std::string &name) const {
    return m_members.find(name) != m_members.end();
}

void ShaderInputsBuffer::setFloat(const std::string &name, float value) {
    const auto &info = getMemberInfo(name);
    if (info.type != "float") {
        throw std::runtime_error("ShaderInputsBuffer: Member '" + name + "' is not a float (is " + info.type + ")");
    }
    fprintf(stderr, "ShaderInputsBuffer::setFloat('%s', %f) at offset %zu\n",
            name.c_str(), value, info.offset);
    std::memcpy(m_data.data() + info.offset, &value, sizeof(float));
}

void ShaderInputsBuffer::setInt(const std::string &name, int value) {
    const auto &info = getMemberInfo(name);
    if (info.type != "int") {
        throw std::runtime_error("ShaderInputsBuffer: Member '" + name + "' is not an int (is " + info.type + ")");
    }
    std::memcpy(m_data.data() + info.offset, &value, sizeof(int));
}

void ShaderInputsBuffer::setUint(const std::string &name, uint32_t value) {
    const auto &info = getMemberInfo(name);
    if (info.type != "uint" && info.type != "bool") {
        throw std::runtime_error("ShaderInputsBuffer: Member '" + name + "' is not a uint (is " + info.type + ")");
    }
    std::memcpy(m_data.data() + info.offset, &value, sizeof(uint32_t));
}

void ShaderInputsBuffer::setVec2(const std::string &name, const glm::vec2 &value) {
    const auto &info = getMemberInfo(name);
    if (info.type != "vec2") {
        throw std::runtime_error("ShaderInputsBuffer: Member '" + name + "' is not a vec2 (is " + info.type + ")");
    }
    std::memcpy(m_data.data() + info.offset, &value, sizeof(glm::vec2));
}

void ShaderInputsBuffer::setVec3(const std::string &name, const glm::vec3 &value) {
    const auto &info = getMemberInfo(name);
    if (info.type != "vec3") {
        throw std::runtime_error("ShaderInputsBuffer: Member '" + name + "' is not a vec3 (is " + info.type + ")");
    }
    // Note: vec3 in std140 takes 16 bytes, but we only copy 12 bytes of data
    std::memcpy(m_data.data() + info.offset, &value, sizeof(glm::vec3));
}

void ShaderInputsBuffer::setVec4(const std::string &name, const glm::vec4 &value) {
    const auto &info = getMemberInfo(name);
    if (info.type != "vec4") {
        throw std::runtime_error("ShaderInputsBuffer: Member '" + name + "' is not a vec4 (is " + info.type + ")");
    }
    fprintf(stderr, "ShaderInputsBuffer::setVec4('%s', [%f, %f, %f, %f]) at offset %zu\n",
            name.c_str(), value.x, value.y, value.z, value.w, info.offset);
    std::memcpy(m_data.data() + info.offset, &value, sizeof(glm::vec4));
}

void ShaderInputsBuffer::upload() {
    fprintf(stderr, "ShaderInputsBuffer::upload() - uploading %zu bytes\n", m_data.size());
    fprintf(stderr, "  Raw data: ");
    for (size_t i = 0; i < m_data.size(); ++i) {
        fprintf(stderr, "%02x ", m_data[i]);
    }
    fprintf(stderr, "\n");

    void *mapped = m_buffer->mapForWriting();
    std::memcpy(mapped, m_data.data(), m_data.size());
    m_buffer->flush();
}

} // namespace tracey
