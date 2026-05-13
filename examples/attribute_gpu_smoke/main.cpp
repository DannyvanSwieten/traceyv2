// Phase A verification for the GPU-resident Attribute refactor.
//
// Exercises the dual-state shape end-to-end:
//   • Default construction is CPU-only; buffer() returns nullptr
//     until a Device is registered.
//   • After registering a Device, buffer() lazily allocates and
//     uploads the current CPU vector.
//   • Mutating via data() bumps the generation and re-uploads on the
//     next buffer() call (CPU is the new source of truth).
//   • Writing through the GPU buffer + flipping side back to GPU
//     surfaces in subsequent data() reads (download path).
//   • Vec3's 12 → 16 byte stride pack is correct (the trailing
//     padding doesn't leak into adjacent elements).
//   • resize() invalidates the GPU buffer; next buffer() rebuilds
//     it at the new size.
//
// Skipped silently when no Vulkan device is available — the
// pre-Device assertions still run.

#include "geometry/attribute.hpp"
#include "geometry/attribute_allocator.hpp"
#include "device/buffer.hpp"
#include "device/device.hpp"
#include "core/types.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
    int failures = 0;
    void check(bool ok, const char *what)
    {
        if (ok) std::printf("  ok   %s\n", what);
        else { ++failures; std::printf("  FAIL %s\n", what); }
    }
}

int main()
{
    using namespace tracey;
    std::printf("attribute_gpu_smoke:\n");

    // ── No device registered: GPU accessors should silently
    //    decline; the CPU-only API stays fully functional.
    {
        Attribute<Vec3> a("P", AttributeClass::Point, 3, Vec3(0.0f));
        a.data()[0] = Vec3(1, 2, 3);
        a.data()[1] = Vec3(4, 5, 6);
        a.data()[2] = Vec3(7, 8, 9);
        // Three mutating data() calls + 1 in constructor.size() is
        // 3 — generation should be at least 3 (one bump per call).
        check(a.size() == 3, "no-device: size == 3");
        check(a.data()[1] == Vec3(4, 5, 6), "no-device: CPU value readback");
        check(a.buffer() == nullptr, "no-device: buffer() returns nullptr");
        check(a.bufferConst() == nullptr, "no-device: bufferConst() returns nullptr");
    }

    // ── With a Device registered, exercise the full dual-state
    //    pipeline. Skip silently when no Vulkan backend is present.
    std::unique_ptr<Device> dev;
    try
    {
        dev.reset(createDevice(DeviceType::Gpu, DeviceBackend::Compute));
    }
    catch (const std::exception &e)
    {
        std::printf("[gpu] device unavailable (%s) — skipping GPU compare\n", e.what());
    }
    if (!dev)
    {
        std::printf("attribute_gpu_smoke: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    AttributeAllocator::setDevice(dev.get());

    // 1) Round-trip Vec3 through the GPU: write CPU values, get the
    //    buffer (forces upload + flips side to GPU), read raw bytes
    //    via mapForReading, verify they match the std430 layout.
    {
        Attribute<Vec3> a("P", AttributeClass::Point, 4, Vec3(0.0f));
        a.data()[0] = Vec3(1, 2, 3);
        a.data()[1] = Vec3(4, 5, 6);
        a.data()[2] = Vec3(7, 8, 9);
        a.data()[3] = Vec3(10, 11, 12);
        const uint64_t genBeforeGpu = a.generation();

        Buffer *buf = a.buffer();
        check(buf != nullptr, "vec3: buffer() returns a real buffer with device");
        check(a.generation() > genBeforeGpu, "vec3: buffer() bumped generation");

        // Verify the raw GPU layout — vec3 is 16-byte-strided in std430.
        const float *gpu = static_cast<const float *>(buf->mapForReading());
        check(gpu[0]  == 1 && gpu[1]  == 2 && gpu[2]  == 3,  "vec3: slot 0 .xyz on GPU");
        check(gpu[4]  == 4 && gpu[5]  == 5 && gpu[6]  == 6,  "vec3: slot 1 .xyz on GPU (16B stride)");
        check(gpu[8]  == 7 && gpu[9]  == 8 && gpu[10] == 9,  "vec3: slot 2 .xyz on GPU");
        check(gpu[12] == 10 && gpu[13] == 11 && gpu[14] == 12, "vec3: slot 3 .xyz on GPU");
        buf->unmap();
    }

    // 2) GPU → CPU sync: write through the GPU mapping, flip side to
    //    GPU by calling buffer() (which marks GPU as the authority),
    //    then read const data() and verify the CPU view downloaded.
    {
        Attribute<Vec3> a("P", AttributeClass::Point, 2, Vec3(0.0f));
        a.data()[0] = Vec3(0.5f, 0.5f, 0.5f);
        a.data()[1] = Vec3(0.5f, 0.5f, 0.5f);

        // Get write-side buffer (sync uploads first).
        Buffer *buf = a.buffer();
        check(buf != nullptr, "gpu→cpu: buffer() returned");
        if (buf)
        {
            // Write directly to GPU memory in std430 vec3 layout.
            float *gpu = static_cast<float *>(buf->mapForWriting());
            gpu[0]  = 100;  gpu[1]  = 200;  gpu[2]  = 300;  // slot 0 .xyz
            gpu[4]  = 400;  gpu[5]  = 500;  gpu[6]  = 600;  // slot 1 .xyz
            buf->unmap();
        }

        // Reading via const data() must download GPU → CPU; the
        // side flips to Both and the new values surface.
        const auto &cpuView = const_cast<const Attribute<Vec3>&>(a).data();
        check(cpuView[0] == Vec3(100, 200, 300),
              "gpu→cpu: const data() downloads slot 0");
        check(cpuView[1] == Vec3(400, 500, 600),
              "gpu→cpu: const data() downloads slot 1");
    }

    // 3) Generation counter: each mutating access bumps; const
    //    accessors don't. This is the load-bearing property the
    //    cook cache will key on in Phase C.
    {
        Attribute<float> a("density", AttributeClass::Point, 5, 0.0f);
        const uint64_t g0 = a.generation();
        a.data()[0] = 1.0f;  // mutating
        const uint64_t g1 = a.generation();
        check(g1 > g0, "generation: data() write bumps");
        (void)const_cast<const Attribute<float>&>(a).data();  // const read
        check(a.generation() == g1, "generation: const data() does NOT bump");
        (void)a.buffer();  // mutating GPU access
        check(a.generation() > g1, "generation: buffer() bumps");
    }

    // 4) Resize invalidates the buffer; the next GPU access re-
    //    allocates at the new size with the extended CPU contents.
    {
        Attribute<Vec3> a("P", AttributeClass::Point, 2, Vec3(0.0f));
        a.data()[0] = Vec3(1, 2, 3);
        a.data()[1] = Vec3(4, 5, 6);
        (void)a.buffer();  // forces GPU allocation

        a.resize(4);  // should drop the GPU buffer
        check(a.size() == 4, "resize: vector grew to 4");
        check(a.data()[0] == Vec3(1, 2, 3), "resize: existing element survived");
        check(a.data()[3] == Vec3(0.0f),   "resize: new element took default");

        // Fetch a fresh GPU buffer; it must reflect the new size.
        Buffer *buf2 = a.buffer();
        check(buf2 != nullptr, "resize: GPU buffer reallocated");
        if (buf2)
        {
            const float *gpu = static_cast<const float *>(buf2->mapForReading());
            check(gpu[0] == 1 && gpu[1] == 2 && gpu[2] == 3,
                  "resize: slot 0 preserved across reallocation");
            check(gpu[12] == 0 && gpu[13] == 0 && gpu[14] == 0,
                  "resize: new slot 3 zero-initialised");
            buf2->unmap();
        }
    }

    // 5) Float attributes round-trip via tight (4-byte) stride.
    //    Belt-and-braces: confirms the non-Vec3 path isn't broken
    //    by the new sync machinery.
    {
        Attribute<float> a("pscale", AttributeClass::Point, 3, 0.0f);
        a.data()[0] = 1.5f;
        a.data()[1] = 2.5f;
        a.data()[2] = 3.5f;
        Buffer *buf = a.buffer();
        const float *gpu = static_cast<const float *>(buf->mapForReading());
        check(gpu[0] == 1.5f && gpu[1] == 2.5f && gpu[2] == 3.5f,
              "float: round-trip 4-byte stride");
        buf->unmap();
    }

    // Clear the device pointer so Attribute teardown (when the
    // process exits) doesn't try to free against an already-destroyed
    // device. Matches what EditorServer does in its destructor.
    AttributeAllocator::setDevice(nullptr);

    std::printf("attribute_gpu_smoke: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
