#include "tlas.hpp"
#include "blas.hpp"
#include "intersect.hpp"
namespace tracey
{

    Tlas::Tlas(std::span<const Blas> blases, std::span<const Instance> instances) : blases(blases),
                                                                                    instances(instances)
    {
    }
    std::optional<Hit> Tlas::intersect(const Ray &ray, float tMin, float tMax, RayFlags flags) const
    {
        std::optional<Hit> closestHit = std::nullopt;
        float closestT = tMax;

        for (const auto &instance : instances)
        {
            const uint32_t blasIndex = instance.blasIndex;
            const Blas &blas = blases[blasIndex];
            const auto [min, max] = blas.getBounds();
            // Transform the AABB into world space
            const auto [worldMin, worldMax] = transformAABB(instance.transform, min, max);
            float tEnter, tExit;
            if (!intersectAABB(ray, worldMin, worldMax, tMin, tMax, tEnter, tExit))
                continue; // Ray misses the instance's bounds

            // Transform the ray into the instance's local space
            const auto inverseTransform = glm::inverse(instance.transform);
            Vec3 localRayDirection = Vec3(inverseTransform * Vec4(ray.direction, 0.0f));
            Vec3 localRayOrigin = Vec3(inverseTransform * Vec4(ray.origin, 1.0f));

            Ray localRay{localRayOrigin, localRayDirection};

            if (const auto hitOpt = blas.intersect(localRay, tMin, closestT, flags); hitOpt)
            {
                if (hitOpt->t < closestT)
                {
                    closestHit = hitOpt;
                    closestT = hitOpt->t;
                    closestHit->instanceId = instance.instanceId;
                    // primitiveId remains as in the BLAS hit
                    // Transform the hit position back to world space
                    closestHit->position = ray.origin + ray.direction * closestHit->t;
                    if (flags & RAY_FLAG_TERMINATE_ON_FIRST_HIT)
                        break;
                }
            }
        }

        return closestHit;
    }
}
