#pragma once
#include "scene.hpp"
#include <string>
#include <memory>

namespace tracey
{
    class GltfLoader
    {
    public:
        // Load a GLTF/GLB file and convert it to our Scene format
        static std::unique_ptr<Scene> loadFromFile(const std::string &path);

        // Options for loading
        struct LoadOptions
        {
            bool loadMaterials = true;
            bool loadNormals = true;
            bool loadTexCoords = true;
            float scaleFactor = 1.0f;
        };

        static std::unique_ptr<Scene> loadFromFile(const std::string &path, const LoadOptions &options);
    };
}
