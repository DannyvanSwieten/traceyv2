#include "../../src/core/blas.hpp"
#include "../../src/core/tlas.hpp"
#include <iostream>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>
int main()
{
    // 1) Define a single triangle
    std::array<tracey::Vec3, 3> triangle = {tracey::Vec3{-1, 0, 3}, tracey::Vec3{1, 0, 3}, tracey::Vec3{0, 1, 3}};

    // 2) Build Blas
    tracey::Blas blas(triangle);

    // 3) Create some instances. One on the left and one on the right.
    std::array<tracey::Tlas::Instance, 2> instances;
    instances[0].blasAddress = 0;
    instances[0].instanceCustomIndexAndMask = 0;
    instances[0].setTransform(glm::translate(tracey::Vec3(-2, 0, 0)));

    instances[1].blasAddress = 0;
    instances[1].instanceCustomIndexAndMask = 1;
    instances[1].setTransform(glm::translate(tracey::Vec3(2, 0, 0)));
    // 4) Build Tlas
    const tracey::Blas *blasPtr = &blas;
    tracey::Tlas tlas(std::span<const tracey::Blas *>(&blasPtr, 1), instances);

    // 5) Shoot a ray from the origin to -X direction, hitting the triangle on the left
    tracey::Ray ray;
    ray.origin = tracey::Vec3(0, 0, 0);
    ray.direction = glm::normalize(tracey::Vec3(-0.5, 0, 1));
    const auto tMin = 0.0f;
    const auto tMax = 100.0f;
    tracey::RayFlags flags = tracey::RAY_FLAG_TERMINATE_ON_FIRST_HIT;
    if (const auto intersection = tlas.intersect(ray, tMin, tMax, flags); intersection)
    {
        std::cout << "Hit instance " << intersection->instanceId
                  << " at t = " << intersection->t
                  << " pos = (" << intersection->position.x << ", "
                  << intersection->position.y << ", "
                  << intersection->position.z << ")\n";
    }
    else
    {
        std::cout << "No hit\n";
    }

    // 6) Shoot a ray from the origin to +X direction, hitting the triangle on the right
    ray.origin = tracey::Vec3(0, 0, 0);
    ray.direction = glm::normalize(tracey::Vec3(0.5, 0, 1));
    if (const auto intersection = tlas.intersect(ray, tMin, tMax, flags); intersection)
    {
        std::cout << "Hit instance " << intersection->instanceId
                  << " at t = " << intersection->t
                  << " pos = (" << intersection->position.x << ", "
                  << intersection->position.y << ", "
                  << intersection->position.z << ")\n";
    }
    else
    {
        std::cout << "No hit\n";
    }

    return 0;
}