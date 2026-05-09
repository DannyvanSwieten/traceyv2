#pragma once

#include "../core/types.hpp"
#include "attribute_class.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <typeindex>
#include <vector>

namespace tracey
{
    // Type-erased attribute base. Stored owner-side as unique_ptr<AttributeBase>
    // in AttributeTable; downcast to Attribute<T> to read/write typed data.
    //
    // The Welford-style cook caching (see SopGraph) treats two attributes as
    // equivalent only if name + AttributeClass + element type match.
    class AttributeBase
    {
    public:
        AttributeBase(std::string name, AttributeClass cls)
            : m_name(std::move(name)), m_class(cls) {}
        virtual ~AttributeBase() = default;

        const std::string &name() const { return m_name; }
        AttributeClass attributeClass() const { return m_class; }

        // Element count this attribute carries. Must match the table's
        // class-size when the attribute is bound (table::resize() will keep
        // them in sync).
        virtual size_t size() const = 0;
        virtual void resize(size_t n) = 0;

        // For codegen + serialization: the C++ element type. Compared via
        // std::type_index for fast equality without RTTI on the hot path.
        virtual std::type_index typeIndex() const = 0;

        // Stable string tag for JSON serialization. One of:
        //   "float", "int", "vec2", "vec3", "vec4", "mat3", "mat4", "string"
        virtual const char *typeTag() const = 0;

        // Deep copy.
        virtual std::unique_ptr<AttributeBase> clone() const = 0;

    private:
        std::string m_name;
        AttributeClass m_class;
    };

    // Typed attribute holding `vector<T>` of element data. T ∈ {float, int,
    // Vec2, Vec3, Vec4, Mat3, Mat4, std::string}.
    template <typename T>
    class Attribute : public AttributeBase
    {
    public:
        Attribute(std::string name, AttributeClass cls, size_t size, T def = T{})
            : AttributeBase(std::move(name), cls), m_data(size, def), m_default(std::move(def)) {}

        size_t size() const override { return m_data.size(); }
        void resize(size_t n) override { m_data.resize(n, m_default); }

        std::type_index typeIndex() const override { return std::type_index(typeid(T)); }
        const char *typeTag() const override;

        std::unique_ptr<AttributeBase> clone() const override
        {
            auto out = std::make_unique<Attribute<T>>(name(), attributeClass(), m_data.size(), m_default);
            out->m_data = m_data;
            return out;
        }

        std::vector<T> &data() { return m_data; }
        const std::vector<T> &data() const { return m_data; }
        T &at(size_t i) { return m_data[i]; }
        const T &at(size_t i) const { return m_data[i]; }

        const T &defaultValue() const { return m_default; }

    private:
        std::vector<T> m_data;
        T m_default;
    };

    // typeTag specializations live in attribute.cpp.
    template <> const char *Attribute<float>::typeTag() const;
    template <> const char *Attribute<int>::typeTag() const;
    template <> const char *Attribute<Vec2>::typeTag() const;
    template <> const char *Attribute<Vec3>::typeTag() const;
    template <> const char *Attribute<Vec4>::typeTag() const;
    template <> const char *Attribute<Mat3>::typeTag() const;
    template <> const char *Attribute<Mat4>::typeTag() const;
    template <> const char *Attribute<std::string>::typeTag() const;
}
