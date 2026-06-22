// Smoke test for the multi-layer EXR writer/reader (io/exr_writer).
//
// Writes a small EXR carrying a beauty layer + an albedo layer (3ch) + a depth
// layer (1ch named "Z"), reads it back, and asserts the channel planes survive
// the round-trip (deinterleave + alphabetical channel ordering are correct).
//
// Exit 0 on success. Depends only on `tracey`.

#include "io/exr_writer.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace tracey;

namespace {
int failures = 0;
void check(bool ok, const char *what)
{
    if (ok) std::printf("  ok   %s\n", what);
    else { ++failures; std::printf("  FAIL %s\n", what); }
}
bool approx(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }
}

int main()
{
    std::printf("[exr_roundtrip_smoke]\n");

    const int W = 4, H = 4;
    const size_t pix = static_cast<size_t>(W) * H;

    // Beauty: R/G/B = a per-pixel ramp so we can detect transposes/shuffles.
    std::vector<float> beauty(pix * 3);
    for (size_t p = 0; p < pix; ++p)
    {
        beauty[p * 3 + 0] = static_cast<float>(p) * 0.1f;
        beauty[p * 3 + 1] = static_cast<float>(p) * 0.01f;
        beauty[p * 3 + 2] = static_cast<float>(p) * 0.001f;
    }
    // Albedo: constant colour.
    std::vector<float> albedo(pix * 3);
    for (size_t p = 0; p < pix; ++p)
    {
        albedo[p * 3 + 0] = 0.5f;
        albedo[p * 3 + 1] = 0.25f;
        albedo[p * 3 + 2] = 0.75f;
    }
    // Depth (single channel "Z").
    std::vector<float> depth(pix);
    for (size_t p = 0; p < pix; ++p) depth[p] = static_cast<float>(p) + 1.0f;

    std::vector<ExrLayer> layers = {
        {"", 3, beauty.data()},
        {"albedo", 3, albedo.data()},
        {"Z", 1, depth.data()},
    };

    const std::string path =
        (std::filesystem::temp_directory_path() / "tracey_exr_roundtrip.exr").string();

    std::string err;
    check(writeMultiLayerExr(path, W, H, layers, &err),
          ("write multi-layer EXR" + (err.empty() ? "" : " (" + err + ")")).c_str());

    {
        std::error_code ec;
        check(std::filesystem::file_size(path, ec) > 0 && !ec, "EXR file non-empty");
    }

    int rw = 0, rh = 0;
    std::vector<std::pair<std::string, std::vector<float>>> chans;
    err.clear();
    check(readMultiLayerExr(path, &rw, &rh, chans, &err),
          ("read EXR back" + (err.empty() ? "" : " (" + err + ")")).c_str());
    check(rw == W && rh == H, "dimensions preserved");

    // Index channels by name.
    std::map<std::string, const std::vector<float> *> byName;
    for (const auto &c : chans) byName[c.first] = &c.second;

    check(byName.count("R") && byName.count("G") && byName.count("B"), "beauty RGB present");
    check(byName.count("albedo.R") && byName.count("albedo.G") && byName.count("albedo.B"),
          "albedo layer present");
    check(byName.count("Z") == 1, "depth channel present");

    if (byName.count("R") && byName.count("B"))
    {
        bool rOk = true, bOk = true;
        for (size_t p = 0; p < pix; ++p)
        {
            rOk = rOk && approx((*byName["R"])[p], static_cast<float>(p) * 0.1f);
            bOk = bOk && approx((*byName["B"])[p], static_cast<float>(p) * 0.001f);
        }
        check(rOk, "beauty R plane round-trips");
        check(bOk, "beauty B plane round-trips");
    }
    if (byName.count("albedo.G"))
    {
        bool ok = true;
        for (size_t p = 0; p < pix; ++p) ok = ok && approx((*byName["albedo.G"])[p], 0.25f);
        check(ok, "albedo.G plane round-trips");
    }
    if (byName.count("Z"))
    {
        bool ok = true;
        for (size_t p = 0; p < pix; ++p) ok = ok && approx((*byName["Z"])[p], static_cast<float>(p) + 1.0f);
        check(ok, "depth Z plane round-trips");
    }

    std::printf(failures == 0 ? "[exr_roundtrip_smoke] all checks passed\n"
                              : "[exr_roundtrip_smoke] %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
