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
    class StructureLayout
    {
    public:
        StructureLayout(const std::string_view name);
        void addMember(const StructField &field);
        const std::vector<StructField> &fields() const { return m_fields; }
        const std::string &name() const { return m_name; }
        size_t size() const { return m_size; }

    private:
        std::string m_name;
        size_t m_size = 0;
        std::vector<StructField> m_fields;
    };
}