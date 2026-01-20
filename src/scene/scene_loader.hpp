#pragma once
#include "scene.hpp"
#include <memory>
#include <string>

namespace tracey
{
    class SceneLoader
    {
    public:
        static std::unique_ptr<Scene> loadFromFile(const std::string &path);
        static std::unique_ptr<Scene> loadFromString(const std::string &jsonContent);

    private:
        static void parseObjects(Scene &scene, const std::string &objectsJson);
        static void parseActors(Scene &scene, const std::string &actorsJson);
        static SceneObject parseObject(const std::string &objectJson);
        static void parseActor(Scene &scene, const std::string &actorJson);
        static SceneInstance parseInstance(const std::string &instanceJson);
        static MaterialInstance parseMaterialProperties(const std::string &propsJson, const std::string &shaderId);
        static Transform parseTransform(const std::string &transformJson);
        static Vec3 parseVec3(const std::string &arrayJson);
        static Vec4 parseVec4(const std::string &arrayJson);
    };
}
