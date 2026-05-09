#include "attribute_table.hpp"

namespace tracey
{
    AttributeTable &AttributeTable::operator=(const AttributeTable &other)
    {
        if (this == &other) return *this;
        m_class = other.m_class;
        m_size = other.m_size;
        m_byName.clear();
        for (const auto &[name, attr] : other.m_byName)
        {
            m_byName.emplace(name, attr->clone());
        }
        return *this;
    }

    void AttributeTable::resize(size_t n)
    {
        m_size = n;
        for (auto &[_, attr] : m_byName)
        {
            attr->resize(n);
        }
    }

    AttributeBase *AttributeTable::find(std::string_view name)
    {
        auto it = m_byName.find(std::string(name));
        return it == m_byName.end() ? nullptr : it->second.get();
    }

    const AttributeBase *AttributeTable::find(std::string_view name) const
    {
        auto it = m_byName.find(std::string(name));
        return it == m_byName.end() ? nullptr : it->second.get();
    }

    void AttributeTable::remove(std::string_view name)
    {
        m_byName.erase(std::string(name));
    }

    std::vector<std::string> AttributeTable::names() const
    {
        std::vector<std::string> out;
        out.reserve(m_byName.size());
        for (const auto &[name, _] : m_byName) out.push_back(name);
        return out;
    }
}
