#pragma once

#include "../core/types.hpp"
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>
#include <memory>

namespace tracey
{
    /**
     * @brief Attribute class - where the attribute data lives
     *
     * - Point: Per-point attribute (one value per position)
     * - Vertex: Per-vertex attribute (one value per index, for per-face data)
     * - Primitive: Per-primitive attribute (one value per face/triangle)
     * - Detail: Global attribute (single value for entire geometry)
     */
    enum class AttributeClass
    {
        Point,      // Per-point (one per position)
        Vertex,     // Per-vertex (one per corner of face)
        Primitive,  // Per-primitive (one per face/triangle)
        Detail      // Global (one for entire geometry)
    };

    /**
     * @brief Attribute type enumeration
     */
    enum class AttributeType
    {
        Float,
        Vec2,
        Vec3,
        Vec4,
        Int,
        String
    };

    /**
     * @brief Attribute value storage using variant
     */
    using AttributeData = std::variant<
        std::vector<float>,
        std::vector<Vec2>,
        std::vector<Vec3>,
        std::vector<Vec4>,
        std::vector<int>,
        std::vector<std::string>
    >;

    /**
     * @brief Generic attribute for geometry
     *
     * Stores typed data per-point, per-vertex, per-primitive, or per-detail.
     * This is the foundation for VOP-style attribute manipulation.
     */
    class Attribute
    {
    public:
        Attribute(std::string name, AttributeClass attrClass, AttributeType type);
        Attribute(std::string name, AttributeClass attrClass, AttributeType type, size_t count);

        const std::string& name() const { return m_name; }
        AttributeClass attributeClass() const { return m_class; }
        AttributeType type() const { return m_type; }

        // Get data size (number of elements)
        size_t size() const;

        // Resize attribute data
        void resize(size_t count);

        // Get/set typed data
        template<typename T>
        const std::vector<T>* getData() const {
            return std::get_if<std::vector<T>>(&m_data);
        }

        template<typename T>
        std::vector<T>* getData() {
            return std::get_if<std::vector<T>>(&m_data);
        }

        // Direct access to data variant
        const AttributeData& data() const { return m_data; }
        AttributeData& data() { return m_data; }

    private:
        std::string m_name;
        AttributeClass m_class;
        AttributeType m_type;
        AttributeData m_data;
    };

    /**
     * @brief Attribute container for a geometry
     *
     * Manages all attributes of different classes (point, vertex, primitive, detail)
     */
    class AttributeSet
    {
    public:
        AttributeSet() = default;
        ~AttributeSet() = default;

        // Delete copy, allow move
        AttributeSet(const AttributeSet&) = delete;
        AttributeSet& operator=(const AttributeSet&) = delete;
        AttributeSet(AttributeSet&&) = default;
        AttributeSet& operator=(AttributeSet&&) = default;

        // Add attribute
        void addAttribute(std::unique_ptr<Attribute> attr);

        // Get attribute by name and class
        Attribute* getAttribute(AttributeClass attrClass, const std::string& name);
        const Attribute* getAttribute(AttributeClass attrClass, const std::string& name) const;

        // Check if attribute exists
        bool hasAttribute(AttributeClass attrClass, const std::string& name) const;

        // Remove attribute
        void removeAttribute(AttributeClass attrClass, const std::string& name);

        // Get all attributes of a class
        const std::unordered_map<std::string, std::unique_ptr<Attribute>>&
        getAttributes(AttributeClass attrClass) const;

        // Resize all attributes of a class to match element count
        void resizeAttributes(AttributeClass attrClass, size_t count);

    private:
        std::unordered_map<std::string, std::unique_ptr<Attribute>> m_pointAttributes;
        std::unordered_map<std::string, std::unique_ptr<Attribute>> m_vertexAttributes;
        std::unordered_map<std::string, std::unique_ptr<Attribute>> m_primitiveAttributes;
        std::unordered_map<std::string, std::unique_ptr<Attribute>> m_detailAttributes;

        std::unordered_map<std::string, std::unique_ptr<Attribute>>&
        getAttributeMap(AttributeClass attrClass);

        const std::unordered_map<std::string, std::unique_ptr<Attribute>>&
        getAttributeMap(AttributeClass attrClass) const;
    };

} // namespace tracey
