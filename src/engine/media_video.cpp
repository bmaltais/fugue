// Video and audio handles: probing with ffprobe and applying pending edits
// with ffmpeg.

#include "media.h"

#include <cmath>

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>

#include "engine/atomic_file.h"
#include "engine/ffmpeg.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"

namespace fugue {

namespace {

Error unreadable(const QString &path, MediaType type) {
    return makeError(QStringLiteral("media_unreadable"),
                     QStringLiteral("%1 is not %2 Fugue can read.")
                         .arg(path, type == MediaType::Video ? QStringLiteral("a video") : QStringLiteral("audio")));
}

// "25/1" is 25; "0/0" is 0.
double rational(const QString &text) {
    const QStringList parts = text.split(QLatin1Char('/'));
    if (parts.size() != 2)
        return text.toDouble();
    const double denominator = parts.at(1).toDouble();
    return denominator == 0 ? 0 : parts.at(0).toDouble() / denominator;
}

// ffprobe names a container by all its aliases, "mov,mp4,m4a,3gp,3g2,mj2":
// the one matching the file's extension, or else the first.
QString formatName(const QString &formatNames, const QString &path) {
    const QStringList names = formatNames.split(QLatin1Char(','), Qt::SkipEmptyParts);
    const QString extension = QFileInfo(path).suffix().toLower();
    if (names.contains(extension))
        return extension;
    return names.value(0);
}

// The display rotation in degrees, from the display matrix or an old tag.
int rotation(const QJsonObject &stream) {
    for (const QJsonValue &data : stream.value("side_data_list").toArray()) {
        const QJsonObject side = data.toObject();
        if (side.contains("rotation"))
            return side.value("rotation").toInt();
    }
    return stream.value("tags").toObject().value("rotate").toString().toInt();
}

QJsonObject firstStream(const QJsonArray &streams, const QString &codecType) {
    for (const QJsonValue &value : streams) {
        const QJsonObject stream = value.toObject();
        if (stream.value("codec_type").toString() != codecType)
            continue;
        // Cover art in an audio file is a one-picture "video" stream.
        if (stream.value("disposition").toObject().value("attached_pic").toInt() != 0)
            continue;
        return stream;
    }
    return {};
}

double seconds(const QJsonValue &value) {
    bool ok = false;
    const double number = value.toString().toDouble(&ok);
    return ok ? number : 0;
}

} // namespace

Result<MediaHandle> MediaHandle::probeWithFfprobe(MediaType type, const QString &path, FileHashes &hashes, const QString &shown, const QString &shownHash) {
    const QString &named = shown.isEmpty() ? path : shown;
    const Result<QByteArray> output =
        ffmpeg::run(QStringLiteral("ffprobe"),
                    {"-v", "error", "-print_format", "json", "-show_format", "-show_streams", path},
                    QStringLiteral("media_unreadable"));
    if (!output)
        return output.error().code == QLatin1String("internal") ? output.error() : unreadable(named, type);
    const QJsonObject probe = QJsonDocument::fromJson(*output).object();
    const QJsonObject format = probe.value("format").toObject();
    const QJsonArray streams = probe.value("streams").toArray();

    MediaHandle handle;
    handle.m_type = type;
    handle.m_path = named;
    handle.m_format = formatName(format.value("format_name").toString(), named);

    if (type == MediaType::Video) {
        const QJsonObject video = firstStream(streams, QStringLiteral("video"));
        if (video.isEmpty() || video.value("width").toInt() <= 0)
            return unreadable(named, type);
        // The size it is shown at: ffmpeg turns frames by the display
        // rotation when it decodes them, so frames have this size too.
        const bool turned = std::abs(rotation(video)) % 180 == 90;
        handle.m_width = video.value(turned ? "height" : "width").toInt();
        handle.m_height = video.value(turned ? "width" : "height").toInt();
        handle.m_fps = rational(video.value("avg_frame_rate").toString());
        if (handle.m_fps <= 0)
            handle.m_fps = rational(video.value("r_frame_rate").toString());
        handle.m_duration = seconds(format.value("duration"));
        if (handle.m_duration <= 0)
            handle.m_duration = seconds(video.value("duration"));
        handle.m_frames = video.value("nb_frames").toString().toLongLong();
        if (handle.m_frames <= 0) {
            // Matroska keeps no count. Counting packets reads the file but
            // decodes nothing.
            const Result<QByteArray> counted = ffmpeg::run(
                QStringLiteral("ffprobe"),
                {"-v", "error", "-select_streams", QStringLiteral("%1").arg(video.value("index").toInt()),
                 "-count_packets", "-show_entries", "stream=nb_read_packets", "-print_format", "json", path},
                QStringLiteral("media_unreadable"));
            if (counted) {
                const QJsonArray list = QJsonDocument::fromJson(*counted).object().value("streams").toArray();
                handle.m_frames = list.at(0).toObject().value("nb_read_packets").toString().toLongLong();
            }
        }
        handle.m_hasAudio = !firstStream(streams, QStringLiteral("audio")).isEmpty();
    } else {
        const QJsonObject audio = firstStream(streams, QStringLiteral("audio"));
        if (audio.isEmpty())
            return unreadable(named, type);
        handle.m_channels = audio.value("channels").toInt();
        handle.m_sampleRate = audio.value("sample_rate").toString().toInt();
        handle.m_duration = seconds(format.value("duration"));
        if (handle.m_duration <= 0)
            handle.m_duration = seconds(audio.value("duration"));
    }

    handle.m_hash = shown.isEmpty() ? hashes.hash(path) : shownHash;
    if (handle.m_hash.isEmpty())
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot read %1.").arg(named));
    return handle;
}

namespace {

// The edits this process has already applied, by output path: what went in (the source
// handle without its path, so its hash and its edits) and what came out. plainFile() is
// called once per node that takes the video, and two encodes of the same edit do not
// even give the same bytes, so the first one is kept and reused while its file is still
// the file that was written.
struct AppliedEdit {
    QJsonObject source;
    MediaHandle result;
};

QMutex appliedMutex;
QHash<QString, AppliedEdit> &appliedEdits() {
    static QHash<QString, AppliedEdit> edits;
    return edits;
}

QJsonObject withoutPath(QJsonObject handle) {
    handle.remove("path");
    return handle;
}

} // namespace

Result<MediaHandle> MediaHandle::withEditsApplied(const QString &dir, const QString &name, FileHashes &hashes) const {
    if (!QDir().mkpath(dir))
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot create %1.").arg(dir));
    const QString path = QDir(dir).filePath(name + QStringLiteral(".mkv"));
    const QJsonObject source = withoutPath(toJson());
    {
        const QMutexLocker lock(&appliedMutex);
        const auto found = appliedEdits().constFind(path);
        // The memo of file hashes notices a file that was replaced or removed since.
        if (found != appliedEdits().constEnd() && found->source == source && hashes.hash(path) == found->result.hash())
            return found->result;
    }
    // The same source and edits make the same file, so one already at the name is replaced.
    PartialFile partial(dir, name + QStringLiteral(".mkv"));

    // Re-encoded losslessly (FFV1 video, FLAC audio, in Matroska): trims must
    // be exact to the frame, which a stream copy is not, and a lossless copy
    // keeps the edit from costing quality.
    QStringList args{"-nostdin", "-hide_banner", "-loglevel", "error", "-y"};
    args << ffmpeg::trimArgs(m_edits) << "-i" << m_path << "-map" << "0:v:0" << "-map" << "0:a?";
    const QString crop = ffmpeg::cropFilter(m_edits);
    if (!crop.isEmpty())
        args << "-vf" << crop;
    args << "-c:v" << "ffv1" << "-c:a" << "flac" << "-f" << "matroska" << partial.path();
    const Result<QByteArray> done = ffmpeg::run(QStringLiteral("ffmpeg"), args, QStringLiteral("media_unreadable"));
    if (!done)
        return done.errors();
    if (const Result<bool> committed = partial.commit(path, Overwrite::Replace); !committed)
        return committed.errors();
    const Result<MediaHandle> made = probeWithFfprobe(MediaType::Video, path, hashes);
    if (made) {
        const QMutexLocker lock(&appliedMutex);
        appliedEdits().insert(path, AppliedEdit{source, made.value()});
    }
    return made;
}

} // namespace fugue
