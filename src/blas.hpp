#pragma once
#include <span>
#include <optional>
#include "types.hpp"
#include "ray.hpp"
#include "hit.hpp"
#include "bvh_node.hpp"
namespace tracey
{
    class Blas
    {
    public:
        Blas(std::span<const Vec3> positions);
        Blas(std::span<const Vec3> positions, std::span<const uint32_t> indices);

        std::optional<Hit> intersect(const Ray &ray, float tMin, float tMax, RayFlags flags) const;
        std::tuple<Vec3, Vec3> getBounds() const;

    private:
        struct PrimitiveRef
        {
            uint32_t index;
            Vec3 bMin;
            Vec3 bMax;
        };

        uint32_t buildRecursive(std::span<PrimitiveRef> primRefs, uint32_t nodeIndex, uint32_t start, uint32_t end, int depth);

    private:
        std::vector<BVHNode> m_nodes;
        std::vector<uint32_t> m_primIndices;
        std::span<const Vec3> m_positions;
        std::span<const uint32_t> m_indices;
    };
}