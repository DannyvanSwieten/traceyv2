#pragma once

#include "../../core/types.hpp"
#include <string>
#include <variant>

namespace tracey
{
    /**
     * @brief Universal value type for all parameters (not just materials)
     *
     * This variant type can hold any supported parameter value type.
     * It's used throughout the procedural node system for:
     * - Parameter storage
     * - Keyframe values
     * - Node evaluation results
     * - Material properties (MaterialInstance can migrate to use this)
     */
    using ParameterValue = std::variant<
        float,              // Scalar value
        Vec2,               // 2D vector
        Vec3,               // 3D vector (color, position, etc.)
        Vec4,               // 4D vector (RGBA, quaternion components, etc.)
        int,                // Integer value
        bool,               // Boolean flag
        std::string         // String (texture paths, expressions, etc.)
        // Future extensions:
        // Quaternion,      // Rotation as quaternion
        // Mat4,            // 4x4 transformation matrix
        // std::vector<T>   // Arrays/lists
    >;

    /**
     * @brief Type-safe getters for ParameterValue
     */
    template<typename T>
    inline const T* getValuePtr(const ParameterValue& value) {
        return std::get_if<T>(&value);
    }

    template<typename T>
    inline T getValue(const ParameterValue& value, const T& defaultValue = T{}) {
        if (auto* ptr = std::get_if<T>(&value)) {
            return *ptr;
        }
        return defaultValue;
    }

    /**
     * @brief Check if ParameterValue holds a specific type
     */
    template<typename T>
    inline bool holdsType(const ParameterValue& value) {
        return std::holds_alternative<T>(value);
    }

} // namespace tracey
