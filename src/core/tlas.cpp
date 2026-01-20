#include "tlas.hpp"
#include "blas.hpp"
#include "intersect.hpp"
#include <algorithm>
namespace tracey
{

    Tlas::Tlas(std::span<const Blas *> blases, std::span<const Instance> instances) : Tlas(blases, instances, Config{})
    {
    }

    Tlas::Tlas(std::span<const Blas *> blases, std::span<const Instance> instances, const Config &config) : blases(blases),
                                                                                                            instances(instances),
                                                                                                            m_config(config)
    {
        // Prepare instance references with world-space AABBs
        std::vector<InstanceRef> instanceRefs(instances.size());

        for (size_t i = 0; i < instances.size(); ++i)
        {
            const auto &instance = instances[i];
            // Precompute inverse transforms for each instance
            Transforms transforms;
            // first turn the 3x4 row-major into a 4x4 matrix
            Mat4 toWorldMat(
                instance.transform[0][0], instance.transform[1][0], instance.transform[2][0], 0.0f,
                instance.transform[0][1], instance.transform[1][1], instance.transform[2][1], 0.0f,
                instance.transform[0][2], instance.transform[1][2], instance.transform[2][2], 0.0f,
                instance.transform[0][3], instance.transform[1][3], instance.transform[2][3], 1.0f);
            transforms.toWorld = toWorldMat;
            // Invert the toWorldMat to get toObject
            transforms.toObject = glm::inverse(toWorldMat);
            instanceTransforms.push_back(transforms);

            // Compute world-space AABB for this instance
            const uint32_t blasIndex = static_cast<uint32_t>(instance.blasAddress);
            const Blas &blas = *blases[blasIndex];
            const auto [localMin, localMax] = blas.getBounds();

            // Transform BLAS bounds to world space
            const auto [worldMin, worldMax] = transformAABB(toWorldMat, localMin, localMax);

            instanceRefs[i].index = static_cast<uint32_t>(i);
            instanceRefs[i].bMin = worldMin;
            instanceRefs[i].bMax = worldMax;
        }

        // Build BVH over instances
        if (instances.empty())
            return;

        m_nodes.reserve(instances.size() * 2);
        m_nodes.emplace_back(); // root

        if (instances.size() == 1)
        {
            // Special case: single instance
            BVHNode &node = m_nodes[0];
            node.boundsMin = instanceRefs[0].bMin;
            node.boundsMax = instanceRefs[0].bMax;
            node.firstChildOrPrim = 0;
            node.primCountAndType = 1; // one instance
            m_instanceIndices.push_back(instanceRefs[0].index);
            return;
        }

        buildRecursive(instanceRefs, 0, 0, static_cast<uint32_t>(instances.size()), 0);
    }

    uint32_t Tlas::buildRecursive(std::span<InstanceRef> refs, uint32_t nodeIndex, uint32_t start, uint32_t end, int depth)
    {
        BVHNode &node = m_nodes[nodeIndex];
        Vec3 bMin(std::numeric_limits<float>::max());
        Vec3 bMax(std::numeric_limits<float>::lowest());
        for (uint32_t i = start; i < end; ++i)
        {
            bMin = glm::min(bMin, refs[i].bMin);
            bMax = glm::max(bMax, refs[i].bMax);
        }
        node.boundsMin = bMin;
        node.boundsMax = bMax;

        int count = static_cast<int>(end - start);
        if (count <= m_config.leafThreshold)
        { // create leaf
            node.primCountAndType = count;
            node.firstChildOrPrim = static_cast<uint32_t>(m_instanceIndices.size());

            for (uint32_t i = start; i < end; ++i)
                m_instanceIndices.emplace_back(refs[i].index);

            return nodeIndex;
        }

        auto surfaceArea = [](const Vec3 &mn, const Vec3 &mx) -> float
        {
            Vec3 e = mx - mn;
            return 2.0f * (e.x * e.y + e.x * e.z + e.y * e.z);
        };

        const float parentArea = surfaceArea(bMin, bMax);
        const float Ci = m_config.intersectionCost;
        const float Ct = m_config.traversalCost;

        const int BIN_COUNT = m_config.binCount;

        struct Bin
        {
            int count = 0;
            Vec3 bMin = Vec3(std::numeric_limits<float>::max());
            Vec3 bMax = Vec3(std::numeric_limits<float>::lowest());
        };

        float bestCost = std::numeric_limits<float>::infinity();
        int bestAxis = -1;
        int bestSplitBin = -1;
        float bestCMin = 0.0f;
        float bestCMax = 0.0f;

        const int n = count;

        // Binned SAH along each axis
        for (int axis = 0; axis < 3; ++axis)
        {
            // Compute centroid range along this axis
            float cMin = std::numeric_limits<float>::max();
            float cMax = std::numeric_limits<float>::lowest();
            for (int i = 0; i < n; ++i)
            {
                const InstanceRef &p = refs[start + i];
                float c = 0.5f * (p.bMin[axis] + p.bMax[axis]);
                cMin = std::min(cMin, c);
                cMax = std::max(cMax, c);
            }

            // Degenerate range: cannot split along this axis
            if (!(cMax > cMin))
                continue;

            std::vector<Bin> bins(BIN_COUNT);

            const float invBinSize = (float)BIN_COUNT / (cMax - cMin);

            // Fill bins
            for (int i = 0; i < n; ++i)
            {
                const InstanceRef &p = refs[start + i];
                float c = 0.5f * (p.bMin[axis] + p.bMax[axis]);
                int binIdx = (int)((c - cMin) * invBinSize);
                if (binIdx < 0)
                    binIdx = 0;
                if (binIdx >= BIN_COUNT)
                    binIdx = BIN_COUNT - 1;

                Bin &b = bins[binIdx];
                b.count++;
                b.bMin = glm::min(b.bMin, p.bMin);
                b.bMax = glm::max(b.bMax, p.bMax);
            }

            // Prefix (left) and suffix (right) aggregates over bins
            std::vector<int> leftCount(BIN_COUNT);
            std::vector<Vec3> leftMin(BIN_COUNT);
            std::vector<Vec3> leftMax(BIN_COUNT);

            std::vector<int> rightCount(BIN_COUNT);
            std::vector<Vec3> rightMin(BIN_COUNT);
            std::vector<Vec3> rightMax(BIN_COUNT);

            // Build left side prefix
            int runningCount = 0;
            Vec3 runningMin(std::numeric_limits<float>::max());
            Vec3 runningMax(std::numeric_limits<float>::lowest());
            for (int i = 0; i < BIN_COUNT; ++i)
            {
                if (bins[i].count > 0)
                {
                    runningCount += bins[i].count;
                    runningMin = glm::min(runningMin, bins[i].bMin);
                    runningMax = glm::max(runningMax, bins[i].bMax);
                }
                leftCount[i] = runningCount;
                leftMin[i] = runningMin;
                leftMax[i] = runningMax;
            }

            // Build right side suffix
            runningCount = 0;
            runningMin = Vec3(std::numeric_limits<float>::max());
            runningMax = Vec3(std::numeric_limits<float>::lowest());
            for (int i = BIN_COUNT - 1; i >= 0; --i)
            {
                if (bins[i].count > 0)
                {
                    runningCount += bins[i].count;
                    runningMin = glm::min(runningMin, bins[i].bMin);
                    runningMax = glm::max(runningMax, bins[i].bMax);
                }
                rightCount[i] = runningCount;
                rightMin[i] = runningMin;
                rightMax[i] = runningMax;
            }

            // Evaluate SAH cost for splits between bins i and i+1
            for (int i = 0; i < BIN_COUNT - 1; ++i)
            {
                int countL = leftCount[i];
                int countR = rightCount[i + 1];

                if (countL == 0 || countR == 0)
                    continue;

                float areaL = surfaceArea(leftMin[i], leftMax[i]);
                float areaR = surfaceArea(rightMin[i + 1], rightMax[i + 1]);

                float cost = Ct +
                             (areaL / parentArea) * (countL * Ci) +
                             (areaR / parentArea) * (countR * Ci);

                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplitBin = i;
                    bestCMin = cMin;
                    bestCMax = cMax;
                }
            }
        }

        // If SAH did not find a useful split, make a leaf
        if (bestAxis == -1 || bestSplitBin < 0 || !std::isfinite(bestCost))
        {
            node.primCountAndType = count;
            node.firstChildOrPrim = static_cast<uint32_t>(m_instanceIndices.size());
            for (uint32_t i = start; i < end; ++i)
                m_instanceIndices.emplace_back(refs[i].index);
            return nodeIndex;
        }

        // Partition instances in-place based on best axis and bin split
        const float cMinBest = bestCMin;
        const float cMaxBest = bestCMax;
        const float invBinSizeBest = (float)BIN_COUNT / (cMaxBest - cMinBest);

        auto midIter = std::partition(refs.begin() + start, refs.begin() + end,
                                      [bestAxis, bestSplitBin, cMinBest, invBinSizeBest](const InstanceRef &p)
                                      {
                                          float c = 0.5f * (p.bMin[bestAxis] + p.bMax[bestAxis]);
                                          int binIdx = (int)((c - cMinBest) * invBinSizeBest);
                                          if (binIdx < 0)
                                              binIdx = 0;
                                          if (binIdx > bestSplitBin)
                                              return false;
                                          return true;
                                      });
        uint32_t mid = static_cast<uint32_t>(midIter - refs.begin());

        // Fallback: if partition failed to split, just split in the middle
        if (mid <= start || mid >= end)
        {
            mid = (start + end) / 2;
        }

        node.primCountAndType = 0;                                     // interior
        node.firstChildOrPrim = static_cast<uint32_t>(m_nodes.size()); // left child index

        int leftIndex = static_cast<int>(m_nodes.size());
        m_nodes.emplace_back();
        int rightIndex = static_cast<int>(m_nodes.size());
        m_nodes.emplace_back();

        buildRecursive(refs, leftIndex, start, mid, depth + 1);
        buildRecursive(refs, rightIndex, mid, end, depth + 1);

        return nodeIndex;
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
            // const float (*toObject)[4] = xf.toObject;
            // const float (*toWorld)[4] = xf.toWorld;

            Vec3 localRayDirection = transformVector(xf.toObject, ray.direction);
            Vec3 localRayInvDirection = 1.0f / localRayDirection;
            Vec3 localRayOrigin = transformPoint(xf.toObject, ray.origin);

            Ray localRay{localRayOrigin, localRayDirection, localRayInvDirection};

            // Intersect in local space with generous bounds; we'll convert to world-space t for comparison.
            if (const auto hitOpt = blas.intersect(localRay, 0.0f, 1e30f, flags); hitOpt)
            {
                // Compute hit position in local space and transform back to world space.
                const Vec3 localHitPos = localRay.origin + localRay.direction * hitOpt->t;
                const Vec3 worldHitPos = transformPoint(xf.toWorld, localHitPos);

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
