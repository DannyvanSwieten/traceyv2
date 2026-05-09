#include "attribute.hpp"

namespace tracey
{
    template <> const char *Attribute<float>::typeTag() const { return "float"; }
    template <> const char *Attribute<int>::typeTag() const { return "int"; }
    template <> const char *Attribute<Vec2>::typeTag() const { return "vec2"; }
    template <> const char *Attribute<Vec3>::typeTag() const { return "vec3"; }
    template <> const char *Attribute<Vec4>::typeTag() const { return "vec4"; }
    template <> const char *Attribute<Mat3>::typeTag() const { return "mat3"; }
    template <> const char *Attribute<Mat4>::typeTag() const { return "mat4"; }
    template <> const char *Attribute<std::string>::typeTag() const { return "string"; }
}
