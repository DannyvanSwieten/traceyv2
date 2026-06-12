#include "../../src/device/device.hpp"
#include "../../src/scene/scene_loader.hpp"
#include "../../src/scene/scene_compiler.hpp"
#include "../../src/scene/gltf_loader.hpp"
#include "../../src/scene/camera.hpp"
#include "path_tracer/api/path_tracer.hpp"
#include "../../src/rendering/post_processing.hpp"
#include "../../src/graph/graphs/shader_graph/shader_graph.hpp"
#include "../../src/graph/graphs/shader_graph/nodes.hpp"
#include "../../src/graph/graphs/shader_graph/compiler.hpp"

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <vector>

int main(int argc, char *argv[])
{
    // Get scene path from command line argument
    std::filesystem::path scenePath;
    if (argc > 1)
    {
        scenePath = argv[1];
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
    config.useMaterialPrograms = true;

    // Get shader paths
    std::filesystem::path shaderDir = std::filesystem::path(__FILE__).parent_path() / "shaders";
    config.rayGenShader = shaderDir / "ray_gen.glsl";
    config.hitShader = shaderDir / "uber_hit.glsl";
    config.missShader = shaderDir / "sky_miss.glsl";
    config.resolveShader = shaderDir / "resolve.glsl";

    // Create path tracer
    std::cout << "Building pipeline from GLSL shader files..." << std::endl;
    tracey::PathTracer pathTracer(computeDevice.get(), config);
    std::cout << "Pipeline built successfully!" << std::endl;

    // Build a passthrough material graph and compile it. Replaces the
    // hand-built passthrough that PathTracer seeded by default. Identical
    // output is proof the graph -> bytecode -> GPU VM path works end-to-end.
    {
        tracey::ShaderGraph graph(0);
        // Passthrough graph: a single MaterialInput → MaterialOutput
        // wired one-to-one for the five PBR slots. Port indices come
        // from materialInputPorts() / materialOutputPorts() in
        // src/graph/graphs/shader_graph/nodes.hpp.
        struct PortPair { size_t inPort, outPort; };
        const PortPair pairs[] = {
            {7,  0},  // Albedo
            {8,  1},  // Metallic
            {9,  2},  // Roughness
            {10, 3},  // Emission
            {11, 4},  // InNormal → Normal
        };
        graph.addNode(std::make_unique<tracey::MaterialInputNode>(1));
        graph.addNode(std::make_unique<tracey::MaterialOutputNode>(2));
        for (const auto &p : pairs)
            graph.createConnection(1, p.inPort, 2, p.outPort);

        tracey::MaterialProgram prog = tracey::compileShaderGraph(graph);
        tracey::MaterialProgramBuffer programs;
        programs.addProgram(prog);
        pathTracer.setMaterialPrograms(programs);

        std::cout << "Loaded graph-compiled material program ("
                  << prog.code.size() << " instructions, "
                  << prog.parameterCount << " parameters)" << std::endl;
    }

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

        // Special case for Cornell box: place camera inside looking at back wall
        if (scenePath.stem().string().find("cornell_box") != std::string::npos)
        {
            glm::vec3 cameraPos(0.0f, 2.0f, 1.8f);   // Near front wall, inside scaled box (front wall at z=2)
            glm::vec3 lookTarget(0.0f, 2.0f, -2.0f); // Looking at back wall
            glm::vec3 cameraForward = glm::normalize(lookTarget - cameraPos);

            // Compute camera basis
            glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
            glm::vec3 cameraRight = glm::normalize(glm::cross(cameraForward, worldUp));
            glm::vec3 cameraUp = glm::cross(cameraRight, cameraForward);

            // Create lookAt rotation
            glm::mat4 lookAtMatrix = glm::lookAt(cameraPos, lookTarget, worldUp);
            glm::quat rotation = glm::quat_cast(glm::inverse(lookAtMatrix));

            camera.setPosition(cameraPos);
            camera.setRotation(rotation);
            camera.setFov(40.0f); // Standard FOV for Cornell box

            std::cout << "Cornell box interior camera: pos=(" << cameraPos.x << ", "
                      << cameraPos.y << ", " << cameraPos.z
                      << "), looking at back wall, fov=40" << std::endl;
        }
        // Special case for Sponza: place camera in center of atrium at ground level
        else if (scenePath.stem().string().find("Sponza") != std::string::npos)
        {
            glm::vec3 sceneCenter = (minBounds + maxBounds) * 0.5f;
            // Place camera at ground level (Y near minimum), centered in X/Z
            glm::vec3 cameraPos(sceneCenter.x, minBounds.y + 2.0f, sceneCenter.z);

            // Look down the +X axis (typical Sponza orientation)
            glm::vec3 lookTarget = cameraPos + glm::vec3(1.0f, 0.0f, 0.0f);
            glm::vec3 cameraForward = glm::normalize(lookTarget - cameraPos);

            // Compute camera basis
            glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
            glm::vec3 cameraRight = glm::normalize(glm::cross(cameraForward, worldUp));
            glm::vec3 cameraUp = glm::cross(cameraRight, cameraForward);

            // Create lookAt rotation
            glm::mat4 lookAtMatrix = glm::lookAt(cameraPos, lookTarget, worldUp);
            glm::quat rotation = glm::quat_cast(glm::inverse(lookAtMatrix));

            camera.setPosition(cameraPos);
            camera.setRotation(rotation);
            camera.setFov(60.0f); // Wider FOV for interior

            std::cout << "Sponza interior camera: pos=(" << cameraPos.x << ", "
                      << cameraPos.y << ", " << cameraPos.z
                      << "), looking along +X axis, fov=60" << std::endl;
        }
        else
        {
            // Fit camera to scene bounds
            camera = tracey::Camera::fitToBounds(minBounds, maxBounds, 45.0f);

            std::cout << "Auto-generated camera: pos=(" << camera.position().x << ", "
                      << camera.position().y << ", " << camera.position().z
                      << "), fov=" << camera.fov() << std::endl;
        }
    }

    // Render
    double renderTime = pathTracer.render(compiledScene, camera);
    std::cout << "Ray tracing execution time: " << renderTime << " ms" << std::endl;

    // outputImage now holds the resolve shader's tonemap+gamma'd snapshot
    // (already averaged across samples by the linear accumulator). With
    // hdrOutput=true the readback is float in [0,1]; we just need to
    // convert to uint8 for the PPM writer.
    std::vector<float> hdrData(config.width * config.height * 4);
    pathTracer.readback(hdrData.data());

    std::vector<uint8_t> ldrData(config.width * config.height * 4);
    for (size_t i = 0; i < hdrData.size(); ++i)
    {
        float v = std::clamp(hdrData[i], 0.0f, 1.0f);
        ldrData[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
    }

    // Save output
    std::string outputFileName = scenePath.stem().string() + ".ppm";
    tracey::PostProcessing::savePPM(outputFileName, ldrData.data(),
                                    config.width, config.height);

    std::cout << "Output saved to " << outputFileName << std::endl;

    return 0;
}
