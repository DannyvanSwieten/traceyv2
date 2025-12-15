#pragma once
#include "../buffer.hpp"
namespace tracey
{
    class CpuBuffer : public Buffer
    {
    public:
        CpuBuffer(uint32_t size);
        ~CpuBuffer() override;

        void *mapForWriting() override;
        const void *mapForReading() const override;
        void mapRange(uint32_t offset, uint32_t size) override;
        void flush() override;
        void flushRange(uint32_t offset, uint32_t size) override;

    private:
        void *m_data = nullptr;
        uint32_t m_size = 0;
    };
};