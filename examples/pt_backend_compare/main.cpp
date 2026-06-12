// Path tracer backend parity harness.
//
// Renders the same scene through two PathTracer backends (default:
// wavefront vs metal) with identical camera, sample schedule, and material
// programs, then compares the converged outputs. The RNG seed formula and
// hash are bit-exact across backends, so differences are limited to float
// contraction/precision and intersector tie-breaks at geometric edges —
// converged images must agree to a tight PSNR.
//
// Usage:
//   pt_backend_compare [scene.glb] [--a wavefront] [--b metal]
//                      [--spp 64] [--size 512] [--min-psnr 30]
//                      [--out prefix]
// Exit code 0 when PSNR >= threshold, 1 otherwise.

#include "device/device.hpp"
#include "scene/scene_loader.hpp"
#include "scene/scene_compiler.hpp"
#include "scene/gltf_loader.hpp"
#include "scene/camera.hpp"
#include "path_tracer/api/path_tracer.hpp"
#include "path_tracer/api/backend_registry.hpp"
#include "graph/graphs/shader_graph/shader_graph.hpp"
#include "graph/graphs/shader_graph/nodes.hpp"
#include "graph/graphs/shader_graph/compiler.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
    tracey::Camera autoFitCamera(tracey::Scene &scene)
    {
        if (scene.hasCamera()) return scene.camera();

        glm::vec3 minB(std::numeric_limits<float>::max());
        glm::vec3 maxB(std::numeric_limits<float>::lowest());
        bool found = false;
        for (const auto &node : scene.flatten())
        {
            for (const auto &instance : node.actor->instances())
            {
                const auto *obj = scene.getObject(instance.objectRef());
                if (!obj) continue;
                found = true;
                glm::mat4 world = node.worldTransform;
                if (instance.hasLocalTransform())
                    world = world * instance.localTransform()->toMatrix();
                for (const auto &pos : obj->positions())
                {
                    glm::vec4 wp = world * glm::vec4(pos, 1.0f);
                    minB = glm::min(minB, glm::vec3(wp));
                    maxB = glm::max(maxB, glm::vec3(wp));
                }
            }
        }
        if (!found)
        {
            minB = glm::vec3(-10.0f);
            maxB = glm::vec3(10.0f);
        }

        const glm::vec3 center = (minB + maxB) * 0.5f;
        const float radius = glm::length(maxB - minB) * 0.5f;
        const glm::vec3 pos = center + glm::vec3(0.0f, radius * 0.3f, radius * 2.2f);

        tracey::Camera cam;
        cam.setPosition(pos);
        // lookAt: forward toward the center.
        const glm::vec3 fwd = glm::normalize(center - pos);
        // Build a quaternion from forward (default camera looks down -Z).
        const glm::quat q = glm::quatLookAt(fwd, glm::vec3(0, 1, 0));
        cam.setRotation(q);
        cam.setFov(45.0f);
        return cam;
    }

    void writePpm(const std::string &path, const std::vector<float> &rgba,
                  uint32_t w, uint32_t h)
    {
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << w << " " << h << "\n255\n";
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
        {
            for (int c = 0; c < 3; ++c)
            {
                const float v = std::min(std::max(rgba[i * 4 + c], 0.0f), 1.0f);
                out.put(static_cast<char>(std::lround(v * 255.0f)));
            }
        }
    }
}

int main(int argc, char *argv[])
{
    std::filesystem::path scenePath =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "scenes" / "DamagedHelmet.glb";
    std::string backendA = "wavefront";
    std::string backendB = "metal";
    uint32_t spp = 64;
    uint32_t size = 512;
    double minPsnr = 30.0;
    std::string outPrefix = "pt_compare";

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (arg == "--a") backendA = next();
        else if (arg == "--b") backendB = next();
        else if (arg == "--spp") spp = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--size") size = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--min-psnr") minPsnr = std::stod(next());
        else if (arg == "--out") outPrefix = next();
        else scenePath = arg;
    }

    std::cout << "Scene: " << scenePath << "\nBackends: " << backendA << " vs " << backendB
              << "\nSamples: " << spp << ", size: " << size << "x" << size << std::endl;

    std::unique_ptr<tracey::Scene> scene;
    const std::string ext = scenePath.extension().string();
    if (ext == ".gltf" || ext == ".glb")
        scene = tracey::GltfLoader::loadFromFile(scenePath);
    else
        scene = tracey::SceneLoader::loadFromFile(scenePath);

    std::unique_ptr<tracey::Device> device(
        tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute));

    tracey::SceneCompiler::CompiledScene compiled =
        tracey::SceneCompiler::compile(device.get(), *scene);
    std::cout << "Compiled: " << compiled.instances.size() << " instances, "
              << compiled.blases.size() << " BLASes, "
              << compiled.textures.size() << " textures, "
              << compiled.lights.size() << " lights" << std::endl;

    // Same passthrough material program for both backends (mirrors
    // scene_renderer's setup; instanceProgramIndex is all-zero for plain
    // glTF loads so one program covers every instance).
    tracey::MaterialProgramBuffer programs;
    {
        tracey::ShaderGraph graph(0);
        struct PortPair { size_t inPort, outPort; };
        const PortPair pairs[] = {{7, 0}, {8, 1}, {9, 2}, {10, 3}, {11, 4}};
        graph.addNode(std::make_unique<tracey::MaterialInputNode>(1));
        graph.addNode(std::make_unique<tracey::MaterialOutputNode>(2));
        for (const auto &p : pairs) graph.createConnection(1, p.inPort, 2, p.outPort);
        programs.addProgram(tracey::compileShaderGraph(graph));
    }

    const tracey::Camera camera = autoFitCamera(*scene);

    const std::filesystem::path shaderDir =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "scene_renderer" / "shaders";

    auto renderWith = [&](const std::string &backendName) -> std::vector<float> {
        tracey::PathTracerConfig config;
        config.width = size;
        config.height = size;
        config.hdrOutput = true;
        config.useMaterialPrograms = true;
        config.samplesPerFrame = 1;
        config.maxBounces = 8;
        config.rayGenShader = shaderDir / "ray_gen.glsl";
        config.hitShader = shaderDir / "uber_hit.glsl";
        config.missShader = shaderDir / "sky_miss.glsl";
        config.resolveShader = shaderDir / "resolve.glsl";
        config.backend = tracey::pathTracerBackendKindFromString(backendName);

        tracey::PathTracer tracer(device.get(), config);
        tracer.setMaterialPrograms(programs);

        for (uint32_t s = 0; s < spp; ++s)
        {
            const bool clear = (s == 0);
            const bool want = (s == spp - 1);
            tracer.render(compiled, camera, clear, want);
        }
        std::vector<float> pixels(static_cast<size_t>(size) * size * 4);
        tracer.readback(pixels.data());
        return pixels;
    };

    std::cout << "Rendering with '" << backendA << "'..." << std::endl;
    const std::vector<float> imgA = renderWith(backendA);
    std::cout << "Rendering with '" << backendB << "'..." << std::endl;
    const std::vector<float> imgB = renderWith(backendB);

    // ── Compare ──
    double sumSq = 0.0;
    double sumAbs = 0.0;
    double maxAbs = 0.0;
    const size_t pixelCount = static_cast<size_t>(size) * size;
    for (size_t i = 0; i < pixelCount; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            const double d = double(imgA[i * 4 + c]) - double(imgB[i * 4 + c]);
            sumSq += d * d;
            sumAbs += std::abs(d);
            maxAbs = std::max(maxAbs, std::abs(d));
        }
    }
    const double mse = sumSq / (pixelCount * 3.0);
    const double psnr = mse > 0.0 ? 10.0 * std::log10(1.0 / mse)
                                  : std::numeric_limits<double>::infinity();
    const double meanAbs = sumAbs / (pixelCount * 3.0);

    writePpm(outPrefix + "_a.ppm", imgA, size, size);
    writePpm(outPrefix + "_b.ppm", imgB, size, size);
    // Amplified diff for eyeballing.
    {
        std::vector<float> diff(pixelCount * 4, 1.0f);
        for (size_t i = 0; i < pixelCount; ++i)
            for (int c = 0; c < 3; ++c)
                diff[i * 4 + c] = std::min(
                    std::abs(imgA[i * 4 + c] - imgB[i * 4 + c]) * 8.0f, 1.0f);
        writePpm(outPrefix + "_diff.ppm", diff, size, size);
    }

    std::printf("PSNR: %.2f dB | mean |diff|: %.6f | max |diff|: %.4f\n",
                psnr, meanAbs, maxAbs);
    std::printf("Images: %s_a.ppm / %s_b.ppm / %s_diff.ppm\n",
                outPrefix.c_str(), outPrefix.c_str(), outPrefix.c_str());

    if (psnr < minPsnr)
    {
        std::printf("FAIL: PSNR %.2f below threshold %.2f\n", psnr, minPsnr);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
