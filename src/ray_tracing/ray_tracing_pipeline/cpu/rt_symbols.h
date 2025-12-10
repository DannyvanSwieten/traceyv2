#pragma once
namespace rt
{
    struct uvec3
    {
        unsigned int x, y, z;
    };

    struct Builtins
    {
        uvec3 GlobalInvocationID;
        uvec3 NumWorkGroups;
        uvec3 WorkGroupID;
        uvec3 WorkGroupSize;
    };

    // Each thread gets its own copy
    static thread_local Builtins g_builtins;

    extern "C" void setBuiltins(const Builtins &b)
    {
        g_builtins = b;
    }

    struct vec3
    {
        float x, y, z;
    };

    struct Image2D_T;
    using Image2D = Image2D_T *;

    struct Buffer_T;
    using Buffer = Buffer_T *;

    //
    // Opaque acceleration structure handle, matching GLSL:
    //   accelerationStructureEXT topLevel
    //
    struct accelerationStructureEXT_T;
    using accelerationStructureEXT = accelerationStructureEXT_T *;

// Shader code sees these as “gl_” builtins:
#define gl_GlobalInvocationID g_builtins.GlobalInvocationID
#define gl_NumWorkGroups g_builtins.NumWorkGroups
#define gl_WorkGroupID g_builtins.WorkGroupID
#define gl_WorkGroupSize g_builtins.WorkGroupSize
}