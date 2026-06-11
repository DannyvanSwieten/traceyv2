// Single source of truth for the unified geometry-I/O port contract.
//
// The port ORDER in these arrays is load-bearing: geo_input / geo_output
// (geo_io_vops.cpp) declare their ports by iterating them, the GPU
// emitter (codegen/glsl_emit.cpp) uses the port index as the key when
// reading/writing the matching SSBO, and the dispatcher
// (codegen/compute_dispatch.cpp) materialises missing attributes with
// the same canonical defaults. Adding/reordering an entry here changes
// all three in lockstep — which is the point.

#pragma once

#include "../core/types.hpp"

#include <array>
#include <string>

namespace tracey
{
    namespace vops
    {
        struct GeoVecPortSpec
        {
            const char *name;        // attribute name on the geometry
            Vec3 defaultValue;       // canonical default (CPU eval + materialise)
            const char *defaultGlsl; // the same value as a GLSL literal
        };
        struct GeoFloatPortSpec
        {
            const char *name;
            float defaultValue;
            const char *defaultGlsl;
        };

        inline constexpr std::array<GeoVecPortSpec, 6> kGeoVecPorts = {{
            {"P",     Vec3(0.0f, 0.0f, 0.0f), "vec3(0.0)"},
            {"N",     Vec3(0.0f, 1.0f, 0.0f), "vec3(0.0, 1.0, 0.0)"},
            {"Cd",    Vec3(1.0f, 1.0f, 1.0f), "vec3(1.0)"},
            {"uv",    Vec3(0.0f, 0.0f, 0.0f), "vec3(0.0)"},
            {"v",     Vec3(0.0f, 0.0f, 0.0f), "vec3(0.0)"},
            {"force", Vec3(0.0f, 0.0f, 0.0f), "vec3(0.0)"},
        }};
        inline constexpr std::array<GeoFloatPortSpec, 2> kGeoFloatPorts = {{
            {"Alpha",  1.0f, "1.0"},
            {"pscale", 1.0f, "1.0"},
        }};
        // Read-only float ports — geo_input only. ptnum is synthesised
        // from the point index, never stored as an attribute.
        inline constexpr std::array<GeoFloatPortSpec, 3> kGeoReadOnlyFloatPorts = {{
            {"age",   0.0f, "0.0"},
            {"life",  1.0f, "1.0"},
            {"ptnum", 0.0f, "0.0"},
        }};

        // Canonical default for an attribute name, falling back to zero
        // for non-standard names. Both the CPU evaluator's missing-
        // attribute fallback and the dispatcher's materialise path go
        // through these so a missing attribute reads the same on both.
        inline Vec3 geoDefaultVec3For(const std::string &name)
        {
            for (const auto &p : kGeoVecPorts)
                if (name == p.name) return p.defaultValue;
            return Vec3(0.0f);
        }
        inline float geoDefaultFloatFor(const std::string &name)
        {
            for (const auto &p : kGeoFloatPorts)
                if (name == p.name) return p.defaultValue;
            for (const auto &p : kGeoReadOnlyFloatPorts)
                if (name == p.name) return p.defaultValue;
            return 0.0f;
        }
    }
}
