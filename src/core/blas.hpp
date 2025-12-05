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
        Blas(std::span<const float> data, std::uint32_t stride, std::optional<std::span<const uint32_t>> indices = std::nullopt);
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

        struct TriangleData
        {
            Vec3 v0;
            Vec3 edge1;
            Vec3 edge2;
            Vec3 normal;
        };

        uint32_t buildRecursive(std::span<PrimitiveRef> primRefs, uint32_t nodeIndex, uint32_t start, uint32_t end, int depth);
        Vec3 fetchVertex(uint32_t primitiveId, uint32_t element) const
        {
            const auto i0 = m_vertexStride * primitiveId * 3;
            const auto elementOffset = element * m_vertexStride;
            return Vec3{
                m_vertexBuffer[i0 + elementOffset + 0],
                m_vertexBuffer[i0 + elementOffset + 1],
                m_vertexBuffer[i0 + elementOffset + 2]};
        }

        Vec3 fetchVertexWithIndices(uint32_t primitiveId, uint32_t element) const
        {
            const auto index = m_vertexIndices[primitiveId * 3 + element];
            const auto i0 = m_vertexStride * index;
            return Vec3{
                m_vertexBuffer[i0 + 0],
                m_vertexBuffer[i0 + 1],
                m_vertexBuffer[i0 + 2]};
        }

        using FetchFunction = Vec3 (Blas::*)(uint32_t, uint32_t) const;

    private:
        std::vector<BVHNode> m_nodes;
        std::vector<uint32_t> m_primIndices;
        std::vector<TriangleData> m_triangleData;
        std::span<const float> m_vertexBuffer;
        std::span<const uint32_t> m_vertexIndices;
        const uint32_t m_vertexStride; // x, y, z
        const FetchFunction fetchVertexFunc;
    };
}