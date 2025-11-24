#pragma once
#include "ray.hpp"
namespace tracey
{

    bool intersectAABB(const Ray &r, const tracey::Vec3 &bmin, const tracey::Vec3 &bmax, float minT, float maxT,
                       float &tEnter, float &tExit);
    bool intersectTriangle(const Ray &ray, const Vec3 &v0, const Vec3 &v1, const Vec3 &v2, float &tOut, float &uOut, float &vOut);

    std::tuple<tracey::Vec3, tracey::Vec3> transformAABB(const tracey::Mat4 &M,
                                                         const tracey::Vec3 &localMin,
                                                         const tracey::Vec3 &localMax);
}