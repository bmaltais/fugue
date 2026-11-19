#pragma once

// Media handles (spec 2.4): the value an image, video or audio port carries.
//
// A handle is an immutable value; every change returns a new handle. It stands
// for a file, a decoded libvips image (images only), or both:
//   - image: a file, a decoded image, or both. Pixel nodes ask for image(),
//     command and provider nodes for a file.
//   - video: always a file, never decoded whole. Trim and crop are pending
//     edits applied only when frames or a plain file are needed.
//   - audio: a file.
//
// The metadata fields (width, fps, duration, ...) always describe the file at
// path(), as read from it; pending edits are separate, and the effective*()
// accessors give the size and length once they are applied. A video handle's
// identity is therefore its hash plus its edits, not its hash alone.

#include <optional>

#include <QJsonObject>
#include <QString>

#include <vips/vips8>

#include "engine/result.h"

namespace fugue {

enum class MediaType { Image, Video, Audio };

// "image", "video", "audio".
QString mediaTypeName(MediaType type);

// Seconds on the file's timeline, start < end.
struct Trim {
    double start = 0;
    double end = 0;
};

// Pixels in the file's frame.
struct Crop {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct VideoEdits {
    std::optional<Trim> trim;
    std::optional<Crop> crop;

    bool isEmpty() const { return !trim && !crop; }
};

class MediaHandle {
public:
    MediaHandle() = default;

    // The serialized form of 2.4. fromJson needs an absolute path and a
    // sha256 hash, and fails with invalid_document otherwise. toJson writes
    // only the fields that belong to the type, and edits only when there are
    // some.
    static Result<MediaHandle> fromJson(const QJsonObject &object);
    QJsonObject toJson() const;

    MediaType type() const { return m_type; }
    QString path() const { return m_path; }
    QString hash() const { return m_hash; }
    QString format() const { return m_format; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    int channels() const { return m_channels; }
    double fps() const { return m_fps; }
    qint64 frames() const { return m_frames; }
    double duration() const { return m_duration; }
    bool hasAudio() const { return m_hasAudio; }
    int sampleRate() const { return m_sampleRate; }
    VideoEdits edits() const { return m_edits; }

    // Size and length with the pending edits applied (videos); otherwise the
    // same as width(), height() and duration().
    int effectiveWidth() const;
    int effectiveHeight() const;
    double effectiveDuration() const;

    // Pending edits, videos only. Both are relative to what the handle shows
    // now, so a trim of a trimmed video is a trim within the first trim.
    // Out-of-range values fail with value_out_of_range.
    Result<MediaHandle> trimmed(double start, double end) const;
    Result<MediaHandle> cropped(int x, int y, int width, int height) const;

    // The same handle, for the same bytes found at another path. This is how
    // the result cache (#9) re-points a handle after moving its file into the
    // store: the hash, metadata and edits stay as they are.
    MediaHandle atPath(const QString &path) const;

private:
    MediaType m_type = MediaType::Image;
    QString m_path;
    QString m_hash;
    QString m_format;
    int m_width = 0;
    int m_height = 0;
    int m_channels = 0;
    double m_fps = 0;
    qint64 m_frames = 0;
    double m_duration = 0;
    bool m_hasAudio = false;
    int m_sampleRate = 0;
    VideoEdits m_edits;
};

} // namespace fugue
