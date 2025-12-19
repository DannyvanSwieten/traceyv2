#include "trace_rays_ext.hpp"
#include "cpu_descriptor_set.hpp"
#include "../../../device/cpu/cpu_top_level_acceleration_structure.hpp"
#include "../../../core/tlas.hpp"
#include "../../../device/cpu/cpu_image_2d.hpp"
#include <limits>
namespace tracey
{
    void traceRaysExtFunc(rt::accelerationStructureEXT tlas, unsigned int flags, unsigned int cullMask, unsigned int sbtRecordOffset, unsigned int sbtRecordStride, unsigned int missIndex, const rt::vec3 &origin, float tMin, const rt::vec3 &direction, float tMax, unsigned int payloadIndex)
    {
        auto tlasInterface = reinterpret_cast<DispatchedTlas *>(tlas);
        const Tlas &cpuTlas = static_cast<const CpuTopLevelAccelerationStructure *>(tlasInterface->tlasInterface)->tlas();
        const auto &sbt = tlasInterface->pipeline->compiledSbt();
        tracey::Ray ray;
        ray.origin = Vec3{origin.x, origin.y, origin.z};
        ray.direction = Vec3{direction.x, direction.y, direction.z};
        auto safeRcp = [](float x) -> float
        {
            // Avoid inf/NaN on exact zero; matches typical RT behavior.
            return (x != 0.0f) ? (1.0f / x) : std::numeric_limits<float>::infinity();
        };
        ray.invDirection = Vec3{safeRcp(direction.x), safeRcp(direction.y), safeRcp(direction.z)};

        const auto intersection = cpuTlas.intersect(ray, tMin, tMax, RAY_FLAG_OPAQUE);

        if (intersection)
        {
            rt::Builtins builtins;
            sbt.rayGen.getBuiltins(&builtins);
            builtins.glPrimitiveID = static_cast<int>(intersection->primitiveId);
            builtins.glInstanceID = static_cast<int>(intersection->instanceId);
            builtins.glRayTminEXT = tMin;
            builtins.glRayTmaxEXT = tMax;
            builtins.glHitTEXT = intersection->t;
            builtins.glIncomingRayFlagsEXT = flags;
            builtins.glWorldNormalEXT = rt::vec3{intersection->normal.x, intersection->normal.y, intersection->normal.z};
            const auto &instanceTransforms = cpuTlas.getInstanceTransforms(intersection->instanceId);
            // Set object to world and world to object matrices
            builtins.glObjectToWorldEXT = reinterpret_cast<const rt::mat3x4 *>(&instanceTransforms.toWorld);
            builtins.glWorldToObjectEXT = reinterpret_cast<const rt::mat3x4 *>(&instanceTransforms.toObject);

            // Set builtins for the hit shader
            sbt.hits[0].setBuiltins(builtins);
            // transfer payload to shader without mutating shared slot state
            void *payloadPtr = nullptr;
            struct PayloadDummy
            {
                Vec3 dummy;
                bool hitDummy;
            };
            if (payloadIndex < sbt.rayGen.shader.payloadSlots.size() && sbt.rayGen.shader.payloadSlots[payloadIndex])
            {
                sbt.rayGen.shader.payloadSlots[payloadIndex]->getPayload(&payloadPtr, payloadIndex);
            }

            if (payloadIndex < sbt.hits[0].shader.payloadSlots.size() && sbt.hits[0].shader.payloadSlots[payloadIndex])
            {
                sbt.hits[0].shader.payloadSlots[payloadIndex]->setPayload(&payloadPtr, payloadIndex);
            }

            if (sbt.hits[0].shader.func)
            {
                sbt.hits[0].shader.func();
            }
        }
        else
        {
            // Miss shader
            rt::Builtins builtins;
            sbt.rayGen.getBuiltins(&builtins);
            builtins.glRayTminEXT = tMin;
            builtins.glRayTmaxEXT = tMax;
            builtins.glIncomingRayFlagsEXT = flags;

            // Set builtins for the miss shader
            sbt.misses[missIndex].setBuiltins(builtins);

            if (sbt.misses[missIndex].shader.func)
            {
                sbt.misses[missIndex].shader.func();
            }
        }

        (void)cullMask;
        (void)sbtRecordOffset;
        (void)sbtRecordStride;
        (void)missIndex;
    }
    void imageStoreFunc(rt::image2d image, rt::uvec2 coord, rt::vec4 value)
    {
        auto imgInterface = reinterpret_cast<Image2D *>(image);
        auto cpuImg = dynamic_cast<CpuImage2D *>(imgInterface);
        cpuImg->store(coord.x, coord.y, Vec4{value.x, value.y, value.z, value.w});
    };
}