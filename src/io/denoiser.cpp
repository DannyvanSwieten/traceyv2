#include "denoiser.hpp"

#ifdef TRACEY_HAS_OIDN
#include <OpenImageDenoise/oidn.h>
#endif

namespace tracey
{
    bool denoiserAvailable()
    {
#ifdef TRACEY_HAS_OIDN
        return true;
#else
        return false;
#endif
    }

    bool denoiseImage(int width, int height,
                      const float *colorRGBA,
                      const float *albedoRGBA,
                      const float *normalRGBA,
                      float *outRGBA,
                      std::string *error,
                      int maxThreads)
    {
#ifndef TRACEY_HAS_OIDN
        (void)width; (void)height; (void)colorRGBA;
        (void)albedoRGBA; (void)normalRGBA; (void)outRGBA; (void)maxThreads;
        if (error) *error = "denoiser not built (configure with -DTRACEY_WITH_OIDN=ON)";
        return false;
#else
        if (width <= 0 || height <= 0 || !colorRGBA || !outRGBA)
        {
            if (error) *error = "denoiseImage: invalid arguments";
            return false;
        }

        // Our readback buffers are RGBA32F — feed OIDN FLOAT3 with a 16-byte
        // pixel stride so it reads/writes RGB and skips the alpha lane.
        const size_t kPixelStride = 4 * sizeof(float);

        // CPU device: it can denoise directly from shared host pointers
        // (oidnSetSharedFilterImage). The GPU/Metal device would require
        // device-allocated OIDNBuffers + copies — a future optimisation for an
        // interactive preview, not needed for offline export.
        OIDNDevice device = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
        if (!device)
        {
            if (error) *error = "OIDN: failed to create CPU device";
            return false;
        }
        // Cap the worker pool for interactive callers (0 = OIDN default = all
        // cores, which starves the editor's tick/raster threads mid-navigation).
        if (maxThreads > 0)
            oidnSetDeviceInt(device, "numThreads", maxThreads);
        oidnCommitDevice(device);
        {
            const char *msg = nullptr;
            if (oidnGetDeviceError(device, &msg) != OIDN_ERROR_NONE)
            {
                if (error) *error = msg ? msg : "OIDN: device commit failed";
                oidnReleaseDevice(device);
                return false;
            }
        }

        OIDNFilter filter = oidnNewFilter(device, "RT");  // ray-traced beauty
        if (!filter)
        {
            if (error) *error = "OIDN: failed to create RT filter";
            oidnReleaseDevice(device);
            return false;
        }
        oidnSetSharedFilterImage(filter, "color", const_cast<float *>(colorRGBA),
                                 OIDN_FORMAT_FLOAT3, static_cast<size_t>(width),
                                 static_cast<size_t>(height), 0, kPixelStride, 0);
        if (albedoRGBA)
            oidnSetSharedFilterImage(filter, "albedo", const_cast<float *>(albedoRGBA),
                                     OIDN_FORMAT_FLOAT3, static_cast<size_t>(width),
                                     static_cast<size_t>(height), 0, kPixelStride, 0);
        if (normalRGBA)
            oidnSetSharedFilterImage(filter, "normal", const_cast<float *>(normalRGBA),
                                     OIDN_FORMAT_FLOAT3, static_cast<size_t>(width),
                                     static_cast<size_t>(height), 0, kPixelStride, 0);
        oidnSetSharedFilterImage(filter, "output", outRGBA, OIDN_FORMAT_FLOAT3,
                                 static_cast<size_t>(width), static_cast<size_t>(height),
                                 0, kPixelStride, 0);
        oidnSetFilterBool(filter, "hdr", true);  // beauty is linear HDR
        oidnCommitFilter(filter);
        oidnExecuteFilter(filter);

        const char *msg = nullptr;
        const bool ok = (oidnGetDeviceError(device, &msg) == OIDN_ERROR_NONE);
        if (!ok && error) *error = msg ? msg : "OIDN denoise failed";

        oidnReleaseFilter(filter);
        oidnReleaseDevice(device);
        return ok;
#endif
    }
} // namespace tracey
