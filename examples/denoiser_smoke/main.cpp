// Smoke test for the OIDN denoiser wrapper (io/denoiser).
//
// Builds a constant-signal HDR image corrupted with per-pixel noise, plus
// matching albedo/normal guide AOVs, runs denoiseImage(), and asserts the
// output's variance collapses (noise removed) while the mean is preserved
// (signal intact). When the build has no denoiser (TRACEY_WITH_OIDN=OFF) the
// test reports "skipped" and still exits 0.
//
// Exit 0 on success. Depends only on `tracey`.

#include "io/denoiser.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace tracey;

namespace {
int failures = 0;
void check(bool ok, const char *what)
{
    if (ok) std::printf("  ok   %s\n", what);
    else { ++failures; std::printf("  FAIL %s\n", what); }
}
// Variance of the RGB channels across all pixels of an RGBA32F buffer.
double rgbVariance(const std::vector<float> &rgba, size_t pixels)
{
    double mean = 0.0;
    for (size_t p = 0; p < pixels; ++p)
        for (int c = 0; c < 3; ++c) mean += rgba[p * 4 + c];
    mean /= double(pixels * 3);
    double var = 0.0;
    for (size_t p = 0; p < pixels; ++p)
        for (int c = 0; c < 3; ++c)
        {
            const double d = rgba[p * 4 + c] - mean;
            var += d * d;
        }
    return var / double(pixels * 3);
}
double rgbMean(const std::vector<float> &rgba, size_t pixels)
{
    double mean = 0.0;
    for (size_t p = 0; p < pixels; ++p)
        for (int c = 0; c < 3; ++c) mean += rgba[p * 4 + c];
    return mean / double(pixels * 3);
}
}

int main()
{
    std::printf("[denoiser_smoke]\n");

    if (!denoiserAvailable())
    {
        std::printf("  skip  denoiser not built (TRACEY_WITH_OIDN=OFF)\n");
        std::printf("[denoiser_smoke] skipped (no denoiser)\n");
        return 0;
    }

    const int W = 128, H = 128;
    const size_t pixels = static_cast<size_t>(W) * H;

    const float signal = 0.5f;
    std::vector<float> noisy(pixels * 4), albedo(pixels * 4), normal(pixels * 4),
        out(pixels * 4, 0.0f);

    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
    for (size_t p = 0; p < pixels; ++p)
    {
        for (int c = 0; c < 3; ++c) noisy[p * 4 + c] = signal + noise(rng);
        noisy[p * 4 + 3] = 1.0f;
        // Flat albedo + facing normal as guides.
        albedo[p * 4 + 0] = signal; albedo[p * 4 + 1] = signal; albedo[p * 4 + 2] = signal;
        albedo[p * 4 + 3] = 1.0f;
        normal[p * 4 + 0] = 0.0f; normal[p * 4 + 1] = 0.0f; normal[p * 4 + 2] = 1.0f;
        normal[p * 4 + 3] = 0.0f;
    }

    const double varBefore = rgbVariance(noisy, pixels);
    const double meanBefore = rgbMean(noisy, pixels);

    std::string err;
    const bool ok = denoiseImage(W, H, noisy.data(), albedo.data(), normal.data(),
                                 out.data(), &err);
    check(ok, ("denoiseImage runs" + (err.empty() ? "" : " (" + err + ")")).c_str());
    if (!ok)
    {
        std::printf("[denoiser_smoke] %d failure(s)\n", failures);
        return 1;
    }

    const double varAfter = rgbVariance(out, pixels);
    const double meanAfter = rgbMean(out, pixels);

    std::printf("  var %.5f -> %.5f, mean %.4f -> %.4f\n",
                varBefore, varAfter, meanBefore, meanAfter);
    check(varAfter < varBefore * 0.5, "noise variance at least halved");
    check(std::fabs(meanAfter - meanBefore) < 0.05, "mean signal preserved");

    std::printf(failures == 0 ? "[denoiser_smoke] all checks passed\n"
                              : "[denoiser_smoke] %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
