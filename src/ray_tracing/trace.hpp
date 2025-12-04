#pragma once
#include "../core/types.hpp"
#include <functional>
namespace tracey
{
    class Tlas;
    using RaytracerCallback = std::function<void(const UVec2 &location, const UVec2 &resolution, uint32_t iteration, const Tlas &tlas)>;

    void traceRays(const UVec2 &resolution, int tileSize, uint32_t iteration, const RaytracerCallback &callback, const Tlas &tlas);
}