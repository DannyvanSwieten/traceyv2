#include "scene_instance.hpp"

namespace tracey
{
    SceneInstance::SceneInstance(const std::string &objectRef)
        : m_objectRef(objectRef)
    {
    }

    SceneInstance::SceneInstance(const std::string &objectRef, const MaterialInstance &material)
        : m_objectRef(objectRef), m_material(material)
    {
    }
}
