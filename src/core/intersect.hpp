#pragma once
#include <tuple>
#include "ray.hpp"
namespace tracey
{

    inline bool intersectAABB(const Ray &r, const tracey::Vec3 &bmin, const tracey::Vec3 &bmax, float minT, float maxT,
                       float &tEnter, float &tExit)
                       {
        glm::vec3 t0 = (bmin - r.origin) * r.invDirection;
        glm::vec3 t1 = (bmax - r.origin) * r.invDirection;

        glm::vec3 tMin = glm::min(t0, t1);
        glm::vec3 tMax = glm::max(t0, t1);

        tEnter = std::max({tMin.x, tMin.y, tMin.z, minT});
        tExit = std::min({tMax.x, tMax.y, tMax.z, maxT});

        return tExit >= tEnter;
    }
    bool intersectTriangle(const Ray &ray, const Vec3 &v0, const Vec3 &v1, const Vec3 &v2, float &tOut, float &uOut, float &vOut);

    std::tuple<tracey::Vec3, tracey::Vec3> transformAABB(const tracey::Mat4 &M,
                                                         const tracey::Vec3 &localMin,
                                                         const tracey::Vec3 &localMax);
}