#pragma once

// A video's frames, streamed one at a time through ffmpeg (spec 2.4): the
// video is never decoded whole. Pending trim and crop on the handle are
// applied as the frames are decoded.
//
// ffmpeg writes raw frames into a pipe that is read with a blocking read of
// exactly one frame at a time, so ffmpeg waits while the reader works and at
// most one frame (plus the pipe's buffer) is in memory, whatever thread or
// event loop the reader runs on. Destroying the reader stops ffmpeg.

#include <optional>

#include <QString>

#include <vips/vips8>

#include "engine/media.h"
#include "engine/result.h"

namespace fugue {

class VideoFrames {
public:
    // Starts ffmpeg on a video handle that has a file and a size (a probed
    // one, or one read from JSON).
    static Result<VideoFrames> open(const MediaHandle &video);

    VideoFrames(VideoFrames &&other) noexcept;
    VideoFrames &operator=(VideoFrames &&other) noexcept;
    VideoFrames(const VideoFrames &) = delete;
    VideoFrames &operator=(const VideoFrames &) = delete;
    ~VideoFrames();

    // The size of every frame, with a pending crop applied.
    int width() const { return m_width; }
    int height() const { return m_height; }

    // The next frame as an 8-bit sRGB libvips image in memory, or nothing at
    // the end; failed() then tells a clean end from a failure.
    std::optional<vips::VImage> next();

    bool failed() const { return !m_error.code.isEmpty(); }
    Error error() const { return m_error; }

private:
    VideoFrames() = default;
    void finish();
    void stop();

    int m_width = 0;
    int m_height = 0;
    int m_pid = -1;
    int m_fd = -1;
    QString m_stderrPath;
    Error m_error;
};

} // namespace fugue
