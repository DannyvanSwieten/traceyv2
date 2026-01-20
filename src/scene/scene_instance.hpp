#pragma once
#include "material_instance.hpp"
#include "transform.hpp"
#include <string>
#include <optional>

namespace tracey
{
    class SceneInstance
    {
    public:
        SceneInstance() = default;
        SceneInstance(const std::string &objectRef);
        SceneInstance(const std::string &objectRef, const MaterialInstance &material);

        const std::string &objectRef() const { return m_objectRef; }
        void setObjectRef(const std::string &objectRef) { m_objectRef = objectRef; }

        const MaterialInstance &material() const { return m_material; }
        MaterialInstance &material() { return m_material; }
        void setMaterial(const MaterialInstance &material) { m_material = material; }

        const std::optional<Transform> &localTransform() const { return m_localTransform; }
        void setLocalTransform(const Transform &transform) { m_localTransform = transform; }
        void clearLocalTransform() { m_localTransform = std::nullopt; }
        bool hasLocalTransform() const { return m_localTransform.has_value(); }

    private:
        std::string m_objectRef;
        MaterialInstance m_material;
        std::optional<Transform> m_localTransform;
    };
}
