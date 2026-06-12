#include "backend_registry.hpp"

#include "../backends/cpu/cpu_path_tracer_backend.hpp"
#ifdef TRACEY_PT_HAS_METAL
#include "../backends/metal/metal_pathtracer_backend.hpp"
#endif

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace tracey
{
    namespace
    {
        std::string toLower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return s;
        }
    }

    PathTracerBackendKind pathTracerBackendKindFromString(const std::string &name)
    {
        const std::string n = toLower(name);
        if (n == "metal")      return PathTracerBackendKind::MetalRT;
        if (n == "vulkan_rt")  return PathTracerBackendKind::VulkanRT;
        if (n == "cpu")        return PathTracerBackendKind::Cpu;
        return PathTracerBackendKind::Auto;
    }

    const char *pathTracerBackendKindName(PathTracerBackendKind kind)
    {
        switch (kind)
        {
        case PathTracerBackendKind::Auto:     return "auto";
        case PathTracerBackendKind::MetalRT:  return "metal";
        case PathTracerBackendKind::VulkanRT:         return "vulkan_rt";
        case PathTracerBackendKind::Cpu:              return "cpu";
        }
        return "auto";
    }

    bool isPathTracerBackendAvailable(PathTracerBackendKind kind, Device *device)
    {
        switch (kind)
        {
        case PathTracerBackendKind::Auto:
            return true;
        case PathTracerBackendKind::MetalRT:
#ifdef TRACEY_PT_HAS_METAL
            return metalRTBackendSupported(device);
#else
            return false;
#endif
        case PathTracerBackendKind::VulkanRT:
            // Registered for the future Windows/Linux hardware-RT backend.
            return false;
        case PathTracerBackendKind::Cpu:
            // Pure host-side renderer; the façade uploads its pixels into a
            // Vulkan image when a GPU device is present (viewport), and
            // readback() works regardless.
            return true;
        }
        return false;
    }

    std::unique_ptr<PathTracerBackend> createPathTracerBackend(PathTracerBackendKind kind,
                                                               Device *device)
    {
        // Per-launch override for A/B testing backends without touching any
        // caller's configuration. Explicit env beats whatever was requested.
        if (const char *env = std::getenv("TRACEY_PT_BACKEND"))
        {
            kind = pathTracerBackendKindFromString(env);
        }

        auto construct = [](PathTracerBackendKind k) -> std::unique_ptr<PathTracerBackend> {
            switch (k)
            {
            case PathTracerBackendKind::MetalRT:
#ifdef TRACEY_PT_HAS_METAL
                return createMetalRTBackend();
#else
                break;
#endif
            case PathTracerBackendKind::Cpu:
                return std::make_unique<CpuPathTracerBackend>();
            case PathTracerBackendKind::VulkanRT:
            case PathTracerBackendKind::Auto:
                break;
            }
            throw std::runtime_error("Unreachable: unhandled path tracer backend kind");
        };

        if (kind == PathTracerBackendKind::Auto)
        {
            // Preference order: platform-native hardware RT first, then
            // the universal CPU fallback.
            const PathTracerBackendKind order[] = {
                PathTracerBackendKind::MetalRT,
                PathTracerBackendKind::VulkanRT,
                PathTracerBackendKind::Cpu,
            };
            for (PathTracerBackendKind candidate : order)
            {
                if (isPathTracerBackendAvailable(candidate, device))
                    return construct(candidate);
            }
            throw std::runtime_error("No path tracer backend available on this machine");
        }

        if (!isPathTracerBackendAvailable(kind, device))
        {
            throw std::runtime_error(std::string("Path tracer backend '") +
                                     pathTracerBackendKindName(kind) +
                                     "' is not available on this machine");
        }
        return construct(kind);
    }
} // namespace tracey
