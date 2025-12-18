#include "data_structure.hpp"
#include <cassert>
namespace tracey
{
    StructureLayout::StructureLayout(const std::string_view name)
        : m_name(name)
    {
    }

    void StructureLayout::addMember(const StructField &field)
    {
        if (field.type == "float")
        {
            m_size += 4;
        }
        else if (field.type == "vec2")
        {
            m_size += 8;
        }
        else if (field.type == "vec3")
        {
            m_size += 12;
        }
        else if (field.type == "vec4")
        {
            m_size += 16;
        }
        else if (field.type == "int")
        {
            m_size += 4;
        }
        else if (field.type == "ivec2")
        {
            m_size += 8;
        }
        else if (field.type == "ivec3")
        {
            m_size += 12;
        }
        else if (field.type == "ivec4")
        {
            m_size += 16;
        }
        else if (field.type == "bool")
        {
            m_size += 1;
        }
        else
        {
            // Handle other types or throw an error
            assert(false && "Unknown type in StructField");
        }
        m_fields.push_back(field);
    }
}