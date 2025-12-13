#include "data_structure.hpp"

namespace tracey
{
    BufferLayout::BufferLayout(const std::string_view name)
        : m_name(name)
    {
    }

    void BufferLayout::addMember(const StructField &field)
    {
        m_fields.push_back(field);
    }
}