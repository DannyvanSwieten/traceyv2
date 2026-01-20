#include "../../src/device/device.hpp"
#include "../../src/scene/scene_loader.hpp"
#include "../../src/scene/scene_compiler.hpp"
#include "../../src/scene/gltf_loader.hpp"
#include "../../src/scene/camera.hpp"
#include "../../src/rendering/path_tracer.hpp"
#include "../../src/rendering/post_processing.hpp"

#include <iostream>
#include <filesystem>
#include <vector>

int main(int argc, char *argv[])
{
    // Get scene path from command line argument
    std::filesystem::path scenePath;
    if (argc > 1)
    {
        scenePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "scenes" / argv[1];
    }
    else
    {
        // Default to the example scene
        scenePath = std::filesystem::path(__FILE__).parent_path().parent_path() / "scenes" / "DamagedHelmet.glb";
    }

    std::cout << "Loading scene from: " << scenePath << std::endl;

    // Load scene based on file extension
    std::unique_ptr<tracey::Scene> scene;
    try
    {
        std::string ext = scenePath.extension().string();
        if (ext == ".gltf" || ext == ".glb")
        {
            scene = tracey::GltfLoader::loadFromFile(scenePath);
        }
        else if (ext == ".json")
        {
            scene = tracey::SceneLoader::loadFromFile(scenePath);
        }
        else
        {
            std::cerr << "Unsupported scene file format: " << ext << std::endl;
            return 1;
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to load scene: " << e.what() << std::endl;
        return 1;
    }

    // Create compute device
    std::unique_ptr<tracey::Device> computeDevice = std::unique_ptr<tracey::Device>(
        tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute));

    // Compile scene to acceleration structures
    std::cout << "Compiling scene to acceleration structures..." << std::endl;
    tracey::SceneCompiler::CompiledScene compiledScene =
        tracey::SceneCompiler::compile(computeDevice.get(), *scene);

    std::cout << "Scene compiled successfully!" << std::endl;
    std::cout << "  BLAS count: " << compiledScene.blases.size() << std::endl;
    std::cout << "  Instance count: " << compiledScene.instances.size() << std::endl;
    std::cout << "  Material count: " << compiledScene.materials.size() << std::endl;
    std::cout << "  Texture count: " << compiledScene.textures.size() << std::endl;
    std::cout << "  Has UVs: " << (compiledScene.uvBuffer ? "yes" : "no") << std::endl;

    // Configure path tracer
    tracey::PathTracerConfig config;
    config.width = 512;
    config.height = 512;
    config.hdrOutput = true;

    // Get shader paths
    std::filesystem::path shaderDir = std::filesystem::path(__FILE__).parent_path() / "shaders";
    config.rayGenShader = shaderDir / "ray_gen.isf";
    config.hitShader = shaderDir / "diffuse_hit.isf";
    config.missShader = shaderDir / "sky_miss.isf";
    config.resolveShader = shaderDir / "resolve.isf";

    // Create path tracer
    std::cout << "Building pipeline from ISF shader files..." << std::endl;
    tracey::PathTracer pathTracer(computeDevice.get(), config);
    std::cout << "Pipeline built successfully!" << std::endl;

    // Setup camera
    tracey::Camera camera;
    if (scene->hasCamera())
    {
        camera = scene->camera();
        std::cout << "Using camera from scene: pos=(" << camera.position().x << ", "
                  << camera.position().y << ", " << camera.position().z
                  << "), fov=" << camera.fov() << " degrees" << std::endl;
    }
    else
    {
        // Compute scene bounding box for auto-fitted camera
        glm::vec3 minBounds(std::numeric_limits<float>::max());
        glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
        bool foundGeometry = false;

        // Compute bounds from scene
        auto sceneNodes = scene->flatten();
        for (const auto &node : sceneNodes)
        {
            for (const auto &instance : node.actor->instances())
            {
                const auto *obj = scene->getObject(instance.objectRef());
                if (!obj)
                    continue;

                foundGeometry = true;

                // Compute instance world transform
                glm::mat4 worldTransform = node.worldTransform;
                if (instance.hasLocalTransform())
                {
                    worldTransform = worldTransform * instance.localTransform()->toMatrix();
                }

                // Transform all vertices and expand bounds
                for (const auto &pos : obj->positions())
                {
                    glm::vec4 worldPos = worldTransform * glm::vec4(pos, 1.0f);
                    minBounds = glm::min(minBounds, glm::vec3(worldPos));
                    maxBounds = glm::max(maxBounds, glm::vec3(worldPos));
                }
            }
        }

        // Fallback: use raw object geometry
        if (!foundGeometry)
        {
            for (const auto &[name, obj] : scene->objects())
            {
                for (const auto &pos : obj->positions())
                {
                    minBounds = glm::min(minBounds, pos);
                    maxBounds = glm::max(maxBounds, pos);
                    foundGeometry = true;
                }
            }
        }

        // If still no geometry, use default view
        if (!foundGeometry)
        {
            minBounds = glm::vec3(-10.0f);
            maxBounds = glm::vec3(10.0f);
        }

        std::cout << "Scene bounds: min=(" << minBounds.x << ", " << minBounds.y << ", " << minBounds.z << ")"
                  << " max=(" << maxBounds.x << ", " << maxBounds.y << ", " << maxBounds.z << ")" << std::endl;

        // Fit camera to scene bounds
        camera = tracey::Camera::fitToBounds(minBounds, maxBounds, 45.0f);

        std::cout << "Auto-generated camera: pos=(" << camera.position().x << ", "
                  << camera.position().y << ", " << camera.position().z
                  << "), fov=" << camera.fov() << std::endl;
    }

    // Render
    double renderTime = pathTracer.render(compiledScene, camera);
    std::cout << "Ray tracing execution time: " << renderTime << " ms" << std::endl;

    // Readback HDR image
    std::vector<float> hdrData(config.width * config.height * 4);
    pathTracer.readback(hdrData.data());

    // Tone map to LDR
    std::vector<uint8_t> ldrData(config.width * config.height * 4);
    tracey::ToneMapSettings toneMapSettings;
    toneMapSettings.exposure = 0.2f;
    toneMapSettings.gamma = 2.2f;
    toneMapSettings.toneMapOperator = tracey::ToneMapSettings::Operator::ACES;

    tracey::PostProcessing::toneMap(hdrData.data(), ldrData.data(),
                                   config.width, config.height,
                                   toneMapSettings);

    // Save output
    std::string outputFileName = scenePath.stem().string() + ".ppm";
    tracey::PostProcessing::savePPM(outputFileName, ldrData.data(),
                                   config.width, config.height);

    std::cout << "Output saved to " << outputFileName << std::endl;

    return 0;
}
