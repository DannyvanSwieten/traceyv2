// The single translation unit that compiles tinyexr. miniz (the deflate impl
// tinyexr links for ZIP compression) is built separately from deps/tinyexr/miniz.c;
// tinyexr only #include <miniz.h> here for declarations.
#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>

#include "exr_writer.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace tracey
{
    bool writeMultiLayerExr(const std::string &path, int width, int height,
                            const std::vector<ExrLayer> &layers, std::string *error)
    {
        if (width <= 0 || height <= 0 || layers.empty())
        {
            if (error) *error = "writeMultiLayerExr: invalid dimensions or no layers";
            return false;
        }
        const size_t pix = static_cast<size_t>(width) * static_cast<size_t>(height);

        // Flatten every layer into individual single-channel float planes
        // (EXR stores planar, not interleaved).
        struct Chan
        {
            std::string name;
            std::vector<float> data;
        };
        std::vector<Chan> chans;
        chans.reserve(layers.size() * 4);

        for (const auto &L : layers)
        {
            if (!L.data || L.channels < 1 || L.channels > 4)
            {
                if (error) *error = "writeMultiLayerExr: layer '" + L.name + "' invalid";
                return false;
            }
            for (int c = 0; c < L.channels; ++c)
            {
                Chan ch;
                if (L.channels == 1)
                {
                    ch.name = L.name.empty() ? "Y" : L.name;
                }
                else
                {
                    const char *sfx = (c == 0) ? "R" : (c == 1) ? "G" : (c == 2) ? "B" : "A";
                    ch.name = L.name.empty() ? std::string(sfx) : (L.name + "." + sfx);
                }
                ch.data.resize(pix);
                for (size_t p = 0; p < pix; ++p)
                    ch.data[p] = L.data[p * static_cast<size_t>(L.channels) + c];
                chans.push_back(std::move(ch));
            }
        }

        // OpenEXR canonically stores channels in alphabetical order.
        std::sort(chans.begin(), chans.end(),
                  [](const Chan &a, const Chan &b) { return a.name < b.name; });

        const int n = static_cast<int>(chans.size());

        EXRHeader header;
        InitEXRHeader(&header);
        EXRImage image;
        InitEXRImage(&image);

        image.num_channels = n;
        image.width = width;
        image.height = height;
        std::vector<unsigned char *> imgPtrs(n);
        for (int c = 0; c < n; ++c)
            imgPtrs[c] = reinterpret_cast<unsigned char *>(chans[c].data.data());
        image.images = imgPtrs.data();

        std::vector<EXRChannelInfo> chInfos(n);
        std::vector<int> pixTypes(n), reqTypes(n);
        for (int c = 0; c < n; ++c)
        {
            std::memset(&chInfos[c], 0, sizeof(EXRChannelInfo));
            std::strncpy(chInfos[c].name, chans[c].name.c_str(), 255);
            chInfos[c].name[255] = '\0';
            pixTypes[c] = TINYEXR_PIXELTYPE_FLOAT;  // input data is float
            reqTypes[c] = TINYEXR_PIXELTYPE_FLOAT;  // store as 32-bit float
        }
        header.num_channels = n;
        header.channels = chInfos.data();
        header.pixel_types = pixTypes.data();
        header.requested_pixel_types = reqTypes.data();
        header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

        // All arrays are caller-owned (stack/vector) — do NOT Free* the header/
        // image; tinyexr only reads them.
        const char *err = nullptr;
        const int ret = SaveEXRImageToFile(&image, &header, path.c_str(), &err);
        if (ret != TINYEXR_SUCCESS)
        {
            if (error) *error = err ? err : "SaveEXRImageToFile failed";
            if (err) FreeEXRErrorMessage(err);
            return false;
        }
        return true;
    }

    bool readMultiLayerExr(const std::string &path, int *width, int *height,
                           std::vector<std::pair<std::string, std::vector<float>>> &channels,
                           std::string *error)
    {
        const char *err = nullptr;
        EXRVersion ver;
        if (ParseEXRVersionFromFile(&ver, path.c_str()) != TINYEXR_SUCCESS)
        {
            if (error) *error = "ParseEXRVersionFromFile failed for " + path;
            return false;
        }

        EXRHeader header;
        InitEXRHeader(&header);
        if (ParseEXRHeaderFromFile(&header, &ver, path.c_str(), &err) != TINYEXR_SUCCESS)
        {
            if (error) *error = err ? err : "ParseEXRHeaderFromFile failed";
            if (err) FreeEXRErrorMessage(err);
            return false;
        }
        // Force float decode regardless of stored pixel type.
        for (int c = 0; c < header.num_channels; ++c)
            header.requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;

        EXRImage image;
        InitEXRImage(&image);
        if (LoadEXRImageFromFile(&image, &header, path.c_str(), &err) != TINYEXR_SUCCESS)
        {
            if (error) *error = err ? err : "LoadEXRImageFromFile failed";
            if (err) FreeEXRErrorMessage(err);
            FreeEXRHeader(&header);
            return false;
        }

        if (width) *width = image.width;
        if (height) *height = image.height;
        const size_t pix = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);

        channels.clear();
        channels.reserve(image.num_channels);
        for (int c = 0; c < image.num_channels; ++c)
        {
            std::vector<float> plane(pix);
            std::memcpy(plane.data(), image.images[c], pix * sizeof(float));
            channels.emplace_back(header.channels[c].name, std::move(plane));
        }

        FreeEXRImage(&image);
        FreeEXRHeader(&header);
        return true;
    }
} // namespace tracey
