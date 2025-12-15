#pragma once
#include "vec.hpp"
namespace rt
{
    struct mat3x4
    {
        vec4 rows[3];
    };

    inline vec4 operator*(const mat3x4 &m, const vec4 &v)
    {
        return {
            m.rows[0].x * v.x + m.rows[0].y * v.y + m.rows[0].z * v.z + m.rows[0].w * v.w,
            m.rows[1].x * v.x + m.rows[1].y * v.y + m.rows[1].z * v.z + m.rows[1].w * v.w,
            m.rows[2].x * v.x + m.rows[2].y * v.y + m.rows[2].z * v.z + m.rows[2].w * v.w,
            1.0f};
    }
}