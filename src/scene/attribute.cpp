#include "attribute.hpp"
#include <stdexcept>

namespace tracey
{
    Attribute::Attribute(std::string name, AttributeClass attrClass, AttributeType type)
        : m_name(std::move(name))
        , m_class(attrClass)
        , m_type(type)
    {
        // Initialize with appropriate empty vector based on type
        switch (type) {
            case AttributeType::Float:
                m_data = std::vector<float>();
                break;
            case AttributeType::Vec2:
                m_data = std::vector<Vec2>();
                break;
            case AttributeType::Vec3:
                m_data = std::vector<Vec3>();
                break;
            case AttributeType::Vec4:
                m_data = std::vector<Vec4>();
                break;
            case AttributeType::Int:
                m_data = std::vector<int>();
                break;
            case AttributeType::String:
                m_data = std::vector<std::string>();
                break;
        }
    }

    Attribute::Attribute(std::string name, AttributeClass attrClass, AttributeType type, size_t count)
        : Attribute(std::move(name), attrClass, type)
    {
        resize(count);
    }

    size_t Attribute::size() const
    {
        return std::visit([](const auto& vec) { return vec.size(); }, m_data);
    }

    void Attribute::resize(size_t count)
    {
        std::visit([count](auto& vec) { vec.resize(count); }, m_data);
    }

    // AttributeSet implementation

    void AttributeSet::addAttribute(std::unique_ptr<Attribute> attr)
    {
        auto& map = getAttributeMap(attr->attributeClass());
        map[attr->name()] = std::move(attr);
    }

    Attribute* AttributeSet::getAttribute(AttributeClass attrClass, const std::string& name)
    {
        auto& map = getAttributeMap(attrClass);
        auto it = map.find(name);
        return it != map.end() ? it->second.get() : nullptr;
    }

    const Attribute* AttributeSet::getAttribute(AttributeClass attrClass, const std::string& name) const
    {
        const auto& map = getAttributeMap(attrClass);
        auto it = map.find(name);
        return it != map.end() ? it->second.get() : nullptr;
    }

    bool AttributeSet::hasAttribute(AttributeClass attrClass, const std::string& name) const
    {
        const auto& map = getAttributeMap(attrClass);
        return map.find(name) != map.end();
    }

    void AttributeSet::removeAttribute(AttributeClass attrClass, const std::string& name)
    {
        auto& map = getAttributeMap(attrClass);
        map.erase(name);
    }

    const std::unordered_map<std::string, std::unique_ptr<Attribute>>&
    AttributeSet::getAttributes(AttributeClass attrClass) const
    {
        return getAttributeMap(attrClass);
    }

    void AttributeSet::resizeAttributes(AttributeClass attrClass, size_t count)
    {
        auto& map = getAttributeMap(attrClass);
        for (auto& [name, attr] : map) {
            attr->resize(count);
        }
    }

    std::unordered_map<std::string, std::unique_ptr<Attribute>>&
    AttributeSet::getAttributeMap(AttributeClass attrClass)
    {
        switch (attrClass) {
            case AttributeClass::Point:
                return m_pointAttributes;
            case AttributeClass::Vertex:
                return m_vertexAttributes;
            case AttributeClass::Primitive:
                return m_primitiveAttributes;
            case AttributeClass::Detail:
                return m_detailAttributes;
            default:
                throw std::runtime_error("Invalid attribute class");
        }
    }

    const std::unordered_map<std::string, std::unique_ptr<Attribute>>&
    AttributeSet::getAttributeMap(AttributeClass attrClass) const
    {
        switch (attrClass) {
            case AttributeClass::Point:
                return m_pointAttributes;
            case AttributeClass::Vertex:
                return m_vertexAttributes;
            case AttributeClass::Primitive:
                return m_primitiveAttributes;
            case AttributeClass::Detail:
                return m_detailAttributes;
            default:
                throw std::runtime_error("Invalid attribute class");
        }
    }

} // namespace tracey
