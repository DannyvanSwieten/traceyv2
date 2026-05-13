#pragma once

namespace tracey
{
    namespace dops
    {
        // Populate DopRegistry with the v1 built-in DOP set. Must be called
        // exactly once at editor/process startup. See sops/register_builtins.hpp
        // for the rationale on why this is explicit (TUs in a static lib get
        // dropped by the linker without a referenced symbol).
        //
        // Phase 0 stub: empty. Phase 1 will register pop_source, pop_solver,
        // pop_gravity, pop_force, pop_kill.
        void registerBuiltinDops();
    }
}
