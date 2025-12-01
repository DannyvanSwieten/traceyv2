#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
// transforms
#define GLM_ENABLE_EXPERIMENTAL
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
} // namespace tracey