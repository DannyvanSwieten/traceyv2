// Backend registry for the path tracer module. The single place that knows
// which PathTracerBackend implementations exist, which are available on the
// running machine, and how `Auto` resolves. Replaces the old
// selectPathTracerBackend free function.

#pragma once

#include "path_tracer_backend.hpp"

#include <memory>
#include <string>

namespace tracey
{
    // "auto" | "wavefront" | "metal" | "vulkan_rt" | "cpu" (case-insensitive).
    // Unknown strings resolve to Auto.
    PathTracerBackendKind pathTracerBackendKindFromString(const std::string &name);
    const char *pathTracerBackendKindName(PathTracerBackendKind kind);

    // Can this backend run on this machine/device? `Auto` is always available.
    bool isPathTracerBackendAvailable(PathTracerBackendKind kind, Device *device);

    // Resolve `Auto` to the preferred available backend and construct it.
    // Throws std::runtime_error when an explicitly requested backend is
    // unavailable on this machine.
    std::unique_ptr<PathTracerBackend> createPathTracerBackend(PathTracerBackendKind kind,
                                                               Device *device);
} // namespace tracey
