#include "media/thumbnail_cache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include "engine/ffmpeg.h"

namespace fugue {

namespace {

constexpr int kFfmpegTimeoutMs = 15000;
// Seconds into a clip the frame is taken at: past a fade-in or a black first frame, and a clip
// shorter than this is read again from its start.
constexpr const char *kFrameAt = "0.5";

// The picture ffmpeg makes of a video or a sound, scaled down to fit `box`; null when it cannot.
QImage ffmpegPicture(const QString &path, const QString &kind, int box) {
    const QStringList head{"-nostdin", "-hide_banner", "-loglevel", "error"};
    const QStringList tail{"-frames:v", "1", "-f", "image2pipe", "-c:v", "png", "pipe:1"};
    auto read = [&](const QStringList &args) {
        const Result<QByteArray> done = ffmpeg::run(QStringLiteral("ffmpeg"), head + args + tail, QStringLiteral("media_unreadable"),
                                                    kFfmpegTimeoutMs);
        return done ? QImage::fromData(*done, "PNG") : QImage();
    };
    QImage image;
    if (kind == QLatin1String("audio")) {
        image = read({"-i", path, "-filter_complex",
                      QStringLiteral("showwavespic=s=%1x%2:colors=0xb0b0b0").arg(box).arg(qMax(1, box / 2))});
    } else {
        image = read({"-ss", QString::fromLatin1(kFrameAt), "-i", path});
        if (image.isNull())
            image = read({"-i", path});
    }
    if (image.isNull())
        return {};
    if (image.width() > box || image.height() > box)
        image = image.scaled(box, box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

} // namespace

QString mediaKindOfPath(const QString &path) {
    static const QSet<QString> video{"mp4", "mkv", "webm", "mov", "avi", "m4v", "gif"};
    static const QSet<QString> audio{"wav", "flac", "mp3", "ogg", "opus", "m4a", "aac"};
    const QString suffix = QFileInfo(path).suffix().toLower();
    return video.contains(suffix) ? QStringLiteral("video") : audio.contains(suffix) ? QStringLiteral("audio") : QStringLiteral("image");
}

QString defaultThumbnailDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/fugue/thumbs");
}

QString thumbnailCachePath(const QString &cacheDir, const QString &path, int box) {
    const QFileInfo info(path);
    if (!info.isFile())
        return {};
    const QByteArray key = QStringLiteral("%1\n%2\n%3\n%4")
                               .arg(info.canonicalFilePath())
                               .arg(info.size())
                               .arg(info.lastModified().toMSecsSinceEpoch())
                               .arg(box)
                               .toUtf8();
    return QDir(cacheDir).filePath(QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex()) +
                                   QStringLiteral(".png"));
}

QImage thumbnail(const QString &cacheDir, const QString &path, int box, bool *fromCache) {
    if (fromCache)
        *fromCache = false;
    const QString cached = thumbnailCachePath(cacheDir, path, box);
    if (cached.isEmpty())
        return {};
    if (QFileInfo::exists(cached)) {
        const QImage image(cached);
        if (!image.isNull()) {
            if (fromCache)
                *fromCache = true;
            return image;
        }
    }
    QImage image;
    const QString kind = mediaKindOfPath(path);
    if (kind == QLatin1String("image")) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (size.isValid() && (size.width() > box || size.height() > box))
            reader.setScaledSize(size.scaled(box, box, Qt::KeepAspectRatio));
        image = reader.read();
    } else {
        image = ffmpegPicture(path, kind, box);
    }
    if (image.isNull())
        return {};
    // A failed write only costs the next decode.
    if (QDir().mkpath(cacheDir)) {
        QSaveFile file(cached);
        if (file.open(QIODevice::WriteOnly) && image.save(&file, "PNG"))
            file.commit();
    }
    return image;
}

} // namespace fugue
