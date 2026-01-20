#include "material_instance.hpp"

namespace tracey
{
    MaterialInstance::MaterialInstance(const std::string &shaderId)
        : m_shaderId(shaderId)
    {
    }

    void MaterialInstance::setFloat(const std::string &name, float value)
    {
        m_properties[name] = value;
    }

    void MaterialInstance::setVec2(const std::string &name, const Vec2 &value)
    {
        m_properties[name] = value;
    }

    void MaterialInstance::setVec3(const std::string &name, const Vec3 &value)
    {
        m_properties[name] = value;
    }

    void MaterialInstance::setVec4(const std::string &name, const Vec4 &value)
    {
        m_properties[name] = value;
    }

    void MaterialInstance::setInt(const std::string &name, int value)
    {
        m_properties[name] = value;
    }

    void MaterialInstance::setTexture(const std::string &name, const std::string &texturePath)
    {
        m_properties[name] = texturePath;
    }

    std::optional<float> MaterialInstance::getFloat(const std::string &name) const
    {
        auto it = m_properties.find(name);
        if (it != m_properties.end() && std::holds_alternative<float>(it->second))
        {
            return std::get<float>(it->second);
        }
        return std::nullopt;
    }

    std::optional<Vec2> MaterialInstance::getVec2(const std::string &name) const
    {
        auto it = m_properties.find(name);
        if (it != m_properties.end() && std::holds_alternative<Vec2>(it->second))
        {
            return std::get<Vec2>(it->second);
        }
        return std::nullopt;
    }

    std::optional<Vec3> MaterialInstance::getVec3(const std::string &name) const
    {
        auto it = m_properties.find(name);
        if (it != m_properties.end() && std::holds_alternative<Vec3>(it->second))
        {
            return std::get<Vec3>(it->second);
        }
        return std::nullopt;
    }

    std::optional<Vec4> MaterialInstance::getVec4(const std::string &name) const
    {
        auto it = m_properties.find(name);
        if (it != m_properties.end() && std::holds_alternative<Vec4>(it->second))
        {
            return std::get<Vec4>(it->second);
        }
        return std::nullopt;
    }

    std::optional<int> MaterialInstance::getInt(const std::string &name) const
    {
        auto it = m_properties.find(name);
        if (it != m_properties.end() && std::holds_alternative<int>(it->second))
        {
            return std::get<int>(it->second);
        }
        return std::nullopt;
    }

    std::optional<std::string> MaterialInstance::getTexture(const std::string &name) const
    {
        auto it = m_properties.find(name);
        if (it != m_properties.end() && std::holds_alternative<std::string>(it->second))
        {
            return std::get<std::string>(it->second);
        }
        return std::nullopt;
    }

    bool MaterialInstance::hasProperty(const std::string &name) const
    {
        return m_properties.find(name) != m_properties.end();
    }

    const MaterialValue *MaterialInstance::getProperty(const std::string &name) const
    {
        auto it = m_properties.find(name);
        if (it != m_properties.end())
        {
            return &it->second;
        }
        return nullptr;
    }
}
