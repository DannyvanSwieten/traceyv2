#include "attribute.hpp"

#include "../device/buffer.hpp"
#include "../device/device.hpp"
#include "attribute_allocator.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace tracey
{
    // Out-of-line destructor so `std::unique_ptr<Buffer>` in
    // AttributeBase can hold a forward-declared Buffer. Defined
    // here where buffer.hpp is in scope.
    AttributeBase::~AttributeBase() = default;

    // ── typeTag (verbatim from before) ──
    template <> const char *Attribute<float>::typeTag() const { return "float"; }
    template <> const char *Attribute<int>::typeTag() const { return "int"; }
    template <> const char *Attribute<Vec2>::typeTag() const { return "vec2"; }
    template <> const char *Attribute<Vec3>::typeTag() const { return "vec3"; }
    template <> const char *Attribute<Vec4>::typeTag() const { return "vec4"; }
    template <> const char *Attribute<Mat3>::typeTag() const { return "mat3"; }
    template <> const char *Attribute<Mat4>::typeTag() const { return "mat4"; }
    template <> const char *Attribute<std::string>::typeTag() const { return "string"; }

    // ── gpuStride — GPU buffer per-element byte size ──
    //
    // std430 packing rules:
    //   • float, int — 4 bytes (alignment 4)
    //   • vec2       — 8 bytes (alignment 8)
    //   • vec3 in an ARRAY — 16-byte stride (alignment 16; CPU's
    //     glm::vec3 is 12 bytes so we pad on upload, strip on download)
    //   • vec4       — 16 bytes (alignment 16, no padding)
    template <> size_t Attribute<float>::gpuStride() const { return sizeof(float); }
    template <> size_t Attribute<int>::gpuStride()   const { return sizeof(int); }
    template <> size_t Attribute<Vec2>::gpuStride()  const { return sizeof(Vec2); }
    template <> size_t Attribute<Vec3>::gpuStride()  const { return 16; }
    template <> size_t Attribute<Vec4>::gpuStride()  const { return sizeof(Vec4); }

    // ── Upload / download for the GPU-renderable element types ──
    //
    // The "tightly-packed" cases (float, int, Vec2, Vec4) memcpy
    // directly because CPU stride == GPU stride. Vec3 is the odd one
    // out: glm::vec3 is 12 bytes packed on the CPU, but std430 lays
    // out vec3 arrays at 16-byte stride. So Vec3 upload/download
    // walks the array element-by-element copying 12 bytes into / out
    // of 16-byte slots; the trailing 4 bytes of each slot are
    // padding that the GLSL kernel ignores.

    template <> void Attribute<float>::uploadCpuToGpu(Buffer *dst) const
    {
        void *p = dst->mapForWriting();
        std::memcpy(p, m_data.data(), m_data.size() * sizeof(float));
        dst->unmap();
    }
    template <> void Attribute<float>::downloadGpuToCpu(const Buffer *src) const
    {
        const void *p = src->mapForReading();
        std::memcpy(m_data.data(), p, m_data.size() * sizeof(float));
        src->unmap();
    }

    template <> void Attribute<int>::uploadCpuToGpu(Buffer *dst) const
    {
        void *p = dst->mapForWriting();
        std::memcpy(p, m_data.data(), m_data.size() * sizeof(int));
        dst->unmap();
    }
    template <> void Attribute<int>::downloadGpuToCpu(const Buffer *src) const
    {
        const void *p = src->mapForReading();
        std::memcpy(m_data.data(), p, m_data.size() * sizeof(int));
        src->unmap();
    }

    template <> void Attribute<Vec2>::uploadCpuToGpu(Buffer *dst) const
    {
        void *p = dst->mapForWriting();
        std::memcpy(p, m_data.data(), m_data.size() * sizeof(Vec2));
        dst->unmap();
    }
    template <> void Attribute<Vec2>::downloadGpuToCpu(const Buffer *src) const
    {
        const void *p = src->mapForReading();
        std::memcpy(m_data.data(), p, m_data.size() * sizeof(Vec2));
        src->unmap();
    }

    template <> void Attribute<Vec3>::uploadCpuToGpu(Buffer *dst) const
    {
        // 12-byte CPU vec3 → 16-byte std430 slot. Padding bytes left
        // uninitialised; the GLSL kernel only reads .xyz so it doesn't
        // matter what the trailing float contains.
        void *base = dst->mapForWriting();
        auto *p = static_cast<char *>(base);
        for (size_t i = 0; i < m_data.size(); ++i)
        {
            std::memcpy(p + i * 16, &m_data[i], sizeof(Vec3));
        }
        dst->unmap();
    }
    template <> void Attribute<Vec3>::downloadGpuToCpu(const Buffer *src) const
    {
        const void *base = src->mapForReading();
        const auto *p = static_cast<const char *>(base);
        for (size_t i = 0; i < m_data.size(); ++i)
        {
            std::memcpy(&m_data[i], p + i * 16, sizeof(Vec3));
        }
        src->unmap();
    }

    template <> void Attribute<Vec4>::uploadCpuToGpu(Buffer *dst) const
    {
        void *p = dst->mapForWriting();
        std::memcpy(p, m_data.data(), m_data.size() * sizeof(Vec4));
        dst->unmap();
    }
    template <> void Attribute<Vec4>::downloadGpuToCpu(const Buffer *src) const
    {
        const void *p = src->mapForReading();
        std::memcpy(m_data.data(), p, m_data.size() * sizeof(Vec4));
        src->unmap();
    }

    // ── Sync helpers (live on AttributeBase) ──
    //
    // The lazy-cache invariant is: at any moment, the current side
    // (Cpu / Gpu) holds the authoritative copy. The other side is
    // either stale (newer was last written there) or non-existent (no
    // GPU buffer allocated yet). Both means "in sync; either side is
    // safe to read". Const accessors silently bring the other side
    // up to date when needed; mutating accessors flip the current
    // side and bump the generation.

    void AttributeBase::syncToCpu() const
    {
        if (m_side == Side::Gpu && m_buffer)
        {
            downloadGpuToCpu(m_buffer.get());
            m_side = Side::Both;
        }
    }

    bool AttributeBase::syncToGpu() const
    {
        if (m_side == Side::Gpu) return true;
        if (m_side == Side::Both && m_buffer) return true;
        // CPU side authoritative — need a populated GPU buffer.
        Device *dev = AttributeAllocator::getDevice();
        if (!dev) return false;
        const size_t n = size();
        if (n == 0) return false;  // can't allocate a zero-byte SSBO
        if (!m_buffer)
        {
            try
            {
                size_t stride = 0;
                try { stride = gpuStride(); }
                catch (...) { return false; }
                m_buffer.reset(dev->createBuffer(
                    static_cast<uint32_t>(n * stride),
                    BufferUsage::StorageBuffer));
            }
            catch (const std::exception &)
            {
                // Allocation failed — leave side at Cpu so callers
                // can still operate on the vector.
                return false;
            }
        }
        uploadCpuToGpu(m_buffer.get());
        m_side = Side::Both;
        return true;
    }

    Buffer *AttributeBase::buffer()
    {
        if (!syncToGpu()) return nullptr;
        // The caller is asking for write access: assume the GPU side
        // is about to diverge from CPU. Bump the generation and flip
        // side to Gpu so the next CPU read triggers a download.
        m_side = Side::Gpu;
        ++m_generation;
        return m_buffer.get();
    }

    const Buffer *AttributeBase::bufferConst() const
    {
        if (!syncToGpu()) return nullptr;
        return m_buffer.get();
    }
}
