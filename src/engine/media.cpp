#include "media.h"

#include <QDir>
#include <QFileInfo>

#include "engine/file_hashes.h"
#include "engine/hash.h"

namespace fugue {

namespace {

Error invalid(const QString &message) {
    return makeError(QStringLiteral("invalid_document"), message);
}

Error outOfRange(const QString &message) {
    return makeError(QStringLiteral("value_out_of_range"), message);
}

std::optional<MediaType> typeFromName(const QString &name) {
    if (name == QLatin1String("image"))
        return MediaType::Image;
    if (name == QLatin1String("video"))
        return MediaType::Video;
    if (name == QLatin1String("audio"))
        return MediaType::Audio;
    return std::nullopt;
}

} // namespace

QString mediaTypeName(MediaType type) {
    switch (type) {
    case MediaType::Image:
        return QStringLiteral("image");
    case MediaType::Video:
        return QStringLiteral("video");
    case MediaType::Audio:
        return QStringLiteral("audio");
    }
    return {};
}

Result<MediaHandle> MediaHandle::fromJson(const QJsonObject &object) {
    const std::optional<MediaType> type = typeFromName(object.value("type").toString());
    if (!type)
        return invalid(QStringLiteral("A media handle's type must be image, video or audio."));

    MediaHandle handle;
    handle.m_type = *type;
    handle.m_path = object.value("path").toString();
    handle.m_hash = object.value("hash").toString();
    handle.m_format = object.value("format").toString();
    if (handle.m_path.isEmpty() || !QDir::isAbsolutePath(handle.m_path))
        return invalid(QStringLiteral("A media handle needs the absolute path of its file."));
    if (!isSha256(handle.m_hash))
        return invalid(QStringLiteral("A media handle's hash must be sha256:<64 hex digits>."));

    if (*type != MediaType::Audio) {
        handle.m_width = object.value("width").toInt();
        handle.m_height = object.value("height").toInt();
    }
    if (*type != MediaType::Video)
        handle.m_channels = object.value("channels").toInt();
    if (*type == MediaType::Video) {
        handle.m_fps = object.value("fps").toDouble();
        handle.m_frames = object.value("frames").toInteger();
        handle.m_hasAudio = object.value("has_audio").toBool();
    }
    if (*type != MediaType::Image)
        handle.m_duration = object.value("duration").toDouble();
    if (*type == MediaType::Audio)
        handle.m_sampleRate = object.value("sample_rate").toInt();

    if (object.contains("edits")) {
        if (*type != MediaType::Video)
            return invalid(QStringLiteral("Only a video handle has edits."));
        const QJsonObject edits = object.value("edits").toObject();
        if (edits.contains("trim")) {
            const QJsonObject trim = edits.value("trim").toObject();
            const Trim value{trim.value("start").toDouble(), trim.value("end").toDouble()};
            if (value.start < 0 || value.end <= value.start)
                return invalid(QStringLiteral("A trim needs 0 <= start < end."));
            handle.m_edits.trim = value;
        }
        if (edits.contains("crop")) {
            const QJsonObject crop = edits.value("crop").toObject();
            const Crop value{crop.value("x").toInt(), crop.value("y").toInt(),
                             crop.value("width").toInt(), crop.value("height").toInt()};
            if (value.x < 0 || value.y < 0 || value.width <= 0 || value.height <= 0)
                return invalid(QStringLiteral("A crop needs x, y >= 0 and a positive size."));
            handle.m_edits.crop = value;
        }
    }
    return handle;
}

QJsonObject MediaHandle::toJson() const {
    QJsonObject object{{"type", mediaTypeName(m_type)}};
    if (!m_path.isEmpty())
        object.insert("path", m_path);
    if (!m_hash.isEmpty())
        object.insert("hash", m_hash);
    if (!m_format.isEmpty())
        object.insert("format", m_format);
    if (m_type != MediaType::Audio) {
        object.insert("width", m_width);
        object.insert("height", m_height);
    }
    if (m_type != MediaType::Video)
        object.insert("channels", m_channels);
    if (m_type == MediaType::Video) {
        object.insert("fps", m_fps);
        object.insert("frames", m_frames);
        object.insert("has_audio", m_hasAudio);
    }
    if (m_type != MediaType::Image)
        object.insert("duration", m_duration);
    if (m_type == MediaType::Audio)
        object.insert("sample_rate", m_sampleRate);
    if (!m_edits.isEmpty()) {
        QJsonObject edits;
        if (m_edits.trim)
            edits.insert("trim", QJsonObject{{"start", m_edits.trim->start}, {"end", m_edits.trim->end}});
        if (m_edits.crop)
            edits.insert("crop", QJsonObject{{"x", m_edits.crop->x},
                                             {"y", m_edits.crop->y},
                                             {"width", m_edits.crop->width},
                                             {"height", m_edits.crop->height}});
        object.insert("edits", edits);
    }
    return object;
}

int MediaHandle::effectiveWidth() const {
    return m_edits.crop ? m_edits.crop->width : m_width;
}

int MediaHandle::effectiveHeight() const {
    return m_edits.crop ? m_edits.crop->height : m_height;
}

double MediaHandle::effectiveDuration() const {
    return m_edits.trim ? m_edits.trim->end - m_edits.trim->start : m_duration;
}

Result<MediaHandle> MediaHandle::trimmed(double start, double end) const {
    if (m_type != MediaType::Video)
        return makeError(QStringLiteral("type_mismatch"), QStringLiteral("Only a video can be trimmed."));
    const double length = effectiveDuration();
    if (start < 0 || end <= start || end > length)
        return outOfRange(QStringLiteral("A trim needs 0 <= start < end <= %1 seconds.").arg(length));
    const double offset = m_edits.trim ? m_edits.trim->start : 0;
    MediaHandle handle = *this;
    handle.m_edits.trim = Trim{offset + start, offset + end};
    return handle;
}

Result<MediaHandle> MediaHandle::cropped(int x, int y, int width, int height) const {
    if (m_type != MediaType::Video)
        return makeError(QStringLiteral("type_mismatch"), QStringLiteral("Only a video can be cropped."));
    const int frameWidth = effectiveWidth();
    const int frameHeight = effectiveHeight();
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || qint64(x) + width > frameWidth ||
        qint64(y) + height > frameHeight)
        return outOfRange(QStringLiteral("A crop must fit inside the %1x%2 frame.").arg(frameWidth).arg(frameHeight));
    const int offsetX = m_edits.crop ? m_edits.crop->x : 0;
    const int offsetY = m_edits.crop ? m_edits.crop->y : 0;
    MediaHandle handle = *this;
    handle.m_edits.crop = Crop{offsetX + x, offsetY + y, width, height};
    return handle;
}

Result<MediaHandle> MediaHandle::probe(MediaType type, const QString &path, FileHashes &hashes) {
    const QFileInfo info(path);
    if (!info.isFile())
        return makeError(QStringLiteral("file_not_found"), QStringLiteral("There is no file %1.").arg(path));
    if (!info.isReadable())
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot read %1.").arg(path));
    if (type == MediaType::Image)
        return probeImage(info.absoluteFilePath(), hashes);
    return probeWithFfprobe(type, info.absoluteFilePath(), hashes);
}

Result<MediaHandle> MediaHandle::saved(const QString &dir, const QString &name, FileHashes &hashes) const {
    if (!hasFile()) {
        if (m_type == MediaType::Image && hasImage())
            return savedImage(dir, name, hashes);
        return makeError(QStringLiteral("internal"), QStringLiteral("This %1 handle has no file.").arg(mediaTypeName(m_type)));
    }
    if (!m_hash.isEmpty())
        return *this;
    MediaHandle handle = *this;
    handle.m_hash = hashes.hash(m_path);
    if (handle.m_hash.isEmpty())
        return makeError(QStringLiteral("file_not_found"), QStringLiteral("Cannot read %1.").arg(m_path));
    return handle;
}

Result<MediaHandle> MediaHandle::plainFile(const QString &dir, const QString &name, FileHashes &hashes) const {
    const Result<MediaHandle> handle = saved(dir, name, hashes);
    if (!handle || handle->m_edits.isEmpty())
        return handle;
    return handle->withEditsApplied(dir, name, hashes);
}

MediaHandle MediaHandle::atPath(const QString &path) const {
    MediaHandle handle = *this;
    handle.m_path = path;
    return handle;
}

} // namespace fugue
