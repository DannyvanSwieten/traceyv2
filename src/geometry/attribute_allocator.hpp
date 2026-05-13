#pragma once

// Process-wide Device registration for GPU-backed Attribute storage.
// Phase A of the GPU-resident Geometry refactor. EditorServer (and any
// other host with a render device) registers its Device at startup;
// Attribute<T> reads the registration to lazily allocate its SSBO
// when buffer() is first called.
//
// Lifetime: the caller MUST keep the registered Device alive while the
// pointer is set. Clear via setDevice(nullptr) before tearing down the
// device. When unset, Attribute<T>::buffer() / bufferConst() return
// nullptr — callers fall back to the CPU representation. This makes
// headless smoke tests + CPU-only callers safe-by-default.
//
// Mirrors VopComputeDispatcher::setGlobal/getGlobal — same shape, same
// rules. Kept here (in geometry/) so attribute.cpp doesn't pull in any
// engine-specific headers.

namespace tracey
{
    class Device;

    class AttributeAllocator
    {
    public:
        static Device *getDevice();
        static void setDevice(Device *device);
    };
}
