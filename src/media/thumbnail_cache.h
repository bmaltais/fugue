#pragma once

// Thumbnails for the media browser (#133): a small PNG per file and size in a cache folder, so a
// grid of a hundred results costs one decode of each big file, once, and is quick the next time.
// The name of a thumbnail holds the file's path, size and modification time, so a file that
// changes gets a new one; the old ones are the cache folder's to evict (spec 10.5).
//
// Plain QtGui, no QML. MediaThumbnailProvider (media_library.h) is the QML side of it.

#include <QImage>
#include <QSize>
#include <QString>

namespace fugue {

// The cache file a thumbnail of `path` that fits `box` has, or "" when the file is not there.
QString thumbnailCachePath(const QString &cacheDir, const QString &path, int box);

// The thumbnail: from the cache when it is there, else decoded from `path` scaled to fit `box`
// pixels on its longer side (never enlarged), written to the cache, and returned. A null image
// when the file cannot be read as an image. `fromCache`, when given, says which one it was.
QImage thumbnail(const QString &cacheDir, const QString &path, int box, bool *fromCache = nullptr);

// $XDG_CACHE_HOME/fugue/thumbs.
QString defaultThumbnailDir();

} // namespace fugue
