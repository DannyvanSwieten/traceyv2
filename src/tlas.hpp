#pragma once
#include <span>
#include "hit.hpp"
#include "bvh_node.hpp"
#include "ray.hpp"
namespace tracey
{
    class Blas;
    class Tlas
    {
    public:
        struct Instance
        {
            uint32_t instanceId : 24;
            uint8_t type : 8 = 0; // reserved for future use
            uint32_t blasIndex;
            Mat4 transform;
        };

        Tlas(std::span<const Blas> blases, std::span<const Instance> instances);

        std::optional<Hit> intersect(const Ray &ray, float tMin, float tMax, RayFlags flags) const;

    private:
        std::span<const Blas> blases;
        std::span<const Instance> instances;
        std::vector<BVHNode> m_nodes;
    };
}