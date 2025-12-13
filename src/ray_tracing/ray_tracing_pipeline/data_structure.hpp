#pragma once

#include <vector>

namespace tracey
{
    struct StructField
    {
        std::string name;
        std::string type;
        size_t offset;
        bool isArray = false;
        size_t elementCount = 0;
    };
    class BufferLayout
    {
    public:
        BufferLayout(const std::string_view name);
        void addMember(const StructField &field);
        const std::vector<StructField> &fields() const { return m_fields; }
        const std::string &name() const { return m_name; }

    private:
        std::string m_name;
        std::vector<StructField> m_fields;
    };
}