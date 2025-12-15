#pragma once
#include "runtime/rt_symbols.h"
#include "cpu_descriptor_set.hpp"
#include "cpu_ray_tracing_pipeline.hpp"
namespace tracey
{
    void traceRaysExtFunc(rt::accelerationStructureEXT tlas, unsigned int flags, unsigned int cullMask, unsigned int sbtRecordOffset, unsigned int sbtRecordStride, unsigned int missIndex, const rt::vec3 &origin, float tMin, const rt::vec3 &direction, float tMax, unsigned int payloadIndex);
    void imageStoreFunc(rt::image2d image, rt::uvec2 coord, rt::vec4 value);
}