#pragma once

#include "../core/types.hpp"

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

        // A node parameter. Typed by ParamType + std::variant payload so cook
        // implementations can `value.get<float>()` (or use the lookup helpers
        // on SopNode) without RTTI.
        struct Parameter
        {
            std::string name;
            ParamType type = ParamType::Float;
            std::variant<float, int, bool, Vec3, std::string> value;

            static Parameter makeFloat(std::string name, float v)
            {
                return {std::move(name), ParamType::Float, v};
            }
            static Parameter makeInt(std::string name, int v)
            {
                return {std::move(name), ParamType::Int, v};
            }
            static Parameter makeBool(std::string name, bool v)
            {
                return {std::move(name), ParamType::Bool, v};
            }
            static Parameter makeVec3(std::string name, Vec3 v)
            {
                return {std::move(name), ParamType::Vec3, v};
            }
            static Parameter makeString(std::string name, std::string v)
            {
                return {std::move(name), ParamType::String, std::move(v)};
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
    }
}
