#pragma once

// DOP nodes reuse the SOP parameter system verbatim, exactly like VOPs.
// Parameter is plain data (typed value + optional animation channels) and
// has no SOP-specific behavior. Aliasing into tracey::dops keeps the
// namespace clean while avoiding a duplicate implementation.

#include "../sops/parameter.hpp"

namespace tracey
{
    namespace dops
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
