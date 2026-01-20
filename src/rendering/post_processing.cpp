#include "post_processing.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace tracey
{
    void PostProcessing::toneMap(const float *hdrData, uint8_t *ldrData,
                                 uint32_t width, uint32_t height,
                                 const ToneMapSettings &settings)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                size_t pixelIndex = (y * width + x) * 4;

                // Read HDR values
                float r = hdrData[pixelIndex + 0];
                float g = hdrData[pixelIndex + 1];
                float b = hdrData[pixelIndex + 2];
                float a = hdrData[pixelIndex + 3];

                // Apply exposure
                r *= settings.exposure;
                g *= settings.exposure;
                b *= settings.exposure;

                // Apply tone mapping operator
                switch (settings.toneMapOperator)
                {
                case ToneMapSettings::Operator::ACES:
                {
                    // ACES filmic tone mapping
                    auto aces = [](float x) -> float
                    {
                        const float a = 2.51f;
                        const float b = 0.03f;
                        const float c = 2.43f;
                        const float d = 0.59f;
                        const float e = 0.14f;
                        return std::max(0.0f, std::min(1.0f, (x * (a * x + b)) / (x * (c * x + d) + e)));
                    };
                    r = aces(r);
                    g = aces(g);
                    b = aces(b);
                    break;
                }
                case ToneMapSettings::Operator::Reinhard:
                {
                    // Simple Reinhard tone mapping
                    r = r / (1.0f + r);
                    g = g / (1.0f + g);
                    b = b / (1.0f + b);
                    break;
                }
                case ToneMapSettings::Operator::Uncharted2:
                {
                    // Uncharted 2 tone mapping
                    auto uncharted2 = [](float x) -> float
                    {
                        const float A = 0.15f;
                        const float B = 0.50f;
                        const float C = 0.10f;
                        const float D = 0.20f;
                        const float E = 0.02f;
                        const float F = 0.30f;
                        return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
                    };
                    const float W = 11.2f;
                    r = uncharted2(r * 2.0f) / uncharted2(W);
                    g = uncharted2(g * 2.0f) / uncharted2(W);
                    b = uncharted2(b * 2.0f) / uncharted2(W);
                    break;
                }
                case ToneMapSettings::Operator::None:
                    // Linear - just clamp
                    r = std::max(0.0f, std::min(1.0f, r));
                    g = std::max(0.0f, std::min(1.0f, g));
                    b = std::max(0.0f, std::min(1.0f, b));
                    break;
                }

                // Gamma correction
                r = std::pow(r, 1.0f / settings.gamma);
                g = std::pow(g, 1.0f / settings.gamma);
                b = std::pow(b, 1.0f / settings.gamma);

                // Convert to 8-bit
                ldrData[pixelIndex + 0] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, r)) * 255.0f + 0.5f);
                ldrData[pixelIndex + 1] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, g)) * 255.0f + 0.5f);
                ldrData[pixelIndex + 2] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, b)) * 255.0f + 0.5f);
                ldrData[pixelIndex + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f); // Keep alpha as-is
            }
        }
    }

    void PostProcessing::savePPM(const std::string &filename, const uint8_t *data,
                                 uint32_t width, uint32_t height)
    {
        std::ofstream outFile(filename, std::ios::binary);
        if (!outFile.is_open())
        {
            throw std::runtime_error("Failed to open output file: " + filename);
        }

        // Write PPM header (P6 format)
        outFile << "P6\n"
                << width << " " << height << "\n255\n";

        // Write RGB data (skip alpha channel if present)
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                size_t pixelIndex = (y * width + x) * 4;
                outFile << data[pixelIndex + 0]     // R
                        << data[pixelIndex + 1]     // G
                        << data[pixelIndex + 2];    // B (skip alpha)
            }
        }

        outFile.close();
    }
} // namespace tracey
