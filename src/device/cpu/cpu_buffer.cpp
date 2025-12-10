#include "cpu_buffer.hpp"
#include <cassert>
#include <cstdlib>

namespace tracey
{
    CpuBuffer::CpuBuffer(uint32_t size) : m_size(size)
    {
        m_data = std::malloc(size);
    }

    void *CpuBuffer::mapForWriting()
    {
        return m_data;
    }

    const void *CpuBuffer::mapForReading() const
    {
        return m_data;
    }

    void CpuBuffer::mapRange(uint32_t offset, uint32_t size)
    {
        (void)offset;
        (void)size;
        assert(offset + size <= m_size);
    }

    void CpuBuffer::flush()
    {
    }

    void CpuBuffer::flushRange(uint32_t offset, uint32_t size)
    {
        (void)offset;
        (void)size;
    }
} // namespace tracey