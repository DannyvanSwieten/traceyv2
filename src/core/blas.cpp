#include "blas.hpp"
#include "intersect.hpp"
#include <cassert>
namespace tracey
{
    Blas::Blas(std::span<const Vec3> positions) : Blas(std::span<const float>(reinterpret_cast<const float *>(positions.data()), positions.size() * 3), 3)
    {
    }

    Blas::Blas(std::span<const float> data, std::uint32_t stride)
        : m_vertexBuffer(data),
          vertexStride(stride),
          fetchVertexFunc([this](uint32_t primIdx, uint32_t vertIdx)
                          { return this->fetchVertex(primIdx, vertIdx); })
    {
        std::vector<PrimitiveRef> primRefs((data.size() / stride) / 3);
        const auto primCount = primRefs.size();
        for (size_t i = 0; i < primCount; ++i)
        {
            primRefs[i].index = static_cast<uint32_t>(i);
            const auto v0 = fetchVertexFunc(i, 0);
            const auto v1 = fetchVertexFunc(i, 1);
            const auto v2 = fetchVertexFunc(i, 2);
            primRefs[i]
                .bMin = glm::min(glm::min(v0, v1), v2);
            primRefs[i].bMax = glm::max(glm::max(v0, v1), v2);
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

        bool hitSomething = false;
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
                        const auto v0 = fetchVertexFunc(i, 0);
                        const auto v1 = fetchVertexFunc(i, 1);
                        const auto v2 = fetchVertexFunc(i, 2);
                        Hit localHit;
                        if (intersectTriangle(ray,
                                              v0,
                                              v1,
                                              v2,
                                              localHit.t,
                                              localHit.u,
                                              localHit.v))
                        {
                            localHit.primitiveId = i;
                            if (localHit.t < closestT)
                            {
                                closestT = localHit.t;
                                hit = localHit;
                                hitSomething = true;
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
                // Interior: visit children. You can push closer child last so it’s popped first.
                int left = (int)node.firstChildOrPrim;
                int right = left + 1;

                float tMinR, tMaxR, tEnterL, tExitL, tEnterR, tExitR;
                bool hitL = intersectAABB(ray, m_nodes[left].boundsMin, m_nodes[left].boundsMax, tMin, closestT, tEnterL, tExitL);
                bool hitR = intersectAABB(ray, m_nodes[right].boundsMin, m_nodes[right].boundsMax, tMin, closestT, tEnterR, tExitR);

                if (hitL && tEnterL < closestT)
                    stack[stackTop++] = {left, tEnterL};
                if (hitR && tEnterR < closestT)
                    stack[stackTop++] = {right, tEnterR};
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

        int count = end - start;
        if (count <= 4)
        { // leaf threshold
            node.primCountAndType = count;
            node.firstChildOrPrim = (uint32_t)m_primIndices.size();

            for (int i = start; i < end; ++i)
                m_primIndices.emplace_back(prims[i].index);

            return nodeIndex;
        }

        // Choose split axis based on extent
        glm::vec3 extent = bMax - bMin;
        int axis = 0;
        if (extent.y > extent.x && extent.y > extent.z)
            axis = 1;
        else if (extent.z > extent.x)
            axis = 2;

        int mid = (start + end) / 2;
        std::nth_element(prims.begin() + start, prims.begin() + mid, prims.begin() + end,
                         [axis](const PrimitiveRef &a, const PrimitiveRef &b)
                         {
                             float ca = 0.5f * (a.bMin[axis] + a.bMax[axis]);
                             float cb = 0.5f * (b.bMin[axis] + b.bMax[axis]);
                             return ca < cb;
                         });

        node.primCountAndType = 0;                        // interior
        node.firstChildOrPrim = (uint32_t)m_nodes.size(); // left child index

        int leftIndex = (int)m_nodes.size();
        m_nodes.emplace_back();
        int rightIndex = (int)m_nodes.size();
        m_nodes.emplace_back();

        buildRecursive(prims, leftIndex, start, mid, depth + 1);
        buildRecursive(prims, rightIndex, mid, end, depth + 1);

        return nodeIndex;
    }
}
