#pragma once
#include "uvec.hpp"
#include "vec.hpp"
#include "mat.hpp"
#include "uvec.hpp"
#include "ivec.hpp"
namespace rt
{
    struct Builtins
    {
        uvec3 glLaunchIDEXT;
        uvec3 glLaunchSizeEXT;
        int glPrimitiveID;
        int glInstanceID;
        int glInstanceCustomIndexEXT;
        int glGeometryIndexEXT;
        float glRayTminEXT;
        float glRayTmaxEXT;
        unsigned int glIncomingRayFlagsEXT;
        float glHitTEXT;
        unsigned int glHitKindEXT;
        mat3x4 glWorldToObjectEXT;
        mat3x4 glObjectToWorldEXT;
    };

    // Each thread gets its own copy
    extern "C" thread_local Builtins g_Builtins;
    extern "C" void setBuiltins(const Builtins &b);
    extern "C" void getBuiltins(Builtins *b);

    // ------------------------------------------------------------
    // GLSL-like vector types with common swizzles.
    // NOTE: These are intended for generated shader code, not ABI-stable structs.
    // ------------------------------------------------------------

    struct Image2D_T;
    using image2d = Image2D_T *;

    using Buffer_T = void *;
    using buffer = Buffer_T;

    using Payload_T = void *;
    using payload = Payload_T;

    extern "C" thread_local payload payloads[4];
    extern "C" void setPayload(payload *p, unsigned int index);
    extern "C" void getPayload(payload *p, unsigned int index);

    //
    // Opaque acceleration structure handle, matching GLSL:
    //   accelerationStructureEXT topLevel
    //
    struct TopLevelAccelerationStructure_T;
    using accelerationStructureEXT = TopLevelAccelerationStructure_T *;

// Shader code sees these as “gl_” builtins:
#define gl_LaunchIDEXT g_Builtins.glLaunchIDEXT
#define gl_LaunchSizeEXT g_Builtins.glLaunchSizeEXT
#define gl_PrimitiveID g_Builtins.glPrimitiveID
#define gl_InstanceID g_Builtins.glInstanceID
#define gl_InstanceCustomIndexEXT g_Builtins.glInstanceCustomIndexEXT
#define gl_GeometryIndexEXT g_Builtins.glGeometryIndexEXT
#define gl_RayTminEXT g_Builtins.glRayTminEXT
#define gl_RayTmaxEXT g_Builtins.glRayTmaxEXT
#define gl_IncomingRayFlagsEXT g_Builtins.glIncomingRayFlagsEXT
#define gl_HitTEXT g_Builtins.glHitTEXT
#define gl_HitKindEXT g_Builtins.glHitKindEXT
#define gl_WorldToObjectEXT g_Builtins.glWorldToObjectEXT
#define gl_ObjectToWorldEXT g_Builtins.glObjectToWorldEXT

    using TraceRaysFunc_t = void (*)(accelerationStructureEXT tlas, unsigned int flags, unsigned int cullMask, unsigned int sbtRecordOffset, unsigned int sbtRecordStride, unsigned int missIndex, const vec3 &origin, float tMin, const vec3 &direction, float tMax, unsigned int payloadIndex);
    extern "C" TraceRaysFunc_t traceRaysEXT;

    using ImageStoreFunc_t = void (*)(image2d image, uvec2 coord, vec4 value);
    extern "C" ImageStoreFunc_t imageStore;

}