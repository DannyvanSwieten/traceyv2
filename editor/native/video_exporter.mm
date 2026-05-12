#include "video_exporter.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <cstring>

namespace tracey_editor {

struct VideoExporter::Impl {
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* input = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
    int32_t timescale = 30;  // overwritten in begin()
    bool any_frame_appended = false;
    NSString* path = nil;
};

VideoExporter::VideoExporter() : m_impl(std::make_unique<Impl>()) {}
VideoExporter::~VideoExporter() {
    // Best-effort cleanup if begin() succeeded but finish() never ran.
    if (m_impl && m_impl->writer && m_impl->writer.status == AVAssetWriterStatusWriting) {
        finish(true);
    }
}

bool VideoExporter::begin(const std::string& path,
                          uint32_t width,
                          uint32_t height,
                          uint32_t fps,
                          Codec codec) {
    m_last_error.clear();
    if (width == 0 || height == 0 || fps == 0) {
        m_last_error = "invalid dimensions or fps";
        return false;
    }
    m_width = width;
    m_height = height;

    @autoreleasepool {
        NSString* nsPath = [NSString stringWithUTF8String:path.c_str()];
        // AVAssetWriter refuses to overwrite. Remove any pre-existing file at
        // the destination so a re-export to the same path "just works".
        [[NSFileManager defaultManager] removeItemAtPath:nsPath error:nil];
        NSURL* url = [NSURL fileURLWithPath:nsPath];

        NSError* err = nil;
        AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url
                                                          fileType:AVFileTypeQuickTimeMovie
                                                             error:&err];
        if (!writer) {
            m_last_error = err ? err.localizedDescription.UTF8String : "AVAssetWriter init failed";
            return false;
        }

        NSString* codecKey = (codec == Codec::ProRes422) ? AVVideoCodecTypeAppleProRes422
                                                         : AVVideoCodecTypeH264;
        NSDictionary* settings = @{
            AVVideoCodecKey: codecKey,
            AVVideoWidthKey: @(width),
            AVVideoHeightKey: @(height),
        };
        AVAssetWriterInput* input = [AVAssetWriterInput
            assetWriterInputWithMediaType:AVMediaTypeVideo
                           outputSettings:settings];
        input.expectsMediaDataInRealTime = NO;

        // BGRA pixel buffers — what AVAssetWriter wants and what our channel-
        // swap copy produces.
        NSDictionary* sourcePixelAttrs = @{
            (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (NSString*)kCVPixelBufferWidthKey: @(width),
            (NSString*)kCVPixelBufferHeightKey: @(height),
        };
        AVAssetWriterInputPixelBufferAdaptor* adaptor =
            [AVAssetWriterInputPixelBufferAdaptor
                assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                           sourcePixelBufferAttributes:sourcePixelAttrs];

        if (![writer canAddInput:input]) {
            m_last_error = "writer rejected input";
            return false;
        }
        [writer addInput:input];

        if (![writer startWriting]) {
            m_last_error = writer.error ? writer.error.localizedDescription.UTF8String
                                        : "startWriting failed";
            return false;
        }
        [writer startSessionAtSourceTime:kCMTimeZero];

        m_impl->writer = writer;
        m_impl->input = input;
        m_impl->adaptor = adaptor;
        m_impl->timescale = static_cast<int32_t>(fps);
        m_impl->any_frame_appended = false;
        m_impl->path = nsPath;
    }
    return true;
}

bool VideoExporter::append_frame(const uint8_t* rgba_pixels, uint32_t frame_index) {
    if (!m_impl->writer || !m_impl->adaptor) {
        m_last_error = "exporter not begun";
        return false;
    }
    @autoreleasepool {
        // Pull a buffer from the adaptor's pool when available — recycling
        // CVPixelBuffers is much cheaper than allocating per-frame.
        CVPixelBufferRef pb = nullptr;
        CVPixelBufferPoolRef pool = m_impl->adaptor.pixelBufferPool;
        if (pool) {
            CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb);
        }
        if (!pb) {
            CVPixelBufferCreate(kCFAllocatorDefault, m_width, m_height,
                                kCVPixelFormatType_32BGRA, nullptr, &pb);
        }
        if (!pb) {
            m_last_error = "CVPixelBufferCreate failed";
            return false;
        }

        CVPixelBufferLockBaseAddress(pb, 0);
        uint8_t* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pb));
        const size_t dstStride = CVPixelBufferGetBytesPerRow(pb);
        const size_t srcStride = static_cast<size_t>(m_width) * 4;

        for (uint32_t y = 0; y < m_height; ++y) {
            const uint8_t* srcRow = rgba_pixels + y * srcStride;
            uint8_t* dstRow = dst + y * dstStride;
            for (uint32_t x = 0; x < m_width; ++x) {
                // RGBA → BGRA channel swap.
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
            }
        }

        CVPixelBufferUnlockBaseAddress(pb, 0);

        // Spin (briefly) waiting for the encoder to drain. Encoders can stall
        // briefly under load; we don't have a producer/consumer queue so this
        // serialises pull-pressure naturally.
        while (!m_impl->input.isReadyForMoreMediaData) {
            [NSThread sleepForTimeInterval:0.001];
        }

        CMTime pts = CMTimeMake(static_cast<int64_t>(frame_index), m_impl->timescale);
        BOOL ok = [m_impl->adaptor appendPixelBuffer:pb withPresentationTime:pts];
        CVPixelBufferRelease(pb);

        if (!ok) {
            NSError* e = m_impl->writer.error;
            m_last_error = e ? e.localizedDescription.UTF8String : "append failed";
            return false;
        }
        m_impl->any_frame_appended = true;
    }
    return true;
}

bool VideoExporter::finish(bool cancel) {
    if (!m_impl->writer) return true;
    @autoreleasepool {
        AVAssetWriter* writer = m_impl->writer;
        AVAssetWriterInput* input = m_impl->input;
        NSString* path = m_impl->path;
        const bool any = m_impl->any_frame_appended;

        m_impl->writer = nil;
        m_impl->input = nil;
        m_impl->adaptor = nil;
        m_impl->path = nil;

        [input markAsFinished];

        if (cancel && !any) {
            [writer cancelWriting];
            if (path) {
                [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
            }
            return true;
        }

        // finishWritingWithCompletionHandler: is async; block here so the
        // caller can rely on the file being closed before broadcasting "done".
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        if (writer.status != AVAssetWriterStatusCompleted) {
            NSError* e = writer.error;
            m_last_error = e ? e.localizedDescription.UTF8String
                             : "writer did not complete";
            return false;
        }
    }
    return true;
}

}  // namespace tracey_editor
