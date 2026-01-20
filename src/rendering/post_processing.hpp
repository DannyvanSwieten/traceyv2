#pragma once

#include <cstdint>
#include <string>

namespace tracey
{
    /// Tone mapping settings for HDR to LDR conversion
    struct ToneMapSettings
    {
        float exposure = 0.2f;
        float gamma = 2.2f;

        enum class Operator
        {
            ACES,      // ACES filmic tone mapping (default)
            Reinhard,  // Simple Reinhard
            Uncharted2, // Uncharted 2 tone mapping
            None       // Linear (no tone mapping)
        };

        Operator toneMapOperator = Operator::ACES;
    };

    /// Utility class for post-processing operations
    class PostProcessing
    {
    public:
        /// Convert HDR float image to LDR 8-bit with tone mapping
        /// @param hdrData Input HDR RGBA float data
        /// @param ldrData Output LDR RGBA uint8_t data (must be pre-allocated)
        /// @param width Image width in pixels
        /// @param height Image height in pixels
        /// @param settings Tone mapping settings
        static void toneMap(const float *hdrData, uint8_t *ldrData,
                           uint32_t width, uint32_t height,
                           const ToneMapSettings &settings);

        /// Save image to PPM file (P6 format, RGB only)
        /// @param filename Output file path
        /// @param data RGB or RGBA uint8_t data (alpha channel ignored if present)
        /// @param width Image width in pixels
        /// @param height Image height in pixels
        static void savePPM(const std::string &filename, const uint8_t *data,
                           uint32_t width, uint32_t height);
    };
} // namespace tracey
