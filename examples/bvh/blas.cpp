#include "../../src/blas.hpp"
#include <vector>
#include <iostream>

int main()
{
    // 1) Define a single triangle
    std::array<tracey::Vec3, 3> triangle = {tracey::Vec3{-1, 0, 3}, tracey::Vec3{1, 0, 3}, tracey::Vec3{0, 1, 3}};

    // 2) Build Blas
    tracey::Blas blas(triangle);

    // 3) Shoot a ray from the origin in the +Z direction
    tracey::Ray ray;
    ray.origin = tracey::Vec3(0, 0, 0);
    ray.direction = glm::normalize(tracey::Vec3(0, 0, 1));

    const auto tMin = 0.0f;
    const auto tMax = 100.0f;
    tracey::RayFlags flags = tracey::RAY_FLAG_TERMINATE_ON_FIRST_HIT;
    if (const auto intersection = blas.intersect(ray, tMin, tMax, flags); intersection)
    {
        std::cout << "Hit primitive " << intersection->primitiveId
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