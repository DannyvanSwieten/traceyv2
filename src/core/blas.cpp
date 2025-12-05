#include "blas.hpp"
#include "intersect.hpp"
#include <cassert>
namespace tracey
{
    Blas::Blas(std::span<const Vec3> positions) : Blas(std::span<const float>(reinterpret_cast<const float *>(positions.data()), positions.size() * 3), 3)
    {
    }

    Blas::Blas(std::span<const float> data, std::uint32_t stride, std::optional<std::span<const uint32_t>> indices)
        : m_vertexBuffer(data),
          m_vertexStride(stride),
          m_vertexIndices(indices ? *indices : std::span<const uint32_t>{}),
          fetchVertexFunc(indices.has_value() ? &Blas::fetchVertexWithIndices : &Blas::fetchVertex)
    {
        const auto primCount = (indices.has_value() ? indices->size() / 3 : (data.size() / stride) / 3);
        std::vector<PrimitiveRef> primRefs(primCount);
        for (size_t i = 0; i < primCount; ++i)
        {
            primRefs[i].index = static_cast<uint32_t>(i);
            const auto v0 = (this->*fetchVertexFunc)(i, 0);
            const auto v1 = (this->*fetchVertexFunc)(i, 1);
            const auto v2 = (this->*fetchVertexFunc)(i, 2);
            primRefs[i]
                .bMin = glm::min(glm::min(v0, v1), v2);
            primRefs[i].bMax = glm::max(glm::max(v0, v1), v2);
            // Store triangle data for intersection
            TriangleData triData;
            triData.v0 = v0;
            triData.edge1 = v1 - v0;
            triData.edge2 = v2 - v0;
            triData.normal = glm::normalize(glm::cross(triData.edge1, triData.edge2));
            m_triangleData.emplace_back(triData);
        }

        m_nodes.reserve(primCount * 2); // Rough estimate
        m_nodes.emplace_back();         // root
        if (primCount == 1)
        {
            // Special case: single triangle
            BVHNode &node = m_nodes[0];
            node.boundsMin = primRefs[0].bMin;
            node.boundsMax = primRefs[0].bMax;
            node.firstChildOrPrim = 0;
            node.primCountAndType = 1; // one triangle
            m_primIndices.push_back(primRefs[0].index);
            return;
        }
        buildRecursive(primRefs, 0, 0, static_cast<uint32_t>(primCount), 0);
    }

    Blas::Blas(std::span<const Vec3> positions, std::span<const uint32_t> indices) : Blas(std::span<const float>(reinterpret_cast<const float *>(positions.data()), positions.size() * 3), 3, indices)
    {
    }

    // Blas::Blas(std::span<const Vec3> positions, std::span<const uint32_t> indices) : m_vertexBuffer(std::span<const float>(reinterpret_cast<const float *>(positions.data()), positions.size() * 3)), m_vertexIndices(indices)
    // {
    //     std::vector<PrimitiveRef> primRefs(indices.size() / 3);
    //     const auto primCount = primRefs.size();
    //     for (size_t i = 0; i < primCount; ++i)
    //     {
    //         const auto index = i * 3;
    //         const auto i0 = indices[index];
    //         const auto i1 = indices[index + 1];
    //         const auto i2 = indices[index + 2];
    //         primRefs[i].index = static_cast<uint32_t>(i);
    //         primRefs[i].bMin = glm::min(glm::min(positions[i0], positions[i1]), positions[i2]);
    //         primRefs[i].bMax = glm::max(glm::max(positions[i0], positions[i1]), positions[i2]);
    //     }
    //     m_nodes.reserve(primCount * 2); // Rough estimate
    //     m_nodes.emplace_back();         // root
    //     if (primCount == 1)
    //     {
    //         // Special case: single triangle
    //         BVHNode &node = m_nodes[0];
    //         node.boundsMin = primRefs[0].bMin;
    //         node.boundsMax = primRefs[0].bMax;
    //         node.firstChildOrPrim = 0;
    //         node.primCountAndType = 1; // one triangle
    //         m_primIndices.push_back(primRefs[0].index);
    //         return;
    //     }
    //     buildRecursive(primRefs, 0, 0, static_cast<uint32_t>(primCount), 0);
    // }

    std::optional<Hit> Blas::intersect(const Ray &ray, float tMin, float tMax, RayFlags flags) const
    {
        if (m_nodes.empty())
            return std::nullopt;

        float closestT = tMax;
        std::optional<Hit> hit = std::nullopt;

        struct StackEntry
        {
            int nodeIndex;
            float tNear;
        };
        StackEntry stack[64];
        int stackTop = 0;
        stack[stackTop++] = {0, 0.0f};

        while (stackTop > 0)
        {
            StackEntry entry = stack[--stackTop];
            const BVHNode &node = m_nodes[entry.nodeIndex];

            float nodeEnter, nodeExit;
            if (!intersectAABB(ray, node.boundsMin, node.boundsMax,
                               tMin, closestT, nodeEnter, nodeExit) ||
                nodeEnter > closestT)
            {
                continue;
            }

            const auto primCount = node.primCountAndType & 0xFFFFFF;
            if (primCount > 0)
            {
                const auto primType = (node.primCountAndType >> 24) & 0xFF;
                switch (primType)
                {
                case BVH_LEAF_TYPE_TRIANGLES:
                    for (size_t i = node.firstChildOrPrim; i < node.firstChildOrPrim + primCount; ++i)
                    {
                        const uint32_t primId = m_primIndices[i];
                        const auto &triData = m_triangleData[primId];
                        Hit localHit;
                        if (intersectTriangle(ray,
                                              triData.v0,
                                              triData.edge1,
                                              triData.edge2,
                                              localHit.t,
                                              localHit.u,
                                              localHit.v))
                        {
                            localHit.primitiveId = primId;
                            if (localHit.t < closestT)
                            {
                                closestT = localHit.t;
                                hit = localHit;
                                hit->normal = triData.normal;
                                if (flags & RAY_FLAG_TERMINATE_ON_FIRST_HIT)
                                    return hit;
                            }
                        }
                    }
                    break;
                default:
                    // Unsupported leaf type
                    assert(false);
                    continue;
                }
            }
            else
            {
                // Interior: visit children. Push the farther child first so the nearer one is popped and processed first.
                int left  = static_cast<int>(node.firstChildOrPrim);
                int right = left + 1;

                float tEnterL, tExitL, tEnterR, tExitR;
                bool hitL = intersectAABB(ray, m_nodes[left].boundsMin,  m_nodes[left].boundsMax,
                                          tMin, closestT, tEnterL, tExitL);
                bool hitR = intersectAABB(ray, m_nodes[right].boundsMin, m_nodes[right].boundsMax,
                                          tMin, closestT, tEnterR, tExitR);

                if (hitL && hitR)
                {
                    int   firstChild  = left;
                    int   secondChild = right;
                    float tFirst      = tEnterL;
                    float tSecond     = tEnterR;

                    // Ensure firstChild is the nearer one
                    if (tSecond < tFirst)
                    {
                        std::swap(firstChild, secondChild);
                        std::swap(tFirst, tSecond);
                    }

                    // Push farther child first, then nearer child
                    if (tSecond < closestT)
                        stack[stackTop++] = {secondChild, tSecond};
                    if (tFirst < closestT)
                        stack[stackTop++] = {firstChild, tFirst};
                }
                else
                {
                    if (hitL && tEnterL < closestT)
                        stack[stackTop++] = {left, tEnterL};
                    if (hitR && tEnterR < closestT)
                        stack[stackTop++] = {right, tEnterR};
                }
            }
        }

        return hit;
    }

    std::tuple<Vec3, Vec3> Blas::getBounds() const
    {
        return {m_nodes[0].boundsMin, m_nodes[0].boundsMax};
    }

    uint32_t Blas::buildRecursive(std::span<PrimitiveRef> prims, uint32_t nodeIndex, uint32_t start, uint32_t end, int depth)
    {
        BVHNode &node = m_nodes[nodeIndex];
        Vec3 bMin(std::numeric_limits<float>::max());
        Vec3 bMax(std::numeric_limits<float>::lowest());
        for (uint32_t i = start; i < end; ++i)
        {
            bMin = glm::min(bMin, prims[i].bMin);
            bMax = glm::max(bMax, prims[i].bMax);
        }
        node.boundsMin = bMin;
        node.boundsMax = bMax;

        int count = static_cast<int>(end - start);
        const int leafThreshold = 4;
        if (count <= leafThreshold)
        { // create leaf
            node.primCountAndType = count;
            node.firstChildOrPrim = static_cast<uint32_t>(m_primIndices.size());

            for (int i = start; i < end; ++i)
                m_primIndices.emplace_back(prims[i].index);

            return nodeIndex;
        }

        auto surfaceArea = [](const Vec3 &mn, const Vec3 &mx) -> float {
            Vec3 e = mx - mn;
            return 2.0f * (e.x * e.y + e.x * e.z + e.y * e.z);
        };

        const float parentArea = surfaceArea(bMin, bMax);
        const float Ci = 1.0f; // approximate triangle test cost
        const float Ct = 1.0f; // approximate traversal cost

        constexpr int BIN_COUNT = 16;

        struct Bin
        {
            int  count = 0;
            Vec3 bMin  = Vec3(std::numeric_limits<float>::max());
            Vec3 bMax  = Vec3(std::numeric_limits<float>::lowest());
        };

        float bestCost      = std::numeric_limits<float>::infinity();
        int   bestAxis      = -1;
        int   bestSplitBin  = -1;
        float bestCMin      = 0.0f;
        float bestCMax      = 0.0f;

        const int n = count;

        // Binned SAH along each axis
        for (int axis = 0; axis < 3; ++axis)
        {
            // Compute centroid range along this axis
            float cMin = std::numeric_limits<float>::max();
            float cMax = std::numeric_limits<float>::lowest();
            for (int i = 0; i < n; ++i)
            {
                const PrimitiveRef &p = prims[start + i];
                float c = 0.5f * (p.bMin[axis] + p.bMax[axis]);
                cMin = std::min(cMin, c);
                cMax = std::max(cMax, c);
            }

            // Degenerate range: cannot split along this axis
            if (!(cMax > cMin))
                continue;

            Bin bins[BIN_COUNT];

            const float invBinSize = (float)BIN_COUNT / (cMax - cMin);

            // Fill bins
            for (int i = 0; i < n; ++i)
            {
                const PrimitiveRef &p = prims[start + i];
                float c = 0.5f * (p.bMin[axis] + p.bMax[axis]);
                int binIdx = (int)((c - cMin) * invBinSize);
                if (binIdx < 0) binIdx = 0;
                if (binIdx >= BIN_COUNT) binIdx = BIN_COUNT - 1;

                Bin &b = bins[binIdx];
                b.count++;
                b.bMin = glm::min(b.bMin, p.bMin);
                b.bMax = glm::max(b.bMax, p.bMax);
            }

            // Prefix (left) and suffix (right) aggregates over bins
            int  leftCount[BIN_COUNT];
            Vec3 leftMin[BIN_COUNT];
            Vec3 leftMax[BIN_COUNT];

            int  rightCount[BIN_COUNT];
            Vec3 rightMin[BIN_COUNT];
            Vec3 rightMax[BIN_COUNT];

            // Build left side prefix
            int  runningCount = 0;
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
                leftMin[i]   = runningMin;
                leftMax[i]   = runningMax;
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
                rightMin[i]   = runningMin;
                rightMax[i]   = runningMax;
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
                    bestCost     = cost;
                    bestAxis     = axis;
                    bestSplitBin = i;
                    bestCMin     = cMin;
                    bestCMax     = cMax;
                }
            }
        }

        // If SAH did not find a useful split, make a leaf
        if (bestAxis == -1 || bestSplitBin < 0 || !std::isfinite(bestCost))
        {
            node.primCountAndType = count;
            node.firstChildOrPrim = static_cast<uint32_t>(m_primIndices.size());
            for (int i = start; i < end; ++i)
                m_primIndices.emplace_back(prims[i].index);
            return nodeIndex;
        }

        // Partition primitives in-place based on best axis and bin split
        const float cMinBest     = bestCMin;
        const float cMaxBest     = bestCMax;
        const float invBinSizeBest = (float)BIN_COUNT / (cMaxBest - cMinBest);

        auto midIter = std::partition(prims.begin() + start, prims.begin() + end,
                                      [bestAxis, bestSplitBin, cMinBest, invBinSizeBest](const PrimitiveRef &p) {
                                          float c = 0.5f * (p.bMin[bestAxis] + p.bMax[bestAxis]);
                                          int binIdx = (int)((c - cMinBest) * invBinSizeBest);
                                          if (binIdx < 0) binIdx = 0;
                                          if (binIdx > bestSplitBin) return false;
                                          return true;
                                      });
        uint32_t mid = static_cast<uint32_t>(midIter - prims.begin());

        // Fallback: if partition failed to split, just split in the middle
        if (mid <= start || mid >= end)
        {
            mid = (start + end) / 2;
        }

        node.primCountAndType = 0;                        // interior
        node.firstChildOrPrim = static_cast<uint32_t>(m_nodes.size()); // left child index

        int leftIndex = static_cast<int>(m_nodes.size());
        m_nodes.emplace_back();
        int rightIndex = static_cast<int>(m_nodes.size());
        m_nodes.emplace_back();

        buildRecursive(prims, leftIndex, start, mid, depth + 1);
        buildRecursive(prims, rightIndex, mid, end, depth + 1);

        return nodeIndex;
    }
}
