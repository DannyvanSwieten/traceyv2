// Tiny EXR channel inspector: reads a multi-layer EXR and prints per-channel
// min / max / mean / nonzero-fraction so we can see which AOV layers carry data.
//   exr_inspect <file.exr>

#include "io/exr_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace tracey;

int main(int argc, char **argv)
{
    if (argc < 2) { std::printf("usage: exr_inspect <file.exr>\n"); return 2; }
    int w = 0, h = 0;
    std::vector<std::pair<std::string, std::vector<float>>> chans;
    std::string err;
    if (!readMultiLayerExr(argv[1], &w, &h, chans, &err))
    {
        std::printf("read failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s  %dx%d  %zu channels\n", argv[1], w, h, chans.size());
    std::printf("  %-14s %12s %12s %12s %8s\n", "channel", "min", "max", "mean", "nonzero%");
    for (const auto &c : chans)
    {
        const auto &d = c.second;
        if (d.empty()) continue;
        float mn = d[0], mx = d[0];
        double sum = 0.0;
        size_t nz = 0;
        for (float v : d)
        {
            mn = std::min(mn, v);
            mx = std::max(mx, v);
            sum += v;
            if (v != 0.0f) ++nz;
        }
        std::printf("  %-14s %12.5f %12.5f %12.5f %7.1f%%\n",
                    c.first.c_str(), mn, mx, sum / d.size(),
                    100.0 * double(nz) / double(d.size()));
    }
    return 0;
}
