#include "camera.hpp"
#include <glm/gtc/quaternion.hpp>

namespace tracey
{
    Camera Camera::fitToBounds(const Vec3 &minBounds, const Vec3 &maxBounds, float fovDegrees)
    {
        Camera camera;

        // Compute scene center and size
        Vec3 sceneCenter = (minBounds + maxBounds) * 0.5f;
        Vec3 sceneSize = maxBounds - minBounds;
        float sceneRadius = glm::length(sceneSize) * 0.5f;

        // Position camera at a distance that fits the scene in view
        float cameraDistance = sceneRadius * 2.5f;
        Vec3 cameraPosition = sceneCenter + Vec3(0.0f, sceneRadius * 0.5f, cameraDistance);

        // Look-at rotation toward the scene centre.
        Vec3 worldUp(0.0f, 1.0f, 0.0f);
        Mat4 lookAtMatrix = glm::lookAt(cameraPosition, sceneCenter, worldUp);
        Quaternion rotation = glm::quat_cast(glm::inverse(lookAtMatrix));

        camera.setPosition(cameraPosition);
        camera.setRotation(rotation);
        camera.setFov(fovDegrees);

        return camera;
    }
} // namespace tracey
