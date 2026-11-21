#include "media/thumbnail_cache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>
#include <QStandardPaths>

namespace fugue {

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
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > box || size.height() > box))
        reader.setScaledSize(size.scaled(box, box, Qt::KeepAspectRatio));
    const QImage image = reader.read();
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
