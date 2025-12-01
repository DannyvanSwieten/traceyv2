#include "sampling.hpp"

namespace tracey
{
    Vec3 uniformSampleHemisphere(const Vec2 &xi)
    {
        float phi = 2.0f * glm::pi<float>() * xi.x;
        float cosTheta = 1.0f - xi.y;
        float sinTheta = glm::sqrt(1.0f - cosTheta * cosTheta);

        float x = sinTheta * glm::cos(phi);
        float y = sinTheta * glm::sin(phi);
        float z = cosTheta;

        return Vec3(x, y, z);
    }

    Vec3 cosineSampleHemisphere(const Vec2 &xi)
    {
        float phi = 2.0f * glm::pi<float>() * xi.x;
        float cosTheta = glm::sqrt(1.0f - xi.y);
        float sinTheta = glm::sqrt(xi.y);

        float x = sinTheta * glm::cos(phi);
        float y = sinTheta * glm::sin(phi);
        float z = cosTheta;

        return Vec3(x, y, z);
    }

} // namespace tracey