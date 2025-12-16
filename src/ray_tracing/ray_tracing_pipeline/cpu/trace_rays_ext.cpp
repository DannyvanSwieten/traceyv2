#include "trace_rays_ext.hpp"
#include "cpu_descriptor_set.hpp"
#include "../../../device/cpu/cpu_top_level_acceleration_structure.hpp"
#include "../../../core/tlas.hpp"
#include "../../../device/cpu/cpu_image_2d.hpp"
namespace tracey
{
    void traceRaysExtFunc(rt::accelerationStructureEXT tlas, unsigned int flags, unsigned int cullMask, unsigned int sbtRecordOffset, unsigned int sbtRecordStride, unsigned int missIndex, const rt::vec3 &origin, float tMin, const rt::vec3 &direction, float tMax, unsigned int payloadIndex)
    {
        auto tlasInterface = reinterpret_cast<DispatchedTlas *>(tlas);
        const Tlas &cpuTlas = dynamic_cast<const CpuTopLevelAccelerationStructure *>(tlasInterface->tlasInterface)->tlas();
        const auto &sbt = tlasInterface->pipeline->compiledSbt();
        tracey::Ray ray;
        ray.origin = Vec3{origin.x, origin.y, origin.z};
        ray.direction = Vec3{direction.x, direction.y, direction.z};
        ray.invDirection = Vec3{1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z};

        if (const auto intersection = cpuTlas.intersect(ray, tMin, tMax, RAY_FLAG_OPAQUE); intersection)
        {
            rt::Builtins builtins;
            sbt.rayGen.getBuiltins(&builtins);
            builtins.glPrimitiveID = static_cast<int>(intersection->primitiveId);
            builtins.glInstanceID = static_cast<int>(intersection->instanceId);
            builtins.glRayTminEXT = tMin;
            builtins.glRayTmaxEXT = tMax;
            builtins.glHitTEXT = intersection->t;
            builtins.glIncomingRayFlagsEXT = flags;
            // Set builtins for the hit shader
            sbt.hits[0].setBuiltins(builtins);
            // transfer payload to shader
            auto raygenPayload = sbt.rayGen.shader.payloadSlots[payloadIndex];
            raygenPayload->getPayload(&raygenPayload->payloadPtr, payloadIndex);

            auto &hitPayloadSlot = sbt.hits[0].shader.payloadSlots[payloadIndex];
            hitPayloadSlot->setPayload(&raygenPayload->payloadPtr, payloadIndex);
            sbt.hits[0].shader.func();
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