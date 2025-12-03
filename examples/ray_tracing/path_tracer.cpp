#include <functional>
#include <iostream>
#include <vector>
#include <fstream>
#include "../../src/core/blas.hpp"
#include "../../src/core/tlas.hpp"
#include "../../src/shading/random/sampling.hpp"
#include "../../src/shading/onb.hpp"
#include "../../src/shading/bsdf/pbr/pbr_bsdf.hpp"
#include "../../src/ray_tracing/trace.hpp"

int main()
{
    // Define a simple centered cube made of 36 triangles without indices.
    std::array<tracey::Vec3, 36> cube = {
        // Front face
        tracey::Vec3{-1, -1, 1},
        tracey::Vec3{1, -1, 1},
        tracey::Vec3{1, 1, 1},
        tracey::Vec3{-1, -1, 1},
        tracey::Vec3{1, 1, 1},
        tracey::Vec3{-1, 1, 1},
        // Back face
        tracey::Vec3{1, -1, -1},
        tracey::Vec3{-1, -1, -1},
        tracey::Vec3{-1, 1, -1},
        tracey::Vec3{1, -1, -1},
        tracey::Vec3{-1, 1, -1},
        tracey::Vec3{1, 1, -1},
        // Left face
        tracey::Vec3{-1, -1, -1},
        tracey::Vec3{-1, -1, 1},
        tracey::Vec3{-1, 1, 1},
        tracey::Vec3{-1, -1, -1},
        tracey::Vec3{-1, 1, 1},
        tracey::Vec3{-1, 1, -1},
        // Right face
        tracey::Vec3{1, -1, 1},
        tracey::Vec3{1, -1, -1},
        tracey::Vec3{1, 1, -1},
        tracey::Vec3{1, -1, 1},
        tracey::Vec3{1, 1, -1},
        tracey::Vec3{1, 1, 1},
        // Top face
        tracey::Vec3{-1, 1, 1},
        tracey::Vec3{1, 1, 1},
        tracey::Vec3{1, 1, -1},
        tracey::Vec3{-1, 1, 1},
        tracey::Vec3{1, 1, -1},
        tracey::Vec3{-1, 1, -1},
        // Bottom face
        tracey::Vec3{-1, -1, -1},
        tracey::Vec3{1, -1, -1},
        tracey::Vec3{1, -1, 1},
        tracey::Vec3{-1, -1, -1},
        tracey::Vec3{1, -1, 1},
        tracey::Vec3{-1, -1, 1},
    };

    tracey::Blas blas(cube);
    // Create a TLAS with one instance of the triangle
    std::array<tracey::Tlas::Instance, 2> instances;
    instances[0].blasIndex = 0;
    instances[0].instanceId = 0;
    instances[0].transform = glm::translate(tracey::Vec3(0, 0, 5)) * glm::rotate(glm::radians(30.0f), tracey::Vec3(0, 1, 0)) * glm::rotate(glm::radians(30.0f), tracey::Vec3(-1, 0, 0));
    instances[1].blasIndex = 0;
    instances[1].instanceId = 1;
    instances[1].transform = glm::translate(tracey::Vec3(0, -3, 5)) * glm::scale(tracey::Vec3(10, 1, 10));
    tracey::Tlas tlas(std::span<const tracey::Blas>(&blas, 1), instances);

    const uint32_t imageWidth = 512;
    const uint32_t imageHeight = 512;

    std::vector<tracey::Vec3> framebuffer(imageWidth * imageHeight);

    // Setup shader callback
    const auto shader = [&instances, &framebuffer, &cube, imageWidth, imageHeight](const tracey::UVec2 &pixelCoord, const tracey::UVec2 &resolution, const tracey::Tlas &tlas)
    {
        // Simple pinhole camera ray generation
        float fov = 45.0f;
        float aspectRatio = static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
        float px = (2.0f * ((pixelCoord.x + 0.5f) / imageWidth) - 1.0f) * tan(glm::radians(fov) / 2.0f) * aspectRatio;
        float py = (1.0f - 2.0f * ((pixelCoord.y + 0.5f) / imageHeight)) * tan(glm::radians(fov) / 2.0f);

        const auto tMin = 0.001f;
        const auto tMax = 100.0f;
        const auto maxDepth = 8;
        const auto numSamples = 4;

        tracey::Vec3 accumulatedColor(0.0f);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            tracey::Vec3 radiance(0.0f);
            tracey::RNG rng(static_cast<uint32_t>(pixelCoord.x + pixelCoord.y * imageWidth + sample * 9973));
            tracey::Vec3 jitter = tracey::Vec3(rng.next(), rng.next(), 0.0f) / tracey::Vec3(static_cast<float>(imageWidth - 1), static_cast<float>(imageHeight - 1), 1.0f);
            tracey::Ray ray;
            ray.origin = tracey::Vec3(0, 0, 0);
            ray.direction = tracey::normalize(tracey::Vec3(px, py, 1.0f) + jitter);
            tracey::Vec3 throughput = tracey::Vec3(1.0f);
            tracey::PBRMaterial material;

            for (int depth = 0; depth < maxDepth; ++depth)
            {
                // create a random ray direction with slight jitter for anti-aliasing
                tracey::RayFlags flags = tracey::RAY_FLAG_OPAQUE;
                if (const auto intersection = tlas.intersect(ray, tMin, tMax, flags); intersection)
                {
                    // Calculate normal
                    const auto &instance = instances[intersection->instanceId];
                    // Transform triangle vertices to world space
                    const auto v0 = instance.transform * tracey::Vec4(cube[intersection->primitiveId * 3 + 0], 1.0f);
                    const auto v1 = instance.transform * tracey::Vec4(cube[intersection->primitiveId * 3 + 1], 1.0f);
                    const auto v2 = instance.transform * tracey::Vec4(cube[intersection->primitiveId * 3 + 2], 1.0f);
                    const auto edge1 = tracey::Vec3(v1 - v0);
                    const auto edge2 = tracey::Vec3(v2 - v0);
                    auto normal = tracey::normalize(tracey::cross(edge1, edge2));
                    if (tracey::dot(normal, -ray.direction) < 0.0f)
                        normal = -normal;
                    tracey::Vec3 V = tracey::normalize(-ray.direction);

                    // Optional: emission (none for cube)
                    // radiance += throughput * emission;

                    // Sample BSDF
                    auto s = tracey::sampleBRDF(normal, V, rng, material);
                    if (s.pdf <= 0.0f || tracey::length2(s.f) == 0.0f)
                        break;

                    float NdotL = tracey::max(tracey::dot(normal, s.wi), 0.0f);
                    if (NdotL <= 0.0f)
                        break;

                    // Throughput update: f * cos / pdf
                    throughput *= s.f * (NdotL / s.pdf);

                    // Russian roulette
                    if (depth >= 3)
                    {
                        float p = tracey::clamp(tracey::luminance(throughput), 0.0f, 0.95f);
                        if (rng.next() > p)
                            break;

                        throughput /= p;
                    }

                    // Spawn next ray
                    const auto hitPosition = ray.origin + ray.direction * intersection->t;
                    ray.origin = hitPosition + normal * 1e-4f; // offset to avoid self-intersection
                    ray.direction = tracey::normalize(s.wi);
                }
                else
                {
                    // create a simple sky gradient
                    float t = 0.5f * (ray.direction.y + 1.0f);
                    const auto sky = (1.0f - t) * tracey::Vec3(1.0f) + t * tracey::Vec3(0.5f, 0.7f, 1.0f);
                    radiance += throughput * sky;
                    break;
                }
            }

            accumulatedColor += radiance;
        }

        framebuffer[pixelCoord.y * imageWidth + pixelCoord.x] = accumulatedColor / static_cast<float>(numSamples);
    };

    // Trace rays
    traceRays(tracey::UVec2(imageWidth, imageHeight), shader, tlas);

    // Save framebuffer to PPM image
    std::ofstream ofs("path_tracer.ppm", std::ios::out | std::ios::binary);
    ofs << "P6\n"
        << imageWidth << " " << imageHeight << "\n255\n";
    for (const auto &color : framebuffer)
    {
        ofs << static_cast<unsigned char>(tracey::saturate(color.r) * 255)
            << static_cast<unsigned char>(tracey::saturate(color.g) * 255)
            << static_cast<unsigned char>(tracey::saturate(color.b) * 255);
    }
    ofs.close();
}