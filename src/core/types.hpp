#pragma once
// transforms
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/norm.hpp>

namespace tracey
{
    using Vec2 = glm::vec2;
    using Vec3 = glm::vec3;
    using Vec4 = glm::vec4;

    using IVec2 = glm::ivec2;
    using IVec3 = glm::ivec3;
    using IVec4 = glm::ivec4;

    using UVec2 = glm::uvec2;
    using UVec3 = glm::uvec3;
    using UVec4 = glm::uvec4;

    using Mat3 = glm::mat3;
    using Mat4 = glm::mat4;
    using Mat4x3 = glm::mat4x3;
    using Mat3x4 = glm::mat3x4;

    using Quaternion = glm::quat;

    template <typename T>
    float dot(const T &a, const T &b)
    {
        return glm::dot(a, b);
    }

    template <typename T>
    T cross(const T &a, const T &b)
    {
        return glm::cross(a, b);
    }

    template <typename T>
    T normalize(const T &v)
    {
        return glm::normalize(v);
    }

    template <typename T>
    T reflect(const T &I, const T &N)
    {
        return glm::reflect(I, N);
    }

    template <typename T>
    T clamp(const T &x, const T &minVal, const T &maxVal)
    {
        return glm::clamp(x, minVal, maxVal);
    }

    template <typename T>
    T mix(const T &x, const T &y, float a)
    {
        return glm::mix(x, y, a);
    }

    template <typename T>
    T saturate(const T &x)
    {
        return clamp(x, T(0), T(1));
    }

    template <typename T>
    T radians(const T &degrees)
    {
        return glm::radians(degrees);
    }

    template <typename T>
    T min(const T &a, const T &b)
    {
        return glm::min(a, b);
    }

    template <typename T>
    T max(const T &a, const T &b)
    {
        return glm::max(a, b);
    }

    template <typename T>
    auto length2(const T &v)
    {
        return glm::length2(v);
    }

    template <typename T>
    T pi()
    {
        return glm::pi<T>();
    }

    inline void invert3x4(const float m[3][4], float out[3][4])
    {
        // Extract linear part A (row-major)
        Mat3 A(
            m[0][0], m[1][0], m[2][0], // column 0
            m[0][1], m[1][1], m[2][1], // column 1
            m[0][2], m[1][2], m[2][2]  // column 2
        );

        // Extract translation
        Vec3 t(m[0][3], m[1][3], m[2][3]);

        // Invert linear part
        Mat3 Ainv = glm::inverse(A);

        // Invert translation
        Vec3 tinv = -(Ainv * t);

        // Write back as row-major 3x4
        Mat3 Ainv_row = glm::transpose(Ainv);

        out[0][0] = Ainv_row[0][0];
        out[0][1] = Ainv_row[0][1];
        out[0][2] = Ainv_row[0][2];
        out[0][3] = tinv.x;

        out[1][0] = Ainv_row[1][0];
        out[1][1] = Ainv_row[1][1];
        out[1][2] = Ainv_row[1][2];
        out[1][3] = tinv.y;

        out[2][0] = Ainv_row[2][0];
        out[2][1] = Ainv_row[2][1];
        out[2][2] = Ainv_row[2][2];
        out[2][3] = tinv.z;
    }

    inline Vec3 transformPoint(const float mat[3][4], Vec3 const &point)
    {
        return Vec3(
            mat[0][0] * point.x + mat[0][1] * point.y + mat[0][2] * point.z + mat[0][3],
            mat[1][0] * point.x + mat[1][1] * point.y + mat[1][2] * point.z + mat[1][3],
            mat[2][0] * point.x + mat[2][1] * point.y + mat[2][2] * point.z + mat[2][3]);
    }

    inline Vec3 transformPoint(const Mat4 &mat, Vec3 const &point)
    {
        Vec4 homogenousPoint(point, 1.0f);
        Vec4 transformed = mat * homogenousPoint;
        return Vec3(transformed);
    }

    inline Vec3 transformVector(const float mat[3][4], Vec3 const &vec)
    {
        return Vec3(
            mat[0][0] * vec.x + mat[0][1] * vec.y + mat[0][2] * vec.z,
            mat[1][0] * vec.x + mat[1][1] * vec.y + mat[1][2] * vec.z,
            mat[2][0] * vec.x + mat[2][1] * vec.y + mat[2][2] * vec.z);
    }

    inline Vec3 transformVector(const Mat4 &mat, Vec3 const &vec)
    {
        Vec4 homogenousVec(vec, 0.0f);
        Vec4 transformed = mat * homogenousVec;
        return Vec3(transformed);
    }
} // namespace tracey