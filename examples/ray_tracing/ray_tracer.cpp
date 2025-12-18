#include <functional>
#include <iostream>
#include <vector>
#include <fstream>
#include "../../src/core/blas.hpp"
#include "../../src/core/tlas.hpp"

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
    const tracey::Blas *blasPtr = &blas;
    // Create a TLAS with one instance of the triangle
    std::array<tracey::Tlas::Instance, 2> instances;
    instances[0].blasAddress = 0;
    instances[0].instanceCustomIndexAndMask = 0;
    instances[0].setTransform(glm::translate(tracey::Vec3(0, 0, 5)) * glm::rotate(glm::radians(30.0f), tracey::Vec3(0, 1, 0)) * glm::rotate(glm::radians(30.0f), tracey::Vec3(-1, 0, 0)));
    instances[1].blasAddress = 0;
    instances[1].instanceCustomIndexAndMask = 1;
    instances[1].setTransform(glm::translate(tracey::Vec3(0, -3, 5)) * glm::scale(tracey::Vec3(10, 1, 10)));
    tracey::Tlas tlas(std::span<const tracey::Blas *>(&blasPtr, 1), instances);

    const uint32_t imageWidth = 512;
    const uint32_t imageHeight = 512;

    std::vector<tracey::Vec3> framebuffer(imageWidth * imageHeight);

    // Setup shader callback
    const auto shader = [&instances, &framebuffer, &cube, imageWidth, imageHeight](tracey::UVec2 pixelCoord, const tracey::Tlas &tlas)
    {
        // Simple pinhole camera ray generation
        float fov = 45.0f;
        float aspectRatio = static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
        float px = (2.0f * ((pixelCoord.x + 0.5f) / imageWidth) - 1.0f) * tan(glm::radians(fov) / 2.0f) * aspectRatio;
        float py = (1.0f - 2.0f * ((pixelCoord.y + 0.5f) / imageHeight)) * tan(glm::radians(fov) / 2.0f);
        tracey::Vec3 rayDir = glm::normalize(tracey::Vec3(px, py, 1.0f));
        tracey::Ray ray;
        ray.origin = tracey::Vec3(0, 0, 0);
        ray.direction = rayDir;
        ray.invDirection = tracey::Vec3(1.0f) / ray.direction;
        const auto tMin = 0.0f;
        const auto tMax = 100.0f;
        tracey::RayFlags flags = tracey::RAY_FLAG_OPAQUE;
        if (const auto intersection = tlas.intersect(ray, tMin, tMax, flags); intersection)
        {
            // Transform triangle vertices to world space
            const auto objectToWorldMatrix = instances[intersection->instanceId].transform;
            const auto v0 = tracey::transformPoint(objectToWorldMatrix, cube[intersection->primitiveId * 3 + 0]);
            const auto v1 = tracey::transformPoint(objectToWorldMatrix, cube[intersection->primitiveId * 3 + 1]);
            const auto v2 = tracey::transformPoint(objectToWorldMatrix, cube[intersection->primitiveId * 3 + 2]);
            const auto edge1 = tracey::Vec3(v1 - v0);
            const auto edge2 = tracey::Vec3(v2 - v0);
            auto normal = tracey::normalize(tracey::cross(edge1, edge2));

            // A fake directional light coming from (0,1,-1)
            const auto I = glm::dot(glm::normalize(tracey::Vec3(0, 1, -1)), normal);
            framebuffer[pixelCoord.y * imageWidth + pixelCoord.x] = tracey::Vec3(std::max(I * 0.5f, 0.0f)); // Map to [0,1]
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
    std::ofstream ofs("ray_tracer.ppm", std::ios::out | std::ios::binary);
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