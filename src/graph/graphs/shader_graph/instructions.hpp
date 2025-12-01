#pragma once

namespace tracey
{
    enum class ShaderGraphInstruction
    {
        // Binary Operations
        Add,
        Subtract,
        Multiply,
        Divide,
        DotProduct,
        CrossProduct,
        Reflect,
        // Unary Operations
        Negate,
        Normalize,
        Length,
        // Trigonometric
        Sine,
        Cosine,
        Tangent,
        // Other
        TextureSample,
        Lerp
    };
}