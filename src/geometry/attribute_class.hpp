#pragma once

#include <string_view>

namespace tracey
{
    // Houdini-style geometry attribute owners. Every attribute is keyed on
    // (AttributeClass, name) and addresses one element of that class.
    //
    //   Point     — N points; e.g. P (position), N (normal), Cd (colour).
    //   Vertex    — N face-corners (each references a point); e.g. uv.
    //   Primitive — N primitives (one per face); e.g. material id.
    //   Detail    — exactly one element per geometry; e.g. bounding box.
    enum class AttributeClass
    {
        Point,
        Vertex,
        Primitive,
        Detail,
    };

    inline std::string_view attribute_class_name(AttributeClass cls)
    {
        switch (cls)
        {
        case AttributeClass::Point:     return "point";
        case AttributeClass::Vertex:    return "vertex";
        case AttributeClass::Primitive: return "primitive";
        case AttributeClass::Detail:    return "detail";
        }
        return "?";
    }
}
