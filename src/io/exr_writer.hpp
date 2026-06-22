#pragma once
#include <string>
#include <utility>
#include <vector>

namespace tracey
{
    // One named image layer fed to writeMultiLayerExr. `data` is tightly packed,
    // interleaved, width*height*channels floats (linear). Channel naming in the
    // file follows the comp convention:
    //   - name == ""  → the default beauty layer: "R","G","B" (+"A" if 4 ch)
    //   - 1 channel    → the bare `name` (e.g. "Z" for depth, "id")
    //   - 3/4 channels → "name.R","name.G","name.B"(,"name.A")
    struct ExrLayer
    {
        std::string name;
        int channels = 3;        // 1, 3, or 4
        const float *data = nullptr;
    };

    // Write a single-part, multi-channel OpenEXR (32-bit float, uncompressed-
    // or zip-compressed) gathering every layer into one file — the form Nuke /
    // usdview read AOVs from. Returns true on success; on failure returns false
    // and fills `error` when non-null. Backed by vendored tinyexr.
    bool writeMultiLayerExr(const std::string &path, int width, int height,
                            const std::vector<ExrLayer> &layers,
                            std::string *error = nullptr);

    // Read every channel of a single-part EXR into (channel-name → plane) pairs,
    // each `width*height` floats (row-major). Channel names are the full EXR
    // names ("R", "albedo.R", "Z", …). Returns false + fills `error` on failure.
    // Keeps tinyexr private to this TU; used by tests and downstream readers.
    bool readMultiLayerExr(const std::string &path, int *width, int *height,
                           std::vector<std::pair<std::string, std::vector<float>>> &channels,
                           std::string *error = nullptr);
} // namespace tracey
