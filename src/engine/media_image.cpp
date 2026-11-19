// Image handles: the decoded libvips form and the file form, and the
// conversion between them.

#include "media.h"

#include <cstdio>

#include <QDir>
#include <QFile>

#include "engine/file_hashes.h"
#include "engine/vips.h"

namespace fugue {

namespace {

using vips::VImage;

Error vipsUnavailable() {
    return makeError(QStringLiteral("internal"), QStringLiteral("libvips could not be started."));
}

Error vipsFailed(const vips::VError &error, const QString &code = QStringLiteral("internal")) {
    return makeError(code, QString::fromUtf8(error.what()).trimmed());
}

QByteArray fileName(const QString &path) {
    return QFile::encodeName(path);
}

// "pngload" is "png", "jpegload" is "jpeg".
QString formatOf(const VImage &image) {
    QString loader = QString::fromUtf8(const_cast<VImage &>(image).get_string("vips-loader"));
    const qsizetype load = loader.indexOf(QLatin1String("load"));
    return load > 0 ? loader.left(load) : loader;
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
        return vipsFailed(error);
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
        return VImage::new_from_file(fileName(m_path).constData());
    } catch (const vips::VError &error) {
        return vipsFailed(error, QStringLiteral("media_unreadable"));
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
        handle.m_width = image.width();
        handle.m_height = image.height();
        handle.m_channels = image.bands();
        handle.m_format = formatOf(image);
    } catch (const vips::VError &error) {
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
    const QString path = QDir(dir).filePath(name + QStringLiteral(".png"));
    const QString partial = QDir(dir).filePath(QStringLiteral(".") + name + QStringLiteral(".png.part"));
    try {
        VImage(ready->first).pngsave(fileName(partial).constData(),
                                     VImage::option()
                                         ->set("compression", 6)
                                         ->set("bitdepth", ready->second)
                                         ->set("keep", VIPS_FOREIGN_KEEP_NONE));
    } catch (const vips::VError &error) {
        QFile::remove(partial);
        return vipsFailed(error);
    }
    if (std::rename(fileName(partial).constData(), fileName(path).constData()) != 0) {
        QFile::remove(partial);
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot write %1.").arg(path));
    }

    MediaHandle handle = *this;
    handle.m_path = path;
    handle.m_format = QStringLiteral("png");
    handle.m_hash = hashes.hash(path);
    if (handle.m_hash.isEmpty())
        return makeError(QStringLiteral("permission_denied"), QStringLiteral("Cannot read %1.").arg(path));
    return handle;
}

} // namespace fugue
