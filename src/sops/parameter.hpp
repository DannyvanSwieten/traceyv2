#pragma once

#include "../core/types.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace tracey
{
    namespace sops
    {
        enum class ParamType
        {
            Float,
            Int,
            Bool,
            Vec3,
            String,
        };

        // Per-component animation channel.
        //
        // Storage is float for all numeric types; Int channels are rounded on
        // eval and Bool channels collapse to (value != 0). Vec3 parameters use
        // up to 3 channels (one per component, Houdini-style — each axis can
        // be animated independently).
        //
        // Time is stored in seconds (double). The UI converts seconds <-> frames
        // using the timeline's fps, so changing fps doesn't invalidate keys.
        struct ScalarChannel
        {
            enum class Interp : uint8_t
            {
                Step,
                Linear,
                Bezier,
            };
            enum class Extrap : uint8_t
            {
                Hold,    // clamp to nearest end key
                Cycle,   // repeat the [first,last] range
                Linear,  // continue using the end-key tangent
            };

            struct Key
            {
                double time = 0.0;
                float  value = 0.0f;
                // Tangents are dy/dx in (value-units per second). Used by Bezier.
                float  inTangent = 0.0f;
                float  outTangent = 0.0f;
                // Interpolation owned by the *outgoing* segment of this key.
                Interp interp = Interp::Linear;
            };

            std::vector<Key> keys;  // kept sorted by time
            Extrap pre = Extrap::Hold;
            Extrap post = Extrap::Hold;

            bool empty() const { return keys.empty(); }

            // Sample the channel at `time` (seconds). Returns 0 if empty.
            float evaluate(double time) const;

            // Insert or replace the key at k.time (within 1e-9 seconds).
            // Maintains sort order.
            void setKey(Key k);

            // Remove the key whose time is within `epsilon` of `time`.
            // Returns true if a key was removed.
            bool removeKeyAt(double time, double epsilon = 1e-6);
        };

        // A node parameter. Typed by ParamType + std::variant payload so cook
        // implementations can `value.get<float>()` (or use the lookup helpers
        // on SopNode) without RTTI.
        //
        // `value` is the constant baseline. If any entry in `channels` has at
        // least one keyframe, the parameter is animated; callers that need the
        // time-sampled value should use evaluateAt(t) (or paramXxxAt on SopNode).
        // Component layout:
        //   • Float / Int / Bool: channels[0] (size <= 1)
        //   • Vec3:               channels[0..2] = x,y,z (size <= 3)
        //   • String:             not animatable
        struct Parameter
        {
            std::string name;
            ParamType type = ParamType::Float;
            std::variant<float, int, bool, Vec3, std::string> value;
            std::vector<ScalarChannel> channels;  // empty entries allowed

            // True if any channel has at least one keyframe.
            bool isAnimated() const;

            // Sample the parameter at `time` (seconds). For animated components
            // the channel value wins; non-animated components fall back to the
            // matching component of the constant `value`.
            std::variant<float, int, bool, Vec3, std::string>
            evaluateAt(double time) const;

            // Mutable access to the channel at component `c` (0 for scalar
            // params, 0..2 for Vec3). Grows the channels vector as needed.
            ScalarChannel &channelAt(int component);

            static Parameter makeFloat(std::string name, float v)
            {
                return {std::move(name), ParamType::Float, v, {}};
            }
            static Parameter makeInt(std::string name, int v)
            {
                return {std::move(name), ParamType::Int, v, {}};
            }
            static Parameter makeBool(std::string name, bool v)
            {
                return {std::move(name), ParamType::Bool, v, {}};
            }
            static Parameter makeVec3(std::string name, Vec3 v)
            {
                return {std::move(name), ParamType::Vec3, v, {}};
            }
            static Parameter makeString(std::string name, std::string v)
            {
                return {std::move(name), ParamType::String, std::move(v), {}};
            }
        };

        inline const char *paramTypeName(ParamType t)
        {
            switch (t)
            {
            case ParamType::Float:  return "float";
            case ParamType::Int:    return "int";
            case ParamType::Bool:   return "bool";
            case ParamType::Vec3:   return "vec3";
            case ParamType::String: return "string";
            }
            return "?";
        }

        // String <-> enum helpers for serialization.
        const char *interpName(ScalarChannel::Interp i);
        ScalarChannel::Interp interpFromName(std::string_view name,
                                             ScalarChannel::Interp def = ScalarChannel::Interp::Linear);
        const char *extrapName(ScalarChannel::Extrap e);
        ScalarChannel::Extrap extrapFromName(std::string_view name,
                                             ScalarChannel::Extrap def = ScalarChannel::Extrap::Hold);

        // 64-bit fingerprint of a parameter's name + type + constant value +
        // channel keyframes. Used by the per-node cook cache to detect
        // "params unchanged since last cook" without comparing each field
        // pairwise. Stable within a single process run; not for on-disk.
        uint64_t hashParameter(const Parameter &p);
        uint64_t hashParameters(const std::vector<Parameter> &params);
    }
}
