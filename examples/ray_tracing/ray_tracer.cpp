#include <functional>
#include <iostream>
#include <vector>
#include <fstream>
#include "../../src/blas.hpp"
#include "../../src/tlas.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>

using ShaderCallback = std::function<void(tracey::UVec2 pixelCoord, const tracey::Tlas &tlas)>;

void trace(uint32_t width, uint32_t height, const tracey::Tlas &tlas, const ShaderCallback shader)
{
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            tracey::UVec2 pixelCoord(x, y);
            shader(pixelCoord, tlas);
        }
    }
}

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
    tracey::Tlas::Instance instance;
    instance.blasIndex = 0;
    instance.instanceId = 0;
    instance.transform = glm::translate(tracey::Vec3(0, 0, 5)) * glm::rotate(glm::radians(30.0f), tracey::Vec3(0, 1, 0)) * glm::rotate(glm::radians(30.0f), tracey::Vec3(-1, 0, 0));
    tracey::Tlas tlas(std::span<const tracey::Blas>(&blas, 1), std::span<const tracey::Tlas::Instance>(&instance, 1));

    const uint32_t imageWidth = 512;
    const uint32_t imageHeight = 512;

    std::vector<tracey::Vec3> framebuffer(imageWidth * imageHeight);

    // Setup shader callback
    const auto shader = [&framebuffer, &cube, imageWidth, imageHeight](tracey::UVec2 pixelCoord, const tracey::Tlas &tlas)
    {
        // Simple pinhole camera ray generation
        float fov = 45.0f;
        float aspectRatio = 1.0f; // Assume square for simplicity
        float px = (2.0f * ((pixelCoord.x + 0.5f) / imageWidth) - 1.0f) * tan(glm::radians(fov) / 2.0f) * aspectRatio;
        float py = (1.0f - 2.0f * ((pixelCoord.y + 0.5f) / imageHeight)) * tan(glm::radians(fov) / 2.0f);
        tracey::Vec3 rayDir = glm::normalize(tracey::Vec3(px, py, 1.0f));
        tracey::Ray ray;
        ray.origin = tracey::Vec3(0, 0, 0);
        ray.direction = rayDir;
        const auto tMin = 0.0f;
        const auto tMax = 100.0f;
        tracey::RayFlags flags = tracey::RAY_FLAG_OPAQUE;
        if (const auto intersection = tlas.intersect(ray, tMin, tMax, flags); intersection)
        {
            // Simple shading based on the normal (here we just use the hit position for demonstration)
            tracey::Vec3 color = glm::normalize(intersection->position);
            // Calculate normal
            const auto v0 = cube[intersection->primitiveId * 3 + 0];
            const auto v1 = cube[intersection->primitiveId * 3 + 1];
            const auto v2 = cube[intersection->primitiveId * 3 + 2];
            const auto edge1 = v1 - v0;
            const auto edge2 = v2 - v0;
            const auto normal = glm::normalize(glm::cross(edge1, edge2));

            const auto I = glm::dot(glm::normalize(tracey::Vec3(0, 1, -1)), normal);
            framebuffer[pixelCoord.y * imageWidth + pixelCoord.x] = tracey::Vec3(std::max(I, 0.0f)); // Map to [0,1]
        }
        else
        {
            // create a simple sky gradient
            float t = 0.5f * (ray.direction.y + 1.0f);
            framebuffer[pixelCoord.y * imageWidth + pixelCoord.x] = (1.0f - t) * tracey::Vec3(1.0f) + t * tracey::Vec3(0.5f, 0.7f, 1.0f); // Sky gradient
        }
    };

    // Trace rays
    trace(imageWidth, imageHeight, tlas, shader);

    // Save framebuffer to PPM image
    std::ofstream ofs("output.ppm", std::ios::out | std::ios::binary);
    ofs << "P6\n"
        << imageWidth << " " << imageHeight << "\n255\n";
    for (const auto &color : framebuffer)
    {
        ofs << static_cast<unsigned char>(glm::clamp(color.r, 0.0f, 1.0f) * 255)
            << static_cast<unsigned char>(glm::clamp(color.g, 0.0f, 1.0f) * 255)
            << static_cast<unsigned char>(glm::clamp(color.b, 0.0f, 1.0f) * 255);
    }
    ofs.close();
}