#include "tlas.hpp"
#include "blas.hpp"
#include "intersect.hpp"
#include <algorithm>
namespace tracey
{
    // See blas.cpp: cap tree depth so the fixed traversal stack can't overflow.
    static constexpr int kTraversalStackSize = 64;
    static constexpr int kMaxBvhDepth = 60; // < kTraversalStackSize, with headroom


    Tlas::Tlas(std::span<const Blas *> blases, std::span<const Instance> instances) : Tlas(blases, instances, Config{})
    {
    }

    Tlas::Tlas(std::span<const Blas *> blases, std::span<const Instance> instances, const Config &config)
        : Tlas(blases, instances, instances, false, config)
    {
    }

    namespace
    {
        // Row-major 3×4 Instance transform → column-major 4×4 object→world.
        Mat4 instanceToWorld(const Tlas::Instance &instance)
        {
            return Mat4(
                instance.transform[0][0], instance.transform[1][0], instance.transform[2][0], 0.0f,
                instance.transform[0][1], instance.transform[1][1], instance.transform[2][1], 0.0f,
                instance.transform[0][2], instance.transform[1][2], instance.transform[2][2], 0.0f,
                instance.transform[0][3], instance.transform[1][3], instance.transform[2][3], 1.0f);
        }
    }

    Tlas::Tlas(std::span<const Blas *> blases, std::span<const Instance> instances,
               std::span<const Instance> instancesEnd, bool hasMotion, const Config &config)
        : blases(blases), instances(instances), m_hasMotion(hasMotion), m_config(config)
    {
        // hasMotion requires a parallel end-pose array; fall back to static if
        // the caller didn't supply matching counts.
        if (m_hasMotion && instancesEnd.size() != instances.size())
            m_hasMotion = false;

        // Prepare instance references with world-space AABBs
        std::vector<InstanceRef> instanceRefs(instances.size());

        for (size_t i = 0; i < instances.size(); ++i)
        {
            const auto &instance = instances[i];
            // Precompute inverse transforms for each instance (shutter-open).
            const Mat4 toWorldMat = instanceToWorld(instance);
            Transforms transforms;
            transforms.toWorld = toWorldMat;
            transforms.toObject = glm::inverse(toWorldMat);
            instanceTransforms.push_back(transforms);

            // Compute world-space AABB for this instance
            const uint32_t blasIndex = static_cast<uint32_t>(instance.blasAddress);
            const Blas &blas = *blases[blasIndex];
            const auto [localMin, localMax] = blas.getBounds();

            // Transform BLAS bounds to world space (shutter-open pose).
            auto [worldMin, worldMax] = transformAABB(toWorldMat, localMin, localMax);

            // Motion: cache the shutter-close transform and grow the instance
            // AABB to the swept union so traversal never misses it mid-shutter.
            if (m_hasMotion)
            {
                const Mat4 endToWorld = instanceToWorld(instancesEnd[i]);
                Transforms endXf;
                endXf.toWorld = endToWorld;
                endXf.toObject = glm::inverse(endToWorld);
                instanceTransformsEnd.push_back(endXf);

                const auto [endMin, endMax] = transformAABB(endToWorld, localMin, localMax);
                worldMin = glm::min(worldMin, endMin);
                worldMax = glm::max(worldMax, endMax);
            }

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
        if (count <= m_config.leafThreshold || depth >= kMaxBvhDepth)
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

        if (m_nodes.empty())
            return closestHit;

        // Per-instance Blas test, factored out of the BVH walk. Transforms the
        // ray into the instance's local space (precomputed inverse), intersects
        // the Blas, and folds a closer world-space hit into closestHit.
        const auto testInstance = [&](uint32_t instanceIndex) {
            const Blas &blas = *blases[static_cast<uint32_t>(instances[instanceIndex].blasAddress)];
            const auto &xf = instanceTransforms[instanceIndex];

            // Motion blur: linearly interpolate the object→world matrix between
            // the shutter-open and -close poses at the ray's shutter time, then
            // invert per-ray. Element-wise matrix lerp matches Metal's matrix
            // motion-keyframe interpolation so the two backends stay in lockstep.
            Mat4 toObjectM = xf.toObject;
            Mat4 toWorldM = xf.toWorld;
            if (m_hasMotion)
            {
                const float t = ray.time;
                toWorldM = xf.toWorld * (1.0f - t) + instanceTransformsEnd[instanceIndex].toWorld * t;
                toObjectM = glm::inverse(toWorldM);
            }

            const Vec3 localRayDirection = transformVector(toObjectM, ray.direction);
            const Vec3 localRayInvDirection = 1.0f / localRayDirection;
            const Vec3 localRayOrigin = transformPoint(toObjectM, ray.origin);
            const Ray localRay{localRayOrigin, localRayDirection, localRayInvDirection};

            if (const auto hitOpt = blas.intersect(localRay, 0.0f, 1e30f, flags); hitOpt)
            {
                const Vec3 localHitPos = localRay.origin + localRay.direction * hitOpt->t;
                const Vec3 worldHitPos = transformPoint(toWorldM, localHitPos);
                // World-space t (ray.direction is normalized in the renderer).
                const float tWorld = glm::dot(worldHitPos - ray.origin, ray.direction);
                if (tWorld >= tMin && tWorld < closestT)
                {
                    closestHit = hitOpt;
                    closestT = tWorld;
                    closestHit->instanceId = instanceIndex;
                    closestHit->position = worldHitPos;
                    closestHit->t = tWorld;
                    return true;
                }
            }
            return false;
        };

        // Stack-based traversal of the instance BVH (m_nodes). Node bounds are
        // world-space instance AABBs cached at build time, so there's no per-ray
        // AABB transform (the old code linearly scanned every instance and
        // recomputed one each ray — O(instances) per ray). Mirrors Blas::intersect.
        struct StackEntry { uint32_t nodeIndex; float tNear; };
        StackEntry stack[kTraversalStackSize];
        int stackTop = 0;

        // Test the root once; children are AABB-tested when pushed, so popped
        // nodes only do a cheap tNear cull (no second intersectAABB).
        {
            float rEnter, rExit;
            if (!intersectAABB(ray, m_nodes[0].boundsMin, m_nodes[0].boundsMax,
                               tMin, closestT, rEnter, rExit))
                return closestHit;
            stack[stackTop++] = {0u, rEnter};
        }

        while (stackTop > 0)
        {
            const StackEntry entry = stack[--stackTop];
            if (entry.tNear >= closestT)
                continue;
            const BVHNode &node = m_nodes[entry.nodeIndex];

            const uint32_t primCount = node.primCountAndType & 0xFFFFFFu;
            if (primCount > 0)
            {
                // Leaf: test each referenced instance.
                for (uint32_t k = 0; k < primCount; ++k)
                {
                    const uint32_t instanceIndex = m_instanceIndices[node.firstChildOrPrim + k];
                    if (testInstance(instanceIndex) && (flags & RAY_FLAG_TERMINATE_ON_FIRST_HIT))
                        return closestHit;
                }
            }
            else
            {
                // Interior: push the farther child first so the nearer one is
                // popped and processed first (front-to-back, tighter closestT).
                const uint32_t left = node.firstChildOrPrim;
                const uint32_t right = left + 1u;
                float tEnterL, tExitL, tEnterR, tExitR;
                const bool hitL = intersectAABB(ray, m_nodes[left].boundsMin, m_nodes[left].boundsMax,
                                                tMin, closestT, tEnterL, tExitL);
                const bool hitR = intersectAABB(ray, m_nodes[right].boundsMin, m_nodes[right].boundsMax,
                                                tMin, closestT, tEnterR, tExitR);
                if (hitL && hitR)
                {
                    uint32_t firstChild = left, secondChild = right;
                    float tFirst = tEnterL, tSecond = tEnterR;
                    if (tSecond < tFirst)
                    {
                        std::swap(firstChild, secondChild);
                        std::swap(tFirst, tSecond);
                    }
                    if (tSecond < closestT) stack[stackTop++] = {secondChild, tSecond};
                    if (tFirst < closestT) stack[stackTop++] = {firstChild, tFirst};
                }
                else if (hitL && tEnterL < closestT)
                {
                    stack[stackTop++] = {left, tEnterL};
                }
                else if (hitR && tEnterR < closestT)
                {
                    stack[stackTop++] = {right, tEnterR};
                }
            }
        }

        return closestHit;
    }
}
