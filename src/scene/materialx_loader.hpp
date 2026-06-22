#pragma once

#include "material_instance.hpp"

#include <string>
#include <vector>

namespace tracey
{
    // Imports MaterialX (.mtlx) surface materials into our MaterialInstance /
    // material-VM representation. We use only the MaterialX document model (Core
    // + Format) to parse and resolve the graph; rendering is done by our own
    // path tracer, so MaterialX `standard_surface` is mapped onto the extended
    // BSDF (clearcoat / sheen / subsurface / anisotropy) and arbitrary upstream
    // node graphs are compiled to material-VM bytecode.
    //
    // Built behind TRACEY_WITH_MATERIALX (deps/materialx via bootstrap). When
    // the build is compiled without it, loadMaterials() returns empty and
    // available() returns false — the core build never hard-depends on it.
    class MaterialXLoader
    {
    public:
        struct NamedMaterial
        {
            std::string name;          // material (or shader) node name
            MaterialInstance material; // mapped onto the engine BSDF
        };

        // Parse `path` and map every renderable surface material it defines.
        // Resolves input defaults against the MaterialX standard libraries.
        // Returns empty on error, when no surface material is present, or when
        // MaterialX support is not compiled in.
        static std::vector<NamedMaterial> loadMaterials(const std::string &path);

        // True when the build links MaterialX (TRACEY_HAS_MATERIALX).
        static bool available();
    };
}
