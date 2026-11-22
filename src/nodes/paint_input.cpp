#include "nodes/paint_input.h"

#include <algorithm>
#include <cmath>

#include <QFileInfo>

#include "engine/paint_document.h"
#include "engine/paint_file.h"
#include "nodes/kernels.h"
#include "nodes/pixel_io.h"
#include "nodes/resample.h"

namespace fugue::nodes {

namespace {

using mask::LayerKind;
using mask::MaskPaint;
using mask::PaintDocument;

// Straight RGBA bytes from float pixels (gray and RGB get an opaque alpha).
MaskPaint toBytes(const Pixels &in) {
    std::vector<uint8_t> out(size_t(in.width) * in.height * 4);
    const auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x) {
            uint8_t *p = &out[(size_t(y) * in.width + x) * 4];
            for (int k = 0; k < 3; ++k)
                p[k] = byte(in.at(x, y, std::min(k, in.channels - 1)));
            p[3] = in.channels == 4 ? byte(in.at(x, y, 3)) : 255;
        }
    return MaskPaint(in.width, in.height, out, 4);
}

Pixels toPixels(const MaskPaint &in) {
    Pixels out(in.width(), in.height(), in.channels());
    for (size_t i = 0; i < out.data.size(); ++i)
        out.data[i] = float(in.pixels()[i]) / 255.0f;
    return out;
}

// The picture is what the layers are painted over, so it is all that is left of it: nothing else
// is in the way when every layer is a visible, whole picture.
bool onlyThePicture(const PaintDocument &document) {
    if (document.layerCount() == 0)
        return false;
    for (int i = 0; i < document.layerCount(); ++i) {
        const mask::LayerInfo layer = document.layer(i);
        if (layer.kind != LayerKind::Picture || !layer.visible || layer.opacity < 1)
            return false;
    }
    return true;
}

// A canvas size an input gives: 0 for unset.
Result<int> sizeOf(const NodeContext &context, const QString &name) {
    const Value value = context.input(name);
    if (value.isEmpty() || value.isMedia() || !value.json().isDouble())
        return 0;
    const double rounded = std::round(value.number());
    if (!(rounded >= 1 && rounded <= 8192))
        return makeInputError(QStringLiteral("value_out_of_range"), name, QStringLiteral("The canvas %1 must be 1 to 8192.").arg(name));
    return int(rounded);
}

class PaintKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Value image = context.input(QStringLiteral("image"));
        const bool hasImage = image.isMedia();

        // The layers, when something is painted.
        std::optional<PaintDocument> document;
        QString file = context.input(QStringLiteral("paint")).text();
        if (!file.trimmed().isEmpty()) {
            if (context.roots) {
                // Read the path that was checked, so check and use cannot disagree.
                const Result<QString> readable = context.roots->checkRead(file, QStringLiteral("paint"));
                if (!readable)
                    return Result<Values>(readable.errors());
                file = readable.value();
            }
            if (!QFileInfo(file).isFile())
                return makeInputError(QStringLiteral("file_not_found"), QStringLiteral("paint"),
                                      QStringLiteral("The painting %1 is not there.").arg(file),
                                      QStringLiteral("Paint again."));
            Result<PaintDocument> loaded = mask::readPaintFile(file);
            if (!loaded)
                return Result<Values>(withInput(loaded.errors(), QStringLiteral("paint")));
            document = std::move(loaded.value());
        }

        // The canvas: the picture's size, else Width and Height, else the painting's own.
        int width = 0, height = 0;
        if (hasImage) {
            width = image.media().width();
            height = image.media().height();
        } else {
            const Result<int> w = sizeOf(context, QStringLiteral("width"));
            const Result<int> h = sizeOf(context, QStringLiteral("height"));
            if (!w.ok())
                return Result<Values>(w.errors());
            if (!h.ok())
                return Result<Values>(h.errors());
            width = w.value();
            height = h.value();
            if ((width == 0) != (height == 0))
                return makeInputError(QStringLiteral("missing_input"), QString::fromLatin1(width == 0 ? "width" : "height"),
                                      QStringLiteral("Give both Width and Height for a blank canvas."));
            if (width == 0 && document) {
                width = document->width();
                height = document->height();
            }
            if (width == 0)
                return makeInputError(QStringLiteral("missing_input"), QStringLiteral("image"),
                                      QStringLiteral("Paint has no picture and no size."),
                                      QStringLiteral("Connect a picture, or set Width and Height."));
        }

        Errors warnings;
        if (!document) {
            document.emplace(width, height);
            document->loadLayer(LayerKind::Picture, QStringLiteral("Picture"), true, 1, MaskPaint(1, 1, 4));
        } else if (document->width() != width || document->height() != height) {
            Error resized = makeInputError(QStringLiteral("mask_resized"), QStringLiteral("paint"),
                                           QStringLiteral("The layers were painted at %1 x %2 and are scaled to %3 x %4.")
                                               .arg(document->width()).arg(document->height()).arg(width).arg(height),
                                           QStringLiteral("Open Paint and check the edges."));
            resized.severity = Error::Severity::Warning;
            warnings << resized;
            document = document->scaledTo(width, height);
        }

        // The picture in its place.
        if (hasImage) {
            const Result<Pixels> pixels = pixelsOf(image, QStringLiteral("image"));
            if (!pixels.ok())
                return Result<Values>(pixels.errors());
            const MaskPaint picture = toBytes(pixels.value());
            for (int i = 0; i < document->layerCount(); ++i)
                document->setPicture(document->layer(i).id, picture);
        }

        Values out;
        if (hasImage && onlyThePicture(*document)) {
            out.insert(QStringLiteral("image"), image); // nothing painted over it: the picture itself, exactly
        } else {
            Result<Values> flat = imageOutput(toPixels(document->flatten()));
            if (!flat.ok())
                return flat;
            out.insert(QStringLiteral("image"), flat.value().value(QStringLiteral("image")));
        }
        Result<Values> mask = namedOutput(toPixels(document->flattenMask()), "mask");
        if (!mask.ok())
            return mask;
        out.insert(QStringLiteral("mask"), mask.value().value(QStringLiteral("mask")));
        return warnings.isEmpty() ? Result<Values>(out) : Result<Values>(out, warnings);
    }

private:
    static Errors withInput(Errors errors, const QString &input) {
        for (Error &error : errors)
            if (error.input.isEmpty())
                error.input = input;
        return errors;
    }
};

} // namespace

void addPaintInputKernels(Kernels &kernels) {
    kernels.add("fugue/paint", std::make_shared<PaintKernel>());
}

} // namespace fugue::nodes
