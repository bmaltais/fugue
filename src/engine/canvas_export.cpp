#include "engine/canvas_export.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSize>

#include <algorithm>
#include <cmath>

#include "engine/atomic_file.h"
#include "engine/new_canvas.h"
#include "engine/paint_file.h"
#include "engine/psd_file.h"

namespace fugue::canvas {

namespace {

constexpr int webpMaxSide = 16383;

Error usage(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("usage"), message, hint); }

QString hex(uint8_t r, uint8_t g, uint8_t b) { return QStringLiteral("#%1%2%3").arg(r, 2, 16, QLatin1Char('0')).arg(g, 2, 16, QLatin1Char('0')).arg(b, 2, 16, QLatin1Char('0')); }

Errors checkQuality(int quality) {
    if (quality < 1 || quality > 100)
        return Errors{usage(QStringLiteral("The quality is a whole number from 1 to 100."), QString::number(quality))};
    return {};
}

Result<QByteArray> encode(const mask::MaskPaint &flat, const QString &format, int quality, bool lossless) {
    if (format == QLatin1String("webp") && (flat.width() > webpMaxSide || flat.height() > webpMaxSide))
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("WebP holds at most %1 px a side; this is %2 x %3.").arg(webpMaxSide).arg(flat.width()).arg(flat.height()),
                         QStringLiteral("Export a .png instead."));
    return flat.toPicture(format, mask::MaskPaint::PictureEncoding{quality, lossless});
}

} // namespace

QString normalFormat(const QString &format, bool allowLayered) {
    const QString f = format.trimmed().toLower();
    if (f == QLatin1String("jpeg") || f == QLatin1String("jpg"))
        return QStringLiteral("jpg");
    if (f == QLatin1String("png") || f == QLatin1String("webp") || (allowLayered && (f == QLatin1String("ora") || f == QLatin1String("psd"))))
        return f;
    return {};
}

Result<QSize> exportSize(int canvasWidth, int canvasHeight, const PictureExport &options) {
    if (canvasWidth < 1 || canvasHeight < 1)
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("The canvas has no size."));
    int width = options.width, height = options.height;
    if (width < 0 || height < 0)
        return makeError(QStringLiteral("usage"), QStringLiteral("A width or height is a whole number of pixels of at least 1."));
    if (width > 0 && height > 0) {
        // exact
    } else if (width > 0) {
        height = std::max(1, int(std::lround(double(canvasHeight) * width / canvasWidth)));
    } else if (height > 0) {
        width = std::max(1, int(std::lround(double(canvasWidth) * height / canvasHeight)));
    } else {
        if (!(options.scale > 0) || options.scale > 1000)
            return makeError(QStringLiteral("usage"), QStringLiteral("The scale is more than 0 percent and at most 100000 percent."));
        width = std::max(1, int(std::lround(canvasWidth * options.scale)));
        height = std::max(1, int(std::lround(canvasHeight * options.scale)));
    }
    if (width > mask::paintFileMaxSide || height > mask::paintFileMaxSide || qint64(width) * height > mask::paintFileMaxPixels)
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("A picture of %1 x %2 px is past the limit: %3.").arg(width).arg(height).arg(mask::canvasSizeCap()),
                         QStringLiteral("Choose a smaller size."));
    return QSize(width, height);
}

Result<ExportedPicture> exportPicture(const mask::PaintDocument &document, const PictureExport &options) {
    const QString format = normalFormat(options.format, true);
    if (format.isEmpty())
        return usage(QStringLiteral("The format is png, jpg, webp, ora or psd, not %1.").arg(options.format));
    if (const Errors bad = checkQuality(options.quality); hasErrors(bad))
        return bad;
    const Result<QSize> size = exportSize(document.width(), document.height(), options);
    if (!size)
        return size.errors();
    const bool resized = size->width() != document.width() || size->height() != document.height();

    ExportedPicture out;
    out.format = format;
    out.width = size->width();
    out.height = size->height();
    if (format == QLatin1String("ora")) {
        const Result<QByteArray> bytes = mask::paintToOra(resized ? document.scaledTo(out.width, out.height) : document);
        if (!bytes)
            return bytes.errors();
        out.bytes = bytes.value();
        return out;
    }

    if (format == QLatin1String("psd")) {
        Result<mask::PsdExported> psd = mask::paintToPsd(resized ? document.scaledTo(out.width, out.height) : document);
        if (!psd)
            return psd.errors();
        out.bytes = psd->bytes;
        out.notes = psd->notes;
        return out;
    }

    QString background = options.background.trimmed().toLower();
    if (background.isEmpty())
        background = format == QLatin1String("jpg") ? QStringLiteral("white") : QStringLiteral("transparent");
    const Result<mask::CanvasBackground> parsed = mask::parseBackground(background);
    if (!parsed)
        return parsed.errors();
    if (format == QLatin1String("jpg") && !parsed->filled)
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("JPEG has no transparency."), QStringLiteral("Choose a background (white, black or a color), or another format."));
    out.background = parsed->filled ? hex(parsed->color.r, parsed->color.g, parsed->color.b) : QStringLiteral("transparent");
    if (background == QLatin1String("white") || background == QLatin1String("black"))
        out.background = background;

    mask::MaskPaint flat = document.flatten();
    if (resized) {
        Result<mask::MaskPaint> small = flat.resampled(out.width, out.height);
        if (!small)
            return small.errors();
        flat = small.value();
    }
    // A JPEG on white is the encoder's own flatten (what the picture was before a background could be chosen).
    if (parsed->filled && !(format == QLatin1String("jpg") && background == QLatin1String("white")))
        flat = flat.onBackground(parsed->color.r, parsed->color.g, parsed->color.b);
    const Result<QByteArray> bytes = encode(flat, format, options.quality, options.lossless);
    if (!bytes)
        return bytes.errors();
    out.bytes = bytes.value();
    return out;
}

QString safeFileName(const QString &name) {
    static const QRegularExpression unsafe(QStringLiteral("[/\\\\:*?\"<>|\\x00-\\x1f]"));
    QString safe = name;
    safe.replace(unsafe, QStringLiteral("_"));
    safe = safe.trimmed();
    while (safe.startsWith(QLatin1Char('.')) || safe.startsWith(QLatin1Char(' ')))
        safe.remove(0, 1);
    while (safe.endsWith(QLatin1Char('.')) || safe.endsWith(QLatin1Char(' ')))
        safe.chop(1);
    safe = safe.left(80).trimmed();
    return safe.isEmpty() ? QStringLiteral("Layer") : safe;
}

Result<LayerFiles> exportLayers(const mask::PaintDocument &document, const LayersExport &options) {
    const QString format = normalFormat(options.format, false);
    if (format != QLatin1String("png") && format != QLatin1String("webp"))
        return usage(QStringLiteral("Layers are written as png or webp, not %1.").arg(options.format));
    if (const Errors bad = checkQuality(options.quality); hasErrors(bad))
        return bad;
    LayerFiles out;
    const int digits = std::max(2, int(QString::number(document.layerCount()).size()));
    const QString prefix = options.prefix.trimmed().isEmpty() ? QString() : safeFileName(options.prefix) + QLatin1Char('-');
    for (int position = 0; position < document.layerCount(); ++position) {
        const mask::LayerInfo info = document.layer(position);
        if (info.kind == mask::LayerKind::Mask) {
            out.skipped << info.name;
            continue;
        }
        if (options.visibleOnly && !info.visible)
            continue;
        const mask::MaskPaint *pixels = document.pixels(info.id);
        if (!pixels)
            continue;
        mask::MaskPaint picture = *pixels;
        const mask::MaskPaint *layerMask = options.applyMasks && info.hasMask && info.maskEnabled ? document.maskPixels(info.id) : nullptr;
        if (layerMask) {
            std::vector<uint8_t> bytes = picture.pixels();
            const std::vector<uint8_t> &m = layerMask->pixels();
            for (size_t p = 0; p < m.size() && p * 4 + 3 < bytes.size(); ++p)
                bytes[p * 4 + 3] = uint8_t((bytes[p * 4 + 3] * m[p] + 127) / 255);
            picture = mask::MaskPaint(picture.width(), picture.height(), bytes, 4);
        }
        const Result<QByteArray> encoded = encode(picture, format, options.quality, options.lossless);
        if (!encoded)
            return encoded.errors();
        LayerFile file;
        file.position = position;
        file.name = info.name;
        file.fileName = QStringLiteral("%1%2-%3.%4").arg(prefix).arg(position + 1, digits, 10, QLatin1Char('0')).arg(safeFileName(info.name), format);
        file.bytes = encoded.value();
        file.width = picture.width();
        file.height = picture.height();
        out.files.push_back(std::move(file));
    }
    if (out.files.empty())
        return makeError(QStringLiteral("input_invalid"), QStringLiteral("There is no layer to write."),
                         options.visibleOnly ? QStringLiteral("Every picture layer is hidden: include the hidden layers.") : QStringLiteral("A mask layer is a selection, not a picture."));
    return out;
}

Errors writeLayerFiles(const QString &folder, const LayerFiles &files) {
    const QFileInfo target(folder);
    if (target.exists() && !target.isDir())
        return Errors{makeError(QStringLiteral("permission_denied"), QStringLiteral("%1 is a file, not a folder.").arg(folder), QStringLiteral("Choose a folder."))};
    const QDir dir(folder);
    QStringList taken;
    for (const LayerFile &file : files.files)
        if (QFileInfo::exists(dir.filePath(file.fileName)))
            taken << file.fileName;
    if (!taken.isEmpty())
        return Errors{makeError(QStringLiteral("permission_denied"),
                                QStringLiteral("Nothing was written: %1 already in %2 (%3).").arg(taken.size() == 1 ? QStringLiteral("a file is") : QStringLiteral("%1 files are").arg(taken.size()), folder, taken.join(QStringLiteral(", "))),
                                QStringLiteral("Choose another folder or prefix; a file is never replaced."))};
    if (!QDir().mkpath(folder))
        return Errors{makeError(QStringLiteral("permission_denied"), QStringLiteral("The folder %1 could not be made.").arg(folder))};
    for (const LayerFile &file : files.files) {
        const Errors written = createFile(dir.filePath(file.fileName), file.bytes, QStringLiteral("the layer %1").arg(file.name));
        if (hasErrors(written))
            return written;
    }
    return {};
}

} // namespace fugue::canvas
