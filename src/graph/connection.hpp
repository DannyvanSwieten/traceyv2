#pragma once
#include <cstddef>
namespace tracey
{
    struct Connection
    {
        size_t fromNode;
        size_t fromPort;
        size_t toNode;
        size_t toPort;
    };
}