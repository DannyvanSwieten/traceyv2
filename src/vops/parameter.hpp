#pragma once

// VOP nodes reuse the SOP parameter system verbatim — it's plain data
// (typed value + optional animation channels) and has no SOP-specific
// behavior. Aliasing into tracey::vops keeps the namespace clean while
// avoiding a duplicate implementation.
//
// If VOPs ever need a parameter capability SOPs don't (e.g. per-attribute
// type tags for bind nodes), graduate this to a real type then.

#include "../sops/parameter.hpp"

namespace tracey
{
    namespace vops
    {
        using Parameter = tracey::sops::Parameter;
        using ParamType = tracey::sops::ParamType;
        using ScalarChannel = tracey::sops::ScalarChannel;

        inline const char *paramTypeName(ParamType t)
        {
            return tracey::sops::paramTypeName(t);
        }
    }
}
