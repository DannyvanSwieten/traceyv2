#pragma once

#include "attribute.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tracey
{
    // A bag of attributes, all addressing the same number of elements (the
    // class size). One AttributeTable per AttributeClass per Geometry.
    //
    // resize(n) propagates to every contained attribute so they stay in lockstep.
    class AttributeTable
    {
    public:
        AttributeTable() = default;
        explicit AttributeTable(AttributeClass cls) : m_class(cls) {}

        AttributeTable(const AttributeTable &other) { *this = other; }
        AttributeTable &operator=(const AttributeTable &other);

        AttributeTable(AttributeTable &&) noexcept = default;
        AttributeTable &operator=(AttributeTable &&) noexcept = default;

        AttributeClass attributeClass() const { return m_class; }
        void setAttributeClass(AttributeClass cls) { m_class = cls; }

        size_t size() const { return m_size; }

        // Resize every contained attribute to `n`. New elements take each
        // attribute's defaultValue().
        void resize(size_t n);

        bool has(std::string_view name) const { return m_byName.contains(std::string(name)); }

        // Add (or replace) an attribute of element type T. Returns a pointer
        // to the typed attribute (not owned by caller). Pre-fills with the
        // current table size's worth of `def` values.
        template <typename T>
        Attribute<T> *add(std::string name, T def = T{})
        {
            auto attr = std::make_unique<Attribute<T>>(name, m_class, m_size, def);
            auto *raw = attr.get();
            m_byName[std::move(name)] = std::move(attr);
            return raw;
        }

        // Look up a typed attribute. Returns nullptr if missing or if the
        // stored type doesn't match T.
        template <typename T>
        Attribute<T> *get(std::string_view name)
        {
            auto it = m_byName.find(std::string(name));
            if (it == m_byName.end()) return nullptr;
            return dynamic_cast<Attribute<T> *>(it->second.get());
        }

        template <typename T>
        const Attribute<T> *get(std::string_view name) const
        {
            auto it = m_byName.find(std::string(name));
            if (it == m_byName.end()) return nullptr;
            return dynamic_cast<const Attribute<T> *>(it->second.get());
        }

        // Untyped lookup — needed by the Geometry → SceneObject converter
        // when it just needs to enumerate attributes for serialization.
        AttributeBase *find(std::string_view name);
        const AttributeBase *find(std::string_view name) const;

        void remove(std::string_view name);

        // Names of all attributes in insertion-stable order.
        std::vector<std::string> names() const;

    private:
        AttributeClass m_class = AttributeClass::Point;
        size_t m_size = 0;
        std::unordered_map<std::string, std::unique_ptr<AttributeBase>> m_byName;
    };
}
