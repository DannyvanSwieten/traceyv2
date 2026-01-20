#pragma once
#include "../core/types.hpp"
#include <string>
#include <unordered_map>
#include <variant>
#include <optional>

namespace tracey
{
    // Standard PBR texture slot names
    constexpr const char *TEXTURE_ALBEDO = "albedoMap";
    constexpr const char *TEXTURE_NORMAL = "normalMap";
    constexpr const char *TEXTURE_METALLIC_ROUGHNESS = "metallicRoughnessMap";
    constexpr const char *TEXTURE_EMISSIVE = "emissiveMap";
    constexpr const char *TEXTURE_OCCLUSION = "occlusionMap";

    // Supported material property value types
    using MaterialValue = std::variant<float, Vec2, Vec3, Vec4, int, std::string>;

    class MaterialInstance
    {
    public:
        MaterialInstance() = default;
        MaterialInstance(const std::string &shaderId);

        const std::string &shaderId() const { return m_shaderId; }
        void setShaderId(const std::string &shaderId) { m_shaderId = shaderId; }

        // Property setters
        void setFloat(const std::string &name, float value);
        void setVec2(const std::string &name, const Vec2 &value);
        void setVec3(const std::string &name, const Vec3 &value);
        void setVec4(const std::string &name, const Vec4 &value);
        void setInt(const std::string &name, int value);
        void setTexture(const std::string &name, const std::string &texturePath);

        // Property getters
        std::optional<float> getFloat(const std::string &name) const;
        std::optional<Vec2> getVec2(const std::string &name) const;
        std::optional<Vec3> getVec3(const std::string &name) const;
        std::optional<Vec4> getVec4(const std::string &name) const;
        std::optional<int> getInt(const std::string &name) const;
        std::optional<std::string> getTexture(const std::string &name) const;

        // Generic access
        bool hasProperty(const std::string &name) const;
        const MaterialValue *getProperty(const std::string &name) const;
        const std::unordered_map<std::string, MaterialValue> &properties() const { return m_properties; }

        // Convenience methods for common PBR properties
        void setAlbedo(const Vec3 &albedo) { setVec3("albedo", albedo); }
        void setMetallic(float metallic) { setFloat("metallic", metallic); }
        void setRoughness(float roughness) { setFloat("roughness", roughness); }
        void setEmission(const Vec3 &emission) { setVec3("emission", emission); }

        std::optional<Vec3> albedo() const { return getVec3("albedo"); }
        std::optional<float> metallic() const { return getFloat("metallic"); }
        std::optional<float> roughness() const { return getFloat("roughness"); }
        std::optional<Vec3> emission() const { return getVec3("emission"); }

    private:
        std::string m_shaderId;
        std::unordered_map<std::string, MaterialValue> m_properties;

        // Future extension point for shader graphs
        // std::variant<std::string, std::unique_ptr<ShaderGraph>> m_shader;
    };
}
