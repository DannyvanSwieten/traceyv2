// See glsl_layout.hpp. Extracted from vulkan_compute_pipeline_compiler.cpp
// so the host-side ShaderInputsBuffer and the shader-side struct emitters
// share one implementation.

#include "glsl_layout.hpp"

namespace tracey
{
    // Helper: Get alignment requirement for GLSL type in std140 layout (uniform buffers)
    size_t getStd140Alignment(const std::string &glslType)
    {
        if (glslType == "float" || glslType == "int" || glslType == "uint" || glslType == "bool")
            return 4;
        if (glslType == "vec2" || glslType == "ivec2" || glslType == "uvec2")
            return 8;
        if (glslType == "vec3" || glslType == "ivec3" || glslType == "uvec3")
            return 16; // std140: vec3 is 16-byte aligned
        if (glslType == "vec4" || glslType == "ivec4" || glslType == "uvec4")
            return 16;
        if (glslType == "mat3")
            return 16;
        if (glslType == "mat4")
            return 16;
        return 16; // Default for unknown types
    }

    // Helper: Get size for GLSL type in std140 layout
    size_t getStd140Size(const std::string &glslType)
    {
        if (glslType == "float" || glslType == "int" || glslType == "uint" || glslType == "bool")
            return 4;
        if (glslType == "vec2" || glslType == "ivec2" || glslType == "uvec2")
            return 8;
        if (glslType == "vec3" || glslType == "ivec3" || glslType == "uvec3")
            return 16; // std140: vec3 takes 16 bytes
        if (glslType == "vec4" || glslType == "ivec4" || glslType == "uvec4")
            return 16;
        if (glslType == "mat3")
            return 48; // 3 columns * 16 bytes each
        if (glslType == "mat4")
            return 64; // 4 columns * 16 bytes each
        return 16;
    }

    // Helper: Generate struct with proper std140 padding for uniform buffers
    void generateStd140Struct(std::stringstream &ss, const std::string &structName, const std::vector<StructField> &fields)
    {
        ss << "struct " << structName << " {\n";

        size_t currentOffset = 0;
        int paddingCounter = 0;

        for (const auto &field : fields)
        {
            size_t alignment = getStd140Alignment(field.type);
            size_t fieldSize = getStd140Size(field.type);

            // Calculate aligned offset
            size_t alignedOffset = (currentOffset + alignment - 1) & ~(alignment - 1);

            // Insert padding if needed
            // IMPORTANT: In std140, arrays of scalars are aligned to vec4 (16 bytes) per element!
            // So we use individual float fields for padding, NOT float arrays.
            if (alignedOffset > currentOffset)
            {
                size_t paddingSize = alignedOffset - currentOffset;
                size_t numFloats = paddingSize / 4;
                for (size_t i = 0; i < numFloats; ++i)
                {
                    ss << "    float _pad" << paddingCounter++ << ";\n";
                }
            }

            // Write the actual field
            ss << "    " << field.type << " " << field.name;
            if (field.isArray)
            {
                ss << "[";
                if (field.elementCount > 0)
                {
                    ss << field.elementCount;
                }
                ss << "]";
                fieldSize *= field.elementCount > 0 ? field.elementCount : 1;
            }
            ss << ";\n";

            // std140's vec3-in-struct is the classic gotcha. The Khronos spec
            // leaves room for two interpretations: a vec3 followed by a
            // smaller-aligned member (e.g. uint) lands either at +12 or +16
            // depending on the GLSL compiler's interpretation. Our host-side
            // ShaderInputsBuffer treats vec3 as 16 bytes (matching alignment),
            // so the next member sits at +16. Force the shader compiler to
            // agree by emitting an explicit trailing float pad after each
            // vec3-typed scalar member -- this fills the implicit 4-byte tail
            // and makes the next member's offset unambiguous on both sides.
            if ((field.type == "vec3" || field.type == "ivec3" || field.type == "uvec3")
                && !field.isArray)
            {
                ss << "    float _pad" << paddingCounter++ << ";\n";
            }

            currentOffset = alignedOffset + fieldSize;
        }

        ss << "};\n";
    }

    // Helper: Get alignment requirement for GLSL type in std430 layout
    size_t getStd430Alignment(const std::string &glslType)
    {
        if (glslType == "float" || glslType == "int" || glslType == "uint")
            return 4;
        if (glslType == "vec2" || glslType == "ivec2" || glslType == "uvec2")
            return 8;
        if (glslType == "vec3" || glslType == "ivec3" || glslType == "uvec3")
            return 16; // std430: vec3 is 16-byte aligned
        if (glslType == "vec4" || glslType == "ivec4" || glslType == "uvec4")
            return 16;
        if (glslType == "mat3")
            return 16; // mat3 is array of 3 vec3, each aligned to 16
        if (glslType == "mat4")
            return 16;
        return 16; // Default for unknown types
    }

    // Helper: Get size for GLSL type in std430 layout
    size_t getStd430Size(const std::string &glslType)
    {
        if (glslType == "float" || glslType == "int" || glslType == "uint")
            return 4;
        if (glslType == "vec2" || glslType == "ivec2" || glslType == "uvec2")
            return 8;
        if (glslType == "vec3" || glslType == "ivec3" || glslType == "uvec3")
            return 12; // std430: vec3 is 12 bytes (but 16-byte aligned)
        if (glslType == "vec4" || glslType == "ivec4" || glslType == "uvec4")
            return 16;
        if (glslType == "mat3")
            return 48; // 3 * 16 (each column is vec3, 16-byte aligned)
        if (glslType == "mat4")
            return 64; // 4 * 16
        return 16;     // Default for unknown types
    }

    // Helper: Generate struct with proper std430 padding
    void generateStd430Struct(std::stringstream &ss, const std::string &structName, const std::vector<StructField> &fields)
    {
        ss << "struct " << structName << " {\n";

        size_t currentOffset = 0;
        int paddingCounter = 0;

        for (const auto &field : fields)
        {
            size_t alignment = getStd430Alignment(field.type);
            size_t fieldSize = getStd430Size(field.type);

            // Calculate aligned offset
            size_t alignedOffset = (currentOffset + alignment - 1) & ~(alignment - 1);

            // Insert padding if needed
            if (alignedOffset > currentOffset)
            {
                size_t paddingSize = alignedOffset - currentOffset;
                // Determine padding type based on size
                if (paddingSize == 4)
                {
                    ss << "    float _pad" << paddingCounter++ << ";\n";
                }
                else if (paddingSize == 8)
                {
                    ss << "    vec2 _pad" << paddingCounter++ << ";\n";
                }
                else if (paddingSize == 12)
                {
                    ss << "    vec3 _pad" << paddingCounter++ << ";\n";
                }
                else if (paddingSize >= 16)
                {
                    // Use multiple vec4s for large padding
                    while (paddingSize >= 16)
                    {
                        ss << "    vec4 _pad" << paddingCounter++ << ";\n";
                        paddingSize -= 16;
                    }
                    if (paddingSize == 12)
                    {
                        ss << "    vec3 _pad" << paddingCounter++ << ";\n";
                    }
                    else if (paddingSize == 8)
                    {
                        ss << "    vec2 _pad" << paddingCounter++ << ";\n";
                    }
                    else if (paddingSize == 4)
                    {
                        ss << "    float _pad" << paddingCounter++ << ";\n";
                    }
                }
            }

            // Write the actual field
            ss << "    " << field.type << " " << field.name;
            if (field.isArray)
            {
                ss << "[";
                if (field.elementCount > 0)
                {
                    ss << field.elementCount;
                }
                ss << "]";
                fieldSize *= field.elementCount > 0 ? field.elementCount : 1;
            }
            ss << ";\n";

            currentOffset = alignedOffset + fieldSize;
        }

        // Align struct size to 16 bytes (std430 requirement for structs in arrays)
        size_t alignedSize = (currentOffset + 15) & ~15;
        if (alignedSize > currentOffset)
        {
            size_t finalPadding = alignedSize - currentOffset;
            if (finalPadding == 4)
            {
                ss << "    float _pad" << paddingCounter++ << ";\n";
            }
            else if (finalPadding == 8)
            {
                ss << "    vec2 _pad" << paddingCounter++ << ";\n";
            }
            else if (finalPadding == 12)
            {
                ss << "    vec3 _pad" << paddingCounter++ << ";\n";
            }
        }

        ss << "};\n";
    }
}
