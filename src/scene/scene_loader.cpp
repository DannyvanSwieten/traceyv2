#include "scene_loader.hpp"
#include <fstream>
#include <sstream>
#include <regex>
#include <stdexcept>

namespace tracey
{
    namespace
    {
        // Helper to extract string value from JSON
        std::string extractString(const std::string &json, const std::string &key)
        {
            std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
            std::smatch match;
            if (std::regex_search(json, match, pattern))
            {
                return match[1].str();
            }
            return "";
        }

        // Helper to extract float value from JSON
        float extractFloat(const std::string &json, const std::string &key, float defaultVal = 0.0f)
        {
            std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9.\\-eE+]+)");
            std::smatch match;
            if (std::regex_search(json, match, pattern))
            {
                return std::stof(match[1].str());
            }
            return defaultVal;
        }

        // Helper to extract an array of floats from JSON
        std::vector<float> extractFloatArray(const std::string &json, const std::string &key)
        {
            std::regex pattern("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
            std::smatch match;
            if (std::regex_search(json, match, pattern))
            {
                std::string arrayContent = match[1].str();
                std::vector<float> result;
                std::regex numPattern("[0-9.\\-eE+]+");
                std::sregex_iterator it(arrayContent.begin(), arrayContent.end(), numPattern);
                std::sregex_iterator end;
                while (it != end)
                {
                    result.push_back(std::stof(it->str()));
                    ++it;
                }
                return result;
            }
            return {};
        }

        // Helper to extract a JSON object block
        std::string extractObject(const std::string &json, const std::string &key)
        {
            std::regex pattern("\"" + key + "\"\\s*:\\s*\\{");
            std::smatch match;
            if (std::regex_search(json, match, pattern))
            {
                size_t startPos = match.position() + match.length() - 1;
                int braceCount = 0;
                size_t endPos = startPos;

                for (size_t i = startPos; i < json.size(); ++i)
                {
                    if (json[i] == '{')
                        braceCount++;
                    else if (json[i] == '}')
                        braceCount--;

                    if (braceCount == 0)
                    {
                        endPos = i + 1;
                        break;
                    }
                }
                return json.substr(startPos, endPos - startPos);
            }
            return "";
        }

        // Helper to extract a JSON array block
        std::string extractArray(const std::string &json, const std::string &key)
        {
            std::regex pattern("\"" + key + "\"\\s*:\\s*\\[");
            std::smatch match;
            if (std::regex_search(json, match, pattern))
            {
                size_t startPos = match.position() + match.length() - 1;
                int bracketCount = 0;
                size_t endPos = startPos;

                for (size_t i = startPos; i < json.size(); ++i)
                {
                    if (json[i] == '[')
                        bracketCount++;
                    else if (json[i] == ']')
                        bracketCount--;

                    if (bracketCount == 0)
                    {
                        endPos = i + 1;
                        break;
                    }
                }
                return json.substr(startPos, endPos - startPos);
            }
            return "";
        }

        // Helper to split array into elements (objects or primitive values)
        std::vector<std::string> splitArrayElements(const std::string &arrayJson)
        {
            std::vector<std::string> elements;
            if (arrayJson.empty() || arrayJson[0] != '[')
                return elements;

            int depth = 0;
            size_t elementStart = 1;
            bool inString = false;

            for (size_t i = 1; i < arrayJson.size() - 1; ++i)
            {
                char c = arrayJson[i];

                if (c == '"' && (i == 0 || arrayJson[i - 1] != '\\'))
                {
                    inString = !inString;
                }

                if (!inString)
                {
                    if (c == '{' || c == '[')
                        depth++;
                    else if (c == '}' || c == ']')
                        depth--;
                    else if (c == ',' && depth == 0)
                    {
                        std::string element = arrayJson.substr(elementStart, i - elementStart);
                        // Trim whitespace
                        size_t start = element.find_first_not_of(" \t\n\r");
                        size_t end = element.find_last_not_of(" \t\n\r");
                        if (start != std::string::npos)
                        {
                            elements.push_back(element.substr(start, end - start + 1));
                        }
                        elementStart = i + 1;
                    }
                }
            }

            // Last element
            std::string element = arrayJson.substr(elementStart, arrayJson.size() - 1 - elementStart);
            size_t start = element.find_first_not_of(" \t\n\r");
            size_t end = element.find_last_not_of(" \t\n\r");
            if (start != std::string::npos)
            {
                elements.push_back(element.substr(start, end - start + 1));
            }

            return elements;
        }

        // Helper to extract key-value pairs from a JSON object for "objects" section
        // Only extracts top-level entries (depth 1)
        std::vector<std::pair<std::string, std::string>> extractObjectEntries(const std::string &objectsJson)
        {
            std::vector<std::pair<std::string, std::string>> entries;
            if (objectsJson.empty() || objectsJson[0] != '{')
                return entries;

            // Parse manually to track depth and only extract top-level keys
            int depth = 0;
            bool inString = false;
            size_t i = 0;

            while (i < objectsJson.size())
            {
                char c = objectsJson[i];

                // Handle string escaping
                if (c == '"' && (i == 0 || objectsJson[i - 1] != '\\'))
                {
                    if (!inString)
                    {
                        // Starting a string - check if we're at depth 1 (top-level key)
                        if (depth == 1)
                        {
                            // Find the end of the key
                            size_t keyStart = i + 1;
                            size_t keyEnd = objectsJson.find('"', keyStart);
                            if (keyEnd == std::string::npos)
                                break;

                            std::string key = objectsJson.substr(keyStart, keyEnd - keyStart);

                            // Find the colon
                            size_t colonPos = objectsJson.find(':', keyEnd);
                            if (colonPos == std::string::npos)
                                break;

                            // Find the opening brace of the value object
                            size_t valueStart = objectsJson.find('{', colonPos);
                            if (valueStart == std::string::npos)
                            {
                                i = keyEnd + 1;
                                continue;
                            }

                            // Find the matching closing brace
                            int braceCount = 0;
                            size_t valueEnd = valueStart;
                            for (size_t j = valueStart; j < objectsJson.size(); ++j)
                            {
                                if (objectsJson[j] == '{')
                                    braceCount++;
                                else if (objectsJson[j] == '}')
                                    braceCount--;
                                if (braceCount == 0)
                                {
                                    valueEnd = j + 1;
                                    break;
                                }
                            }

                            entries.push_back({key, objectsJson.substr(valueStart, valueEnd - valueStart)});
                            i = valueEnd;
                            continue;
                        }
                    }
                    inString = !inString;
                }

                if (!inString)
                {
                    if (c == '{')
                    {
                        depth++;
                    }
                    else if (c == '}')
                    {
                        depth--;
                    }
                }
                i++;
            }

            return entries;
        }
    } // namespace

    std::unique_ptr<Scene> SceneLoader::loadFromFile(const std::string &path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open scene file: " + path);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return loadFromString(buffer.str());
    }

    std::unique_ptr<Scene> SceneLoader::loadFromString(const std::string &jsonContent)
    {
        auto scene = std::make_unique<Scene>();

        // Parse objects section
        std::string objectsJson = extractObject(jsonContent, "objects");
        if (!objectsJson.empty())
        {
            parseObjects(*scene, objectsJson);
        }

        // Parse actors section
        std::string actorsJson = extractArray(jsonContent, "actors");
        if (!actorsJson.empty())
        {
            parseActors(*scene, actorsJson);
        }

        return scene;
    }

    void SceneLoader::parseObjects(Scene &scene, const std::string &objectsJson)
    {
        auto entries = extractObjectEntries(objectsJson);
        for (const auto &[name, objJson] : entries)
        {
            SceneObject obj = parseObject(objJson);
            obj.setName(name);
            scene.addObject(name, std::move(obj));
        }
    }

    SceneObject SceneLoader::parseObject(const std::string &objectJson)
    {
        std::string type = extractString(objectJson, "type");

        if (type == "primitive")
        {
            std::string primitive = extractString(objectJson, "primitive");
            auto sizeArray = extractFloatArray(objectJson, "size");

            if (primitive == "cube")
            {
                float size = sizeArray.empty() ? 1.0f : sizeArray[0];
                return SceneObject::createCube(size);
            }
            else if (primitive == "plane")
            {
                float width = sizeArray.size() > 0 ? sizeArray[0] : 1.0f;
                float depth = sizeArray.size() > 1 ? sizeArray[1] : 1.0f;
                return SceneObject::createPlane(width, depth);
            }
            else if (primitive == "sphere")
            {
                float radius = sizeArray.empty() ? 1.0f : sizeArray[0];
                return SceneObject::createSphere(radius);
            }
        }
        else if (type == "mesh")
        {
            // Future: Load mesh from file
            std::string file = extractString(objectJson, "file");
            // For now, return an empty object - mesh loading will be added later
            SceneObject obj;
            return obj;
        }

        return SceneObject();
    }

    void SceneLoader::parseActors(Scene &scene, const std::string &actorsJson)
    {
        auto elements = splitArrayElements(actorsJson);
        for (const auto &actorJson : elements)
        {
            if (!actorJson.empty() && actorJson[0] == '{')
            {
                parseActor(scene, actorJson);
            }
        }
    }

    void SceneLoader::parseActor(Scene &scene, const std::string &actorJson)
    {
        Actor *actor = scene.createActor();

        // Parse name
        std::string name = extractString(actorJson, "name");
        actor->setName(name);

        // Parse transform
        std::string transformJson = extractObject(actorJson, "transform");
        if (!transformJson.empty())
        {
            Transform transform = parseTransform(transformJson);
            actor->setTransform(transform);
        }

        // Parse instances
        std::string instancesJson = extractArray(actorJson, "instances");
        if (!instancesJson.empty())
        {
            auto instanceElements = splitArrayElements(instancesJson);
            for (const auto &instanceJson : instanceElements)
            {
                if (!instanceJson.empty() && instanceJson[0] == '{')
                {
                    SceneInstance instance = parseInstance(instanceJson);
                    actor->addInstance(std::move(instance));
                }
            }
        }

        // Note: children parsing could be added for hierarchical scenes
    }

    SceneInstance SceneLoader::parseInstance(const std::string &instanceJson)
    {
        std::string objectRef = extractString(instanceJson, "object");
        std::string shaderId = extractString(instanceJson, "material_shader");

        std::string propsJson = extractObject(instanceJson, "material_properties");
        MaterialInstance material = parseMaterialProperties(propsJson, shaderId);

        SceneInstance instance(objectRef, material);

        // Parse optional instance transform
        std::string transformJson = extractObject(instanceJson, "transform");
        if (!transformJson.empty())
        {
            Transform transform = parseTransform(transformJson);
            instance.setLocalTransform(transform);
        }

        return instance;
    }

    MaterialInstance SceneLoader::parseMaterialProperties(const std::string &propsJson, const std::string &shaderId)
    {
        MaterialInstance material(shaderId);

        if (propsJson.empty())
            return material;

        // Parse common PBR properties
        auto albedoArray = extractFloatArray(propsJson, "albedo");
        if (albedoArray.size() >= 3)
        {
            material.setAlbedo(Vec3(albedoArray[0], albedoArray[1], albedoArray[2]));
        }

        float metallic = extractFloat(propsJson, "metallic", -1.0f);
        if (metallic >= 0.0f)
        {
            material.setMetallic(metallic);
        }

        float roughness = extractFloat(propsJson, "roughness", -1.0f);
        if (roughness >= 0.0f)
        {
            material.setRoughness(roughness);
        }

        auto emissionArray = extractFloatArray(propsJson, "emission");
        if (emissionArray.size() >= 3)
        {
            material.setEmission(Vec3(emissionArray[0], emissionArray[1], emissionArray[2]));
        }

        return material;
    }

    Transform SceneLoader::parseTransform(const std::string &transformJson)
    {
        Transform transform;

        auto posArray = extractFloatArray(transformJson, "position");
        if (posArray.size() >= 3)
        {
            transform.setPosition(Vec3(posArray[0], posArray[1], posArray[2]));
        }

        auto rotArray = extractFloatArray(transformJson, "rotation");
        if (rotArray.size() >= 3)
        {
            // Rotation is in degrees (Euler angles)
            transform.setRotation(Vec3(rotArray[0], rotArray[1], rotArray[2]));
        }

        auto scaleArray = extractFloatArray(transformJson, "scale");
        if (scaleArray.size() >= 3)
        {
            transform.setScale(Vec3(scaleArray[0], scaleArray[1], scaleArray[2]));
        }
        else if (scaleArray.size() == 1)
        {
            // Uniform scale
            transform.setScale(Vec3(scaleArray[0]));
        }

        return transform;
    }

    Vec3 SceneLoader::parseVec3(const std::string &arrayJson)
    {
        std::vector<float> values;
        std::regex numPattern("[0-9.\\-eE+]+");
        std::sregex_iterator it(arrayJson.begin(), arrayJson.end(), numPattern);
        std::sregex_iterator end;
        while (it != end && values.size() < 3)
        {
            values.push_back(std::stof(it->str()));
            ++it;
        }
        if (values.size() >= 3)
        {
            return Vec3(values[0], values[1], values[2]);
        }
        return Vec3(0.0f);
    }

    Vec4 SceneLoader::parseVec4(const std::string &arrayJson)
    {
        std::vector<float> values;
        std::regex numPattern("[0-9.\\-eE+]+");
        std::sregex_iterator it(arrayJson.begin(), arrayJson.end(), numPattern);
        std::sregex_iterator end;
        while (it != end && values.size() < 4)
        {
            values.push_back(std::stof(it->str()));
            ++it;
        }
        if (values.size() >= 4)
        {
            return Vec4(values[0], values[1], values[2], values[3]);
        }
        return Vec4(0.0f);
    }
}
