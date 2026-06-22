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
//   pt_backend_compare [scene.glb] [--a metal] [--b cpu]
//                      [--spp 64] [--size 512] [--min-psnr 30]
//                      [--out prefix]
// Exit code 0 when PSNR >= threshold, 1 otherwise.

#include "device/device.hpp"
#include "scene/scene_loader.hpp"
#include "scene/scene_compiler.hpp"
#include "scene/gltf_loader.hpp"
#include "scene/usd_loader.hpp"
#include "scene/camera.hpp"
#include "path_tracer/api/path_tracer.hpp"
#include "path_tracer/api/backend_registry.hpp"
#include "graph/graphs/shader_graph/shader_graph.hpp"
#include "graph/graphs/shader_graph/nodes.hpp"
#include "graph/graphs/shader_graph/compiler.hpp"

#include <glm/glm.hpp>

#include <array>
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
    std::string backendA = "metal";
    std::string backendB = "cpu";
    uint32_t spp = 64;
    uint32_t size = 512;
    uint32_t bounces = 8;
    double minPsnr = 30.0;
    std::string outPrefix = "pt_compare";
    float clearcoat = -1.0f;  // >=0 injects a clearcoat factor on all materials (R3 lobe test)
    float sheen = -1.0f;      // >=0 injects a sheen factor on all materials (R3 lobe test)
    float subsurface = -1.0f; // >=0 injects a subsurface factor on all materials (R3 lobe test)
    float anisotropy = -2.0f; // >-2 injects an anisotropy factor [-1,1] on all materials (R3 lobe test)
    float aperture = 0.0f;    // >0 enables thin-lens DOF (R4 lens parity test)
    float focal = -1.0f;      // >0 overrides the focal distance (else camera default)
    float motionDx = 0.0f;    // !=0 translates all instances by (dx,0,0) over the shutter (R4 motion parity test)

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (arg == "--a") backendA = next();
        else if (arg == "--b") backendB = next();
        else if (arg == "--spp") spp = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--bounces") bounces = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--size") size = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--min-psnr") minPsnr = std::stod(next());
        else if (arg == "--out") outPrefix = next();
        else if (arg == "--clearcoat") clearcoat = std::stof(next());
        else if (arg == "--sheen") sheen = std::stof(next());
        else if (arg == "--subsurface") subsurface = std::stof(next());
        else if (arg == "--anisotropy") anisotropy = std::stof(next());
        else if (arg == "--aperture") aperture = std::stof(next());
        else if (arg == "--focal") focal = std::stof(next());
        else if (arg == "--motion") motionDx = std::stof(next());
        else scenePath = arg;
    }

    std::cout << "Scene: " << scenePath << "\nBackends: " << backendA << " vs " << backendB
              << "\nSamples: " << spp << ", size: " << size << "x" << size << std::endl;

    std::unique_ptr<tracey::Scene> scene;
    const std::string ext = scenePath.extension().string();
#ifdef TRACEY_HAS_USD
    if (ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz")
        scene = tracey::UsdLoader::loadFromFile(scenePath.string());
    else
#endif
    if (ext == ".gltf" || ext == ".glb")
        scene = tracey::GltfLoader::loadFromFile(scenePath);
    else
        scene = tracey::SceneLoader::loadFromFile(scenePath);

    std::unique_ptr<tracey::Device> device(
        tracey::createDevice(tracey::DeviceType::Gpu, tracey::DeviceBackend::Compute));

    tracey::SceneCompiler::CompiledScene compiled =
        tracey::SceneCompiler::compile(device.get(), *scene);
    if (clearcoat >= 0.0f)
    {
        // Force a clear coat on every material to validate the R3 coat lobe is
        // lockstep across backends (both read the same compiled materials).
        for (auto &m : compiled.materials)
        {
            m.clearcoatFactor = clearcoat;
            m.clearcoatRoughnessFactor = 0.1f;
        }
        std::cout << "Injected clearcoat=" << clearcoat << " on "
                  << compiled.materials.size() << " materials" << std::endl;
    }
    if (sheen >= 0.0f)
    {
        // Force sheen on every material to validate the R3 sheen lobe is
        // lockstep across backends (both read the same compiled materials).
        for (auto &m : compiled.materials)
            m.sheenFactor = sheen;
        std::cout << "Injected sheen=" << sheen << " on "
                  << compiled.materials.size() << " materials" << std::endl;
    }
    if (subsurface >= 0.0f)
    {
        // Force subsurface on every material to validate the R3d wrap-diffusion
        // lobe is lockstep across backends (scatter tint stays the default white).
        for (auto &m : compiled.materials)
            m.subsurfaceFactor = subsurface;
        std::cout << "Injected subsurface=" << subsurface << " on "
                  << compiled.materials.size() << " materials" << std::endl;
    }
    if (anisotropy > -2.0f)
    {
        // Force anisotropy + full metallic so the (gated) anisotropic GGX lobe
        // actually runs — validates it is lockstep across backends.
        for (auto &m : compiled.materials)
        {
            m.anisotropyFactor = anisotropy;
            m.metallicFactor = 1.0f;
        }
        std::cout << "Injected anisotropy=" << anisotropy << " (metallic=1) on "
                  << compiled.materials.size() << " materials" << std::endl;
    }
    if (motionDx != 0.0f)
    {
        // Translate every instance by (dx,0,0) over the shutter to validate the
        // motion-blur path is lockstep across backends (CPU swept-BVH interp vs
        // Metal hardware motion AS).
        compiled.instancesEnd = compiled.instances;
        for (auto &inst : compiled.instancesEnd) inst.transform[0][3] += motionDx;
        compiled.hasMotion = true;
        std::cout << "Motion: instances translated by dx=" << motionDx << " over shutter" << std::endl;
    }
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

    tracey::Camera camera = autoFitCamera(*scene);
    if (aperture > 0.0f)
    {
        // Validate the thin-lens DOF lobe is lockstep across backends.
        camera.setAperture(aperture);
        if (focal > 0.0f) camera.setFocalDistance(focal);
        std::cout << "DOF: aperture=" << aperture
                  << " focalDistance=" << camera.focalDistance() << std::endl;
    }

    constexpr size_t kAov = static_cast<size_t>(tracey::AovKind::Count);
    struct RenderOut
    {
        std::vector<float> beauty;
        std::array<std::vector<float>, kAov> aovs;
    };

    auto renderWith = [&](const std::string &backendName) -> RenderOut {
        tracey::PathTracerConfig config;
        config.width = size;
        config.height = size;
        config.hdrOutput = true;
        config.useMaterialPrograms = true;
        config.samplesPerFrame = 1;
        config.maxBounces = bounces;
        config.enableAovs = true;  // exercise + compare the AOV layers too
        config.backend = tracey::pathTracerBackendKindFromString(backendName);

        tracey::PathTracer tracer(device.get(), config);
        tracer.setMaterialPrograms(programs);

        for (uint32_t s = 0; s < spp; ++s)
        {
            const bool clear = (s == 0);
            const bool want = (s == spp - 1);
            tracer.render(compiled, camera, clear, want);
        }
        const size_t n4 = static_cast<size_t>(size) * size * 4;
        RenderOut out;
        out.beauty.resize(n4);
        tracer.readback(out.beauty.data());
        for (size_t k = 0; k < kAov; ++k)
        {
            out.aovs[k].resize(n4);
            tracer.readbackAOV(static_cast<tracey::AovKind>(k), out.aovs[k].data());
        }
        return out;
    };

    std::cout << "Rendering with '" << backendA << "'..." << std::endl;
    const RenderOut outA = renderWith(backendA);
    std::cout << "Rendering with '" << backendB << "'..." << std::endl;
    const RenderOut outB = renderWith(backendB);
    const std::vector<float> &imgA = outA.beauty;
    const std::vector<float> &imgB = outB.beauty;

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

    // ── AOV parity ──
    // AOVs are first-hit (primary visibility) only, so the two backends should
    // agree except at silhouette/edge pixels where the HW-RT vs BVH tie-break
    // picks a different triangle (the same source as the beauty divergence).
    // Gate the bounded denoiser guides (Albedo, Normal) on a loose mean-abs
    // tolerance — a systematic AOV bug moves the mean; edge pixels don't.
    static const char *kAovNames[kAov] = {"albedo", "normal", "depth",
                                           "position", "emission", "instanceId"};
    constexpr double kAovMeanTol = 0.02;
    bool aovFail = false;
    std::printf("AOV parity (mean |diff| / max |diff|):\n");
    for (size_t k = 0; k < kAov; ++k)
    {
        double aSum = 0.0, aMax = 0.0;
        for (size_t i = 0; i < pixelCount; ++i)
            for (int c = 0; c < 4; ++c)
            {
                const double d = double(outA.aovs[k][i * 4 + c]) -
                                 double(outB.aovs[k][i * 4 + c]);
                aSum += std::abs(d);
                aMax = std::max(aMax, std::abs(d));
            }
        const double aMean = aSum / (pixelCount * 4.0);
        const bool gated = (k == size_t(tracey::AovKind::Albedo) ||
                            k == size_t(tracey::AovKind::Normal));
        const bool bad = gated && aMean > kAovMeanTol;
        std::printf("  %-11s %.6f / %.4f%s\n", kAovNames[k], aMean, aMax,
                    bad ? "  <-- FAIL" : "");
        aovFail = aovFail || bad;
    }

    if (psnr < minPsnr)
    {
        std::printf("FAIL: PSNR %.2f below threshold %.2f\n", psnr, minPsnr);
        return 1;
    }
    if (aovFail)
    {
        std::printf("FAIL: AOV mean |diff| exceeded %.3f\n", kAovMeanTol);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
