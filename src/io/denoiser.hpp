#pragma once
#include <string>

namespace tracey
{
    // True when the build links a working denoiser (Intel OIDN, gated by the
    // TRACEY_WITH_OIDN CMake option). When false, denoiseImage() is a no-op
    // that reports failure — callers fall back to the raw image.
    bool denoiserAvailable();

    // Denoise an HDR *linear* beauty image, optionally guided by albedo +
    // normal AOVs (the R1 layers). All buffers are width*height RGBA32F,
    // interleaved (stride 4 floats); only RGB is read/written — alpha is left
    // untouched. `albedoRGBA` / `normalRGBA` may be null (no guide). `outRGBA`
    // may alias `colorRGBA` for in-place denoising. Returns false + fills
    // `error` on failure (including when no denoiser is built in).
    // `maxThreads` caps OIDN's CPU worker pool (0 = OIDN default, all cores).
    // Interactive preview denoises pass a capped count so the UI / render-tick /
    // raster threads keep breathing while OIDN runs; offline export passes 0.
    bool denoiseImage(int width, int height,
                      const float *colorRGBA,
                      const float *albedoRGBA,
                      const float *normalRGBA,
                      float *outRGBA,
                      std::string *error = nullptr,
                      int maxThreads = 0);
} // namespace tracey
