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
// path(), as read from it; an image is upright everywhere: an EXIF orientation is
// applied, so width and height are the size it is seen at, and image() is turned
// to match (a file sent on as it is keeps its tag). Pending edits are separate, and the effective*()
// accessors give the size and length once they are applied. A video handle's
// identity is therefore its hash plus its edits, not its hash alone.

#include <optional>

#include <QJsonObject>
#include <QString>

#include <vips/vips8>

#include "engine/result.h"

namespace fugue {

class FileHashes;

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

    // A decoded image held in memory, with no file yet. A lazy libvips
    // pipeline is evaluated into memory here (spike 03: materialise every
    // node's output; do not chain lazy images between nodes).
    static Result<MediaHandle> fromImage(const vips::VImage &image);

    // The handle a port of `type` carries for the file at `path`: metadata
    // read from the file (the libvips header for images, ffprobe for video
    // and audio; nothing is decoded) and the content hash from `hashes`.
    // Fails with file_not_found or media_unreadable.
    static Result<MediaHandle> probe(MediaType type, const QString &path, FileHashes &hashes);

    // The serialized form of 2.4. fromJson needs an absolute path and a
    // sha256 hash, and fails with invalid_document otherwise. toJson writes
    // only the fields that belong to the type, and edits only when there are
    // some. An image held only in memory has no path or hash yet, so call
    // saved() before serializing a node's output.
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

    bool hasFile() const { return !m_path.isEmpty(); }
    bool hasImage() const { return m_image.has_value(); }

    // The decoded image, upright: the one held, or else the file opened through
    // libvips, which decodes pixels only as they are read. Images only.
    Result<vips::VImage> image() const;

    // A handle that has a file and a hash, for recording a node's output in
    // the run folder and the result cache. An image held only in memory is
    // written to <dir>/<name>.png, losslessly: 8-bit pixels as 8-bit, 16-bit
    // as 16-bit, and float pixels (0 to 1) as 16-bit; the decoded image is
    // kept too. A handle that already has a file is returned as it is
    // (nothing is encoded again), and pending video edits stay pending.
    Result<MediaHandle> saved(const QString &dir, const QString &name, FileHashes &hashes) const;

    // Like saved(), but with nothing pending: a video's edits are applied
    // into a new file, <dir>/<name>.mkv. This is what a command or provider
    // node receives (spec 7.2), so `path` alone is the whole value. The same
    // edit of the same video into the same file is encoded once per process:
    // a later call gets the file the first one wrote.
    Result<MediaHandle> plainFile(const QString &dir, const QString &name, FileHashes &hashes) const;

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
    static Result<MediaHandle> probeImage(const QString &path, FileHashes &hashes);
    static Result<MediaHandle> probeWithFfprobe(MediaType type, const QString &path, FileHashes &hashes);
    Result<MediaHandle> savedImage(const QString &dir, const QString &name, FileHashes &hashes) const;
    Result<MediaHandle> withEditsApplied(const QString &dir, const QString &name, FileHashes &hashes) const;

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
    std::optional<vips::VImage> m_image;
};

} // namespace fugue
