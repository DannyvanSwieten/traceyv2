#pragma once
#include <cstdint>
namespace tracey
{
    class Buffer
    {
    public:
        virtual ~Buffer() = default;
        virtual void *mapForWriting() = 0;
        virtual const void *mapForReading() const = 0;
        virtual void mapRange(uint32_t offset, uint32_t size) = 0;
        virtual void flush() = 0;
        virtual void flushRange(uint32_t offset, uint32_t size) = 0;
    };
}