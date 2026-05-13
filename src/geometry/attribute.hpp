#pragma once

#include "../core/types.hpp"
#include "../device/buffer.hpp"
#include "attribute_class.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <vector>

namespace tracey
{
    // Buffer is defined via the include above; including it here (vs
    // forward-declaring) lets std::unique_ptr<Buffer> emit its
    // destructor inline. buffer.hpp is a tiny abstract-only header so
    // the include cost is negligible.

    // Type-erased attribute base. Stored owner-side as unique_ptr<AttributeBase>
    // in AttributeTable; downcast to Attribute<T> to read/write typed data.
    //
    // Phase A of the GPU-resident Geometry refactor extends this base with:
    //   • A lazy GPU-side storage buffer (allocated on first buffer()/
    //     bufferConst() call against a registered AttributeAllocator
    //     device).
    //   • A `Side` discriminator tracking which side holds the
    //     authoritative copy (CPU vector / GPU buffer / Both in sync).
    //   • A `generation` counter that bumps on every mutating access
    //     — used by cook caches + apply_emitted signatures instead of
    //     hashing content bytes.
    //
    // The CPU vector is always the lower-bound representation: read-
    // only operations that need CPU data (e.g. the const data()
    // accessors below, save_scene serialisation) silently download
    // from the GPU when GPU is the current side. Mutating CPU
    // operations leave the GPU side stale until the next GPU access
    // re-uploads. This is the "lazy cache" pattern — the same
    // attribute lives on whichever side last touched it, and only
    // synchronises when both sides are needed.
    class AttributeBase
    {
    public:
        AttributeBase(std::string name, AttributeClass cls)
            : m_name(std::move(name)), m_class(cls) {}
        // Out-of-line so the `std::unique_ptr<Buffer>` member can hold
        // a forward-declared Buffer in this header; the destructor is
        // emitted in attribute.cpp where buffer.hpp is included.
        virtual ~AttributeBase();

        const std::string &name() const { return m_name; }
        AttributeClass attributeClass() const { return m_class; }

        // Element count this attribute carries. The vector and the GPU
        // buffer are always sized in lockstep (resize() invalidates
        // the buffer; the next GPU access reallocates at the current
        // vector size).
        virtual size_t size() const = 0;
        virtual void resize(size_t n) = 0;

        // For codegen + serialization: the C++ element type. Compared via
        // std::type_index for fast equality without RTTI on the hot path.
        virtual std::type_index typeIndex() const = 0;

        // Stable string tag for JSON serialization. One of:
        //   "float", "int", "vec2", "vec3", "vec4", "mat3", "mat4", "string"
        virtual const char *typeTag() const = 0;

        // Deep copy. The clone resets to CPU-side; the GPU buffer (if
        // any) is NOT copied — the next GPU access on the clone
        // re-uploads. Phase E will graduate this to a vkCmdCopyBuffer
        // path so cloning a GPU-resident attribute stays on the GPU.
        virtual std::unique_ptr<AttributeBase> clone() const = 0;

        // ── GPU-resident state (Phase A) ──

        // Monotonic per-attribute counter. Bumps on every mutating
        // access (non-const data(), at(), resize(), buffer()) and on
        // sync-from-GPU-after-kernel-write. Cache keys + change
        // detection use this instead of hashing content bytes.
        uint64_t generation() const { return m_generation; }

        // Bytes per element in the GPU buffer. Differs from sizeof(T)
        // for std430-padded types: Vec3 → 16 bytes (vec3 array stride
        // in std430), Vec2 → 8 bytes, float/int/Vec4 → sizeof(T).
        // Throws std::runtime_error for types we haven't taught GPU
        // packing for yet (strings, matrices).
        virtual size_t gpuStride() const = 0;

        // Mutating GPU accessor. Ensures the GPU buffer is allocated
        // + populated from the CPU vector if it wasn't already
        // current, then transitions side to Gpu (subsequent CPU reads
        // download). Bumps the generation. Returns nullptr when no
        // device is registered (AttributeAllocator unset) or the
        // attribute is empty.
        Buffer *buffer();

        // Read-only GPU access. Uploads if CPU side was current, then
        // transitions to Both (either side is now valid). Does NOT
        // bump the generation. Returns nullptr under the same
        // conditions as buffer().
        const Buffer *bufferConst() const;

    protected:
        enum class Side : uint8_t { Cpu, Gpu, Both };

        // Sync the CPU vector to match the GPU buffer if the latter
        // was the current authority. No-op when CPU is already
        // current or in Both state. Marked const because the logical
        // value of the attribute hasn't changed — only the
        // representation has caught up.
        void syncToCpu() const;

        // Inverse direction. Allocates the GPU buffer if it doesn't
        // exist yet (using the registered AttributeAllocator device).
        // After this returns, side == Both. Returns false silently
        // when no device is registered.
        bool syncToGpu() const;

        // Subclass hooks for per-type packing. Vec3 in particular
        // needs to pad to 16-byte stride for std430.
        virtual void uploadCpuToGpu(Buffer *dst) const = 0;
        virtual void downloadGpuToCpu(const Buffer *src) const = 0;

        // All three are mutable so const-context syncs (downloads on
        // const-data() etc.) can update the cache without a const_cast
        // dance. The attribute's logical identity / value doesn't
        // change — only which side is current.
        mutable Side m_side = Side::Cpu;
        mutable std::unique_ptr<Buffer> m_buffer;
        mutable uint64_t m_generation = 0;

    private:
        std::string m_name;
        AttributeClass m_class;
    };

    // Typed attribute holding `vector<T>` of element data. T ∈ {float, int,
    // Vec2, Vec3, Vec4, Mat3, Mat4, std::string}. GPU side is only
    // supported for the numeric types — see gpuStride specializations
    // in attribute.cpp.
    template <typename T>
    class Attribute : public AttributeBase
    {
    public:
        Attribute(std::string name, AttributeClass cls, size_t size, T def = T{})
            : AttributeBase(std::move(name), cls), m_data(size, def), m_default(std::move(def)) {}

        size_t size() const override { return m_data.size(); }

        void resize(size_t n) override
        {
            // Download first so any GPU-side writes survive the resize.
            // The new tail is filled with m_default on the CPU; the GPU
            // buffer is invalidated and reallocated on the next GPU
            // access against the now-extended vector.
            syncToCpu();
            m_data.resize(n, m_default);
            m_buffer.reset();
            m_side = Side::Cpu;
            ++m_generation;
        }

        std::type_index typeIndex() const override { return std::type_index(typeid(T)); }
        const char *typeTag() const override;

        std::unique_ptr<AttributeBase> clone() const override
        {
            // Force the CPU vector to current state before duplicating
            // so the clone observes any pending GPU-side writes.
            syncToCpu();
            auto out = std::make_unique<Attribute<T>>(
                name(), attributeClass(), m_data.size(), m_default);
            out->m_data = m_data;
            // Preserve the source generation. Phase C uses generation
            // as an O(1) change-detection signal across the cook-
            // request side channel (dop_import stamps positions into a
            // detached AttributeTable; the downstream cook compares
            // generations to decide whether to re-run). If clone()
            // reset to 0 the signal would tick once per cook regardless
            // of whether the data changed.
            out->m_generation = m_generation;
            return out;
        }

        // ── Mutating CPU access ──
        // Both bump the generation and flip side to Cpu. Callers who
        // only want to read should use the const overloads below.
        std::vector<T> &data()
        {
            syncToCpu();
            m_side = Side::Cpu;
            ++m_generation;
            return m_data;
        }
        T &at(size_t i)
        {
            syncToCpu();
            m_side = Side::Cpu;
            ++m_generation;
            return m_data[i];
        }

        // ── Read-only CPU access ──
        // Downloads from the GPU if it was the current side. Does NOT
        // bump the generation — the value hasn't changed.
        const std::vector<T> &data() const
        {
            syncToCpu();
            return m_data;
        }
        const T &at(size_t i) const
        {
            syncToCpu();
            return m_data[i];
        }

        const T &defaultValue() const { return m_default; }

        // Default: throw — only the renderable scalar/vector types are
        // specialised below. Strings + matrices instantiate this body
        // and would surface as a runtime "type not GPU-renderable"
        // error if anything ever calls buffer() on them; the
        // AttributeBase sync helpers catch the throw and fall back to
        // CPU silently.
        size_t gpuStride() const override
        {
            throw std::runtime_error(
                "Attribute<T>: GPU storage not supported for this element type");
        }

    protected:
        void uploadCpuToGpu(Buffer * /*dst*/) const override
        {
            throw std::runtime_error(
                "Attribute<T>: upload not implemented for this element type");
        }
        void downloadGpuToCpu(const Buffer * /*src*/) const override
        {
            throw std::runtime_error(
                "Attribute<T>: download not implemented for this element type");
        }

    private:
        // mutable: const-data() needs to download into m_data when the
        // GPU side is the current authority. The attribute's identity
        // doesn't change — only the representation catches up.
        mutable std::vector<T> m_data;
        T m_default;
    };

    // typeTag specializations live in attribute.cpp.
    template <> const char *Attribute<float>::typeTag() const;
    template <> const char *Attribute<int>::typeTag() const;
    template <> const char *Attribute<Vec2>::typeTag() const;
    template <> const char *Attribute<Vec3>::typeTag() const;
    template <> const char *Attribute<Vec4>::typeTag() const;
    template <> const char *Attribute<Mat3>::typeTag() const;
    template <> const char *Attribute<Mat4>::typeTag() const;
    template <> const char *Attribute<std::string>::typeTag() const;

    // gpuStride specializations — only the GPU-renderable element
    // types have one; the rest throw at runtime. (Compile-time
    // would be cleaner but `gpuStride` is virtual.)
    template <> size_t Attribute<float>::gpuStride() const;
    template <> size_t Attribute<int>::gpuStride() const;
    template <> size_t Attribute<Vec2>::gpuStride() const;
    template <> size_t Attribute<Vec3>::gpuStride() const;
    template <> size_t Attribute<Vec4>::gpuStride() const;

    template <> void Attribute<float>::uploadCpuToGpu(Buffer *dst) const;
    template <> void Attribute<float>::downloadGpuToCpu(const Buffer *src) const;
    template <> void Attribute<int>::uploadCpuToGpu(Buffer *dst) const;
    template <> void Attribute<int>::downloadGpuToCpu(const Buffer *src) const;
    template <> void Attribute<Vec2>::uploadCpuToGpu(Buffer *dst) const;
    template <> void Attribute<Vec2>::downloadGpuToCpu(const Buffer *src) const;
    template <> void Attribute<Vec3>::uploadCpuToGpu(Buffer *dst) const;
    template <> void Attribute<Vec3>::downloadGpuToCpu(const Buffer *src) const;
    template <> void Attribute<Vec4>::uploadCpuToGpu(Buffer *dst) const;
    template <> void Attribute<Vec4>::downloadGpuToCpu(const Buffer *src) const;
}
