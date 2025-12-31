#include "tlas.hpp"
#include "blas.hpp"
#include "intersect.hpp"
namespace tracey
{

    Tlas::Tlas(std::span<const Blas *> blases, std::span<const Instance> instances) : blases(blases),
                                                                                      instances(instances)
    {
        for (const auto &instance : instances)
        {
            // Precompute inverse transforms for each instance
            Transforms transforms;
            std::memcpy(transforms.toWorld, instance.transform, sizeof(instance.transform));
            invert3x4(instance.transform, transforms.toObject);
            instanceTransforms.push_back(transforms);
        }
    }
    std::optional<Hit> Tlas::intersect(const Ray &ray, float tMin, float tMax, RayFlags flags) const
    {
        std::optional<Hit> closestHit = std::nullopt;
        float closestT = tMax;

        for (size_t instanceIndex = 0; instanceIndex < instances.size(); ++instanceIndex)
        {
            const auto &instance = instances[instanceIndex];
            const uint32_t blasIndex = static_cast<uint32_t>(instance.blasAddress);
            const Blas &blas = *blases[blasIndex];
            const auto [min, max] = blas.getBounds();

            const auto [worldMin, worldMax] = transformAABB(instanceTransforms[instanceIndex].toWorld, min, max);
            float tEnter, tExit;
            if (!intersectAABB(ray, worldMin, worldMax, tMin, closestT, tEnter, tExit))
                continue; // Ray misses the instance's bounds

            // Transform the ray into the instance's local space (use precomputed inverse)
            const auto &xf = instanceTransforms[instanceIndex];
            const float (*toObject)[4] = xf.toObject;
            const float (*toWorld)[4] = xf.toWorld;

            Vec3 localRayDirection = transformVector(toObject, ray.direction);
            Vec3 localRayInvDirection = 1.0f / localRayDirection;
            Vec3 localRayOrigin = transformPoint(toObject, ray.origin);

            Ray localRay{localRayOrigin, localRayDirection, localRayInvDirection};

            // Intersect in local space with generous bounds; we'll convert to world-space t for comparison.
            if (const auto hitOpt = blas.intersect(localRay, 0.0f, 1e30f, flags); hitOpt)
            {
                // Compute hit position in local space and transform back to world space.
                const Vec3 localHitPos = localRay.origin + localRay.direction * hitOpt->t;
                const Vec3 worldHitPos = transformPoint(toWorld, localHitPos);

                // Convert to world-space t (ray.direction is expected to be normalized in the renderer).
                const float tWorld = glm::dot(worldHitPos - ray.origin, ray.direction);

                // Apply world-space tMin/tMax and closest-hit test.
                if (tWorld >= tMin && tWorld < closestT)
                {
                    closestHit = hitOpt;
                    closestT = tWorld;

                    closestHit->instanceId = static_cast<uint32_t>(instanceIndex);
                    closestHit->position = worldHitPos;
                    closestHit->t = tWorld;

                    if (flags & RAY_FLAG_TERMINATE_ON_FIRST_HIT)
                        break;
                }
            }
        }

        return closestHit;
    }
}
