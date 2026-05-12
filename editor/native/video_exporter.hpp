#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace tracey_editor {

// Thin AVFoundation wrapper that lets the editor write a sequence of RGBA8
// frames to an .mov file. macOS-only; lives behind __APPLE__. The render loop
// owns one of these for the duration of an export run.
class VideoExporter {
public:
    enum class Codec {
        H264,
        ProRes422,
    };

    VideoExporter();
    ~VideoExporter();

    VideoExporter(const VideoExporter&) = delete;
    VideoExporter& operator=(const VideoExporter&) = delete;

    // Open the writer at `path`. Frame size is fixed for the lifetime of the
    // run; the timescale is set so a frame index lands on (frame / fps) seconds.
    // Returns false on any AVFoundation error; call `last_error()` for details.
    bool begin(const std::string& path,
               uint32_t width,
               uint32_t height,
               uint32_t fps,
               Codec codec);

    // Append one RGBA8 frame at frame_index. Channels are swapped to BGRA in
    // the destination CVPixelBuffer (AVAssetWriter's preferred layout).
    bool append_frame(const uint8_t* rgba_pixels, uint32_t frame_index);

    // Cleanly close the writer. If `cancel` is true and no frames were appended
    // the on-disk file is removed instead of finalised.
    bool finish(bool cancel);

    const std::string& last_error() const { return m_last_error; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_last_error;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

}  // namespace tracey_editor
