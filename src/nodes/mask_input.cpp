#include "nodes/mask_input.h"

#include <QFileInfo>

#include "engine/vips.h"
#include "nodes/kernels.h"
#include "nodes/mask_ops.h"
#include "nodes/pixel_io.h"

namespace fugue::nodes {

namespace {

// The painted mask the `mask` input names, as one channel at the size of `picture`; an empty buffer
// (no width) when nothing is painted.
Result<Pixels> readPainted(const NodeContext &context, const MediaHandle &picture, Errors &warnings) {
    QString file = context.input(QStringLiteral("mask")).text();
    if (file.trimmed().isEmpty())
        return Pixels();
    if (context.roots) {
        // Read the path that was checked, so check and use cannot disagree.
        const Result<QString> readable = context.roots->checkRead(file, QStringLiteral("mask"));
        if (!readable)
            return readable.errors();
        file = readable.value();
    }
    if (!QFileInfo(file).isFile())
        return makeInputError(QStringLiteral("file_not_found"), QStringLiteral("mask"),
                              QStringLiteral("The painted mask %1 is not there.").arg(file),
                              QStringLiteral("Paint the mask again."));
    const Result<MediaHandle> media = MediaHandle::probe(MediaType::Image, file, *context.hashes);
    if (!media)
        return media.errors();
    Result<Pixels> loaded = maskOf(Value(media.value()), QStringLiteral("mask"));
    if (!loaded.ok())
        return loaded;
    Pixels mask = std::move(loaded.value());
    if (mask.width != picture.width() || mask.height != picture.height()) {
        Error resized = makeInputError(QStringLiteral("mask_resized"), QStringLiteral("mask"),
                                       QStringLiteral("The mask was painted at %1 x %2 and is scaled to the picture, %3 x %4.")
                                           .arg(mask.width).arg(mask.height).arg(picture.width()).arg(picture.height()),
                                       QStringLiteral("Open Paint mask and check the edge."));
        resized.severity = Error::Severity::Warning;
        warnings << resized;
        mask = maskResized(mask, picture.width(), picture.height());
    }
    return mask;
}

class PaintMaskKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Value image = context.input(QStringLiteral("image"));
        if (!image.isMedia())
            return makeInputError(QStringLiteral("missing_input"), QStringLiteral("image"),
                                  QStringLiteral("Input image has no image."));
        Errors warnings;
        Result<Pixels> mask = paintedMask(context, image.media(), warnings);
        if (!mask.ok())
            return Result<Values>(mask.errors());
        Result<Values> out = namedOutput(std::move(mask.value()), "mask");
        if (!out.ok() || warnings.isEmpty())
            return out;
        return Result<Values>(out.value(), warnings);
    }
};

} // namespace

Result<Pixels> paintedMask(const NodeContext &context, const MediaHandle &picture, Errors &warnings) {
    Result<Pixels> painted = readPainted(context, picture, warnings);
    if (!painted.ok() || painted.value().width > 0)
        return painted;
    return Pixels(picture.width(), picture.height(), 1, 0.0f);
}

Result<Pixels> loadImageMask(const NodeContext &context, const MediaHandle &picture, Errors &warnings) {
    Result<Pixels> painted = readPainted(context, picture, warnings);
    if (!painted.ok() || painted.value().width > 0)
        return painted;
    // Not painted: ComfyUI's Load Image mask, 1 - alpha, when the file has an alpha channel.
    const Result<vips::VImage> image = picture.image();
    if (!image.ok())
        return Result<Pixels>(image.errors());
    bool alpha = false;
    try {
        initVips();
        alpha = image.value().has_alpha();
    } catch (const vips::VError &error) {
        return vipsError(error, QStringLiteral("media_unreadable"));
    }
    if (alpha) {
        const Result<Pixels> pixels = pixelsOf(Value(picture), QStringLiteral("file"));
        if (!pixels.ok())
            return Result<Pixels>(pixels.errors());
        const Pixels &in = pixels.value();
        if (in.channels == 4) {
            Pixels mask(in.width, in.height, 1);
            for (int y = 0; y < in.height; ++y)
                for (int x = 0; x < in.width; ++x)
                    mask.at(x, y, 0) = 1.0f - in.at(x, y, 3);
            return mask;
        }
    }
    return Pixels(picture.width(), picture.height(), 1, 0.0f);
}

void addMaskInputKernels(Kernels &kernels) {
    kernels.add("fugue/paint-mask", std::make_shared<PaintMaskKernel>());
}

} // namespace fugue::nodes
