// Image handles: the decoded libvips form and the file form, and the
// conversion between them.

#include "media.h"


#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>

#include "engine/atomic_file.h"
#include "engine/file_hashes.h"
#include "engine/hash.h"
#include "engine/vips.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>

namespace fugue {

namespace {

using vips::VImage;

QByteArray fileName(const QString &path) {
    return QFile::encodeName(path);
}

// "pngload" is "png", "jpegload" is "jpeg".
QString formatOf(const VImage &image) {
    QString loader = QString::fromUtf8(const_cast<VImage &>(image).get_string("vips-loader"));
    const qsizetype load = loader.indexOf(QLatin1String("load"));
    return load > 0 ? loader.left(load) : loader;
}

// The EXIF orientation (1 to 8) a file's image says it is to be shown at; 1 when it has none.
int orientationOf(const VImage &image) {
    VImage &header = const_cast<VImage &>(image);
    return header.get_typeof("orientation") == 0 ? 1 : header.get_int("orientation");
}

// The image as PNG can hold it without loss, and the bit depth to write.
Result<std::pair<VImage, int>> pngReady(const VImage &image) {
    if (image.bands() < 1 || image.bands() > 4)
        return makeError(QStringLiteral("internal"),
                         QStringLiteral("A PNG holds 1 to 4 channels, not %1.").arg(image.bands()));
    VImage out = image;
    int depth = 8;
    switch (image.format()) {
    case VIPS_FORMAT_UCHAR:
        break;
    case VIPS_FORMAT_USHORT:
        depth = 16;
        break;
    case VIPS_FORMAT_FLOAT:
    case VIPS_FORMAT_DOUBLE:
        // Working images hold 0 to 1 (as ComfyUI's do); 16 bits keep them
        // to within 1/65535 when they are read back.
        depth = 16;
        out = image.linear(65535.0, 0.0)
                  .clamp(VImage::option()->set("min", 0.0)->set("max", 65535.0))
                  .rint()
                  .cast(VIPS_FORMAT_USHORT);
        break;
    default:
        return makeError(QStringLiteral("internal"), QStringLiteral("This image's pixel format cannot be saved as PNG."));
    }
    const bool grey = image.bands() <= 2;
    const VipsInterpretation interpretation = depth == 16 ? (grey ? VIPS_INTERPRETATION_GREY16 : VIPS_INTERPRETATION_RGB16)
                                                          : (grey ? VIPS_INTERPRETATION_B_W : VIPS_INTERPRETATION_sRGB);
    out = out.copy(VImage::option()->set("interpretation", interpretation));
    return std::make_pair(out, depth);
}

} // namespace

Result<MediaHandle> MediaHandle::fromImage(const VImage &image) {
    if (!initVips())
        return vipsUnavailable();
    MediaHandle handle;
    handle.m_type = MediaType::Image;
    try {
        handle.m_image = image.copy_memory();
    } catch (const vips::VError &error) {
        return vipsError(error);
    }
    handle.m_width = handle.m_image->width();
    handle.m_height = handle.m_image->height();
    handle.m_channels = handle.m_image->bands();
    return handle;
}

Result<vips::VImage> MediaHandle::image() const {
    if (m_type != MediaType::Image)
        return makeError(QStringLiteral("type_mismatch"),
                         QStringLiteral("A %1 handle has no image; read its frames instead.").arg(mediaTypeName(m_type)));
    if (m_image)
        return *m_image;
    if (!hasFile())
        return makeError(QStringLiteral("internal"), QStringLiteral("This image handle has neither a file nor an image."));
    if (!initVips())
        return vipsUnavailable();
    try {
        // Upright, as the orientation tag says it is seen (the size probe() reported).
        const VImage image = VImage::new_from_file(fileName(m_path).constData());
        return orientationOf(image) == 1 ? image : image.autorot();
    } catch (const vips::VError &error) {
        return vipsError(error, QStringLiteral("media_unreadable"));
    }
}

std::optional<QSize> MediaHandle::imageHeaderSize(const QString &path) {
    // Judged by stat() first, so a device or a FIFO is never opened at all (opening /dev/ttyUSB0 resets the board on the other end,
    // /dev/video0 powers a camera): the check asks after every edit. What passes is opened once, without blocking, and judged again by
    // what was opened (fstat), since a path checked first could be swapped for another between the check and the open. Only a regular
    // file is read, through the descriptor that was checked.
    struct stat named;
    if (::stat(QFile::encodeName(path).constData(), &named) != 0 || !S_ISREG(named.st_mode))
        return std::nullopt;
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    const auto closeFd = qScopeGuard([fd] { ::close(fd); });
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return std::nullopt;
    // Remembered by the file's identity, as FileHashes does for hashes: the check asks after every edit, and reading some
    // headers (svg, pdf, heif) is not cheap. The key is what `stat` says: device and inode, size, and the modification and
    // status-change times in nanoseconds, so a same-size replacement in the same millisecond, or on a file system with coarse
    // times, is a different key. A file that is not a picture is remembered too.
    struct Header {
        std::array<qint64, 7> stamp{};
        std::optional<QSize> size;
    };
    static QMutex mutex;
    static QHash<QString, Header> memo;
    const QString key = QFileInfo(path).canonicalFilePath();
    const std::array<qint64, 7> stamp{qint64(st.st_dev), qint64(st.st_ino), qint64(st.st_size), qint64(st.st_mtim.tv_sec),
                                      qint64(st.st_mtim.tv_nsec), qint64(st.st_ctim.tv_sec), qint64(st.st_ctim.tv_nsec)};
    {
        const QMutexLocker lock(&mutex);
        if (const auto found = memo.constFind(key); found != memo.constEnd() && found->stamp == stamp)
            return found->size;
    }
    std::optional<QSize> size;
    if (initVips()) {
        try {
            // The source reads the open descriptor and does not close it (the guard above does). Its loaders are not cached
            // (libvips marks every *load_source operation nocache), so no copy of the descriptor outlives this call.
            const auto sizeOf = [](const VImage &image) {
                const bool turned = orientationOf(image) >= 5 && orientationOf(image) <= 8;
                return turned ? QSize(image.height(), image.width()) : QSize(image.width(), image.height());
            };
            try {
                const vips::VSource source = vips::VSource::new_from_descriptor(fd);
                size = sizeOf(VImage::new_from_source(source, ""));
            } catch (const vips::VError &) {
                // A format libvips only loads from a file (OpenEXR, NIfTI, Matlab, OpenSlide...) has no source loader. It is read through
                // the descriptor that was checked (/proc/self/fd/N names that very file, not whatever the name points to now);
                // those loaders decide by the file's content. Sequential, so the header is read and no pixel; the file is let go with
                // the image.
                const QByteArray viaDescriptor = "/proc/self/fd/" + QByteArray::number(fd);
                try {
                    size = sizeOf(VImage::new_from_file(viaDescriptor.constData(), VImage::option()->set("access", VIPS_ACCESS_SEQUENTIAL)));
                } catch (const vips::VError &) {
                    // One that decides by the suffix: by name, but only while the name is still the file that was checked (the same
                    // device and inode), and judged again after.
                    struct stat again;
                    if (::stat(QFile::encodeName(path).constData(), &again) == 0 && S_ISREG(again.st_mode) && again.st_dev == st.st_dev &&
                        again.st_ino == st.st_ino) {
                        const QSize byName = sizeOf(VImage::new_from_file(fileName(path).constData(), VImage::option()->set("access", VIPS_ACCESS_SEQUENTIAL)));
                        struct stat after;
                        if (::stat(QFile::encodeName(path).constData(), &after) == 0 && after.st_dev == st.st_dev && after.st_ino == st.st_ino)
                            size = byName;
                    }
                }
            }
        } catch (const vips::VError &) {
        }
    }
    const QMutexLocker lock(&mutex);
    if (memo.size() > 512)
        memo.clear();
    memo.insert(key, Header{stamp, size});
    return size;
}

Result<MediaHandle> MediaHandle::probeImage(const QString &path, FileHashes &hashes, const QString &shown, const QString &shownHash) {
    const QString &named = shown.isEmpty() ? path : shown;
    if (!initVips())
        return vipsUnavailable();
    MediaHandle handle;
    handle.m_type = MediaType::Image;
    handle.m_path = named;
    try {
        // Opening reads the header only; pixels are decoded when read.
        const VImage image = VImage::new_from_file(fileName(path).constData());
        // The size it is seen at: orientations 5 to 8 turn it a quarter.
        const bool turned = orientationOf(image) >= 5 && orientationOf(image) <= 8;
        handle.m_width = turned ? image.height() : image.width();
        handle.m_height = turned ? image.width() : image.height();
        handle.m_channels = image.bands();
        handle.m_format = formatOf(image);
    } catch (const vips::VError &) {
        return makeError(QStringLiteral("media_unreadable"), QStringLiteral("%1 is not an image Fugue can read.").arg(named));
    }
    handle.m_hash = shown.isEmpty() ? hashes.hash(path) : shownHash;
    if (handle.m_hash.isEmpty())
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot read %1.").arg(named));
    return handle;
}

Result<MediaHandle> MediaHandle::savedImage(const QString &dir, const QString &name, FileHashes &hashes) const {
    if (!initVips())
        return vipsUnavailable();
    const Result<std::pair<VImage, int>> ready = pngReady(*m_image);
    if (!ready)
        return ready.error();
    if (!QDir().mkpath(dir))
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot create %1.").arg(dir));

    // Written beside its final name and renamed, so the name never holds a
    // partial file. No metadata and a fixed compression level: the same
    // pixels always give the same bytes, so an unchanged result keeps its
    // hash and stays a cache hit (spec 10.1).
    // The same pixels make the same file, so one already at the name is replaced.
    const QString path = QDir(dir).filePath(name + QStringLiteral(".png"));
    PartialFile partial(dir, name + QStringLiteral(".png"));
    try {
        VImage(ready->first).pngsave(fileName(partial.path()).constData(),
                                     VImage::option()
                                         ->set("compression", 6)
                                         ->set("bitdepth", ready->second)
                                         ->set("keep", VIPS_FOREIGN_KEEP_NONE));
    } catch (const vips::VError &error) {
        return vipsError(error);
    }
    if (const Result<bool> committed = partial.commit(path, Overwrite::Replace); !committed)
        return committed.errors();

    MediaHandle handle = *this;
    handle.m_path = path;
    handle.m_format = QStringLiteral("png");
    handle.m_hash = hashes.hash(path);
    if (handle.m_hash.isEmpty())
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot read %1.").arg(path));
    return handle;
}

} // namespace fugue
