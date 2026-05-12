#pragma once

namespace tracey
{
    namespace vops
    {
        // Populate VopRegistry with the v1 built-in VOP set. Must be called
        // exactly once at editor/process startup. See sops/register_builtins.hpp
        // for the rationale on why this is explicit (TUs in a static lib get
        // dropped by the linker without a referenced symbol).
        void registerBuiltinVops();
    }
}
