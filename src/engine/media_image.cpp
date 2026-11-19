// Image handles: the decoded libvips form and the file form, and the
// conversion between them.

#include "media.h"


#include <QDir>
#include <QFile>

#include "engine/atomic_file.h"
#include "engine/file_hashes.h"
#include "engine/vips.h"

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

Result<MediaHandle> MediaHandle::probeImage(const QString &path, FileHashes &hashes) {
    if (!initVips())
        return vipsUnavailable();
    MediaHandle handle;
    handle.m_type = MediaType::Image;
    handle.m_path = path;
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
        return makeError(QStringLiteral("media_unreadable"), QStringLiteral("%1 is not an image Fugue can read.").arg(path));
    }
    handle.m_hash = hashes.hash(path);
    if (handle.m_hash.isEmpty())
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot read %1.").arg(path));
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
