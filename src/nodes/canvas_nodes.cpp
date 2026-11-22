#include "nodes/canvas_nodes.h"

#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <optional>

#include "engine/canvas_handle.h"
#include "engine/canvas_store.h"
#include "engine/hash.h"
#include "nodes/canvas_region_ops.h"
#include "nodes/kernels.h"
#include "nodes/pixel_io.h"
#include "nodes/stitch_handle.h"

namespace fugue::nodes {

namespace {

using canvas::CanvasHandle;
using canvas::CanvasStore;
using mask::LayerKind;
using mask::PaintDocument;

constexpr int kMaxSide = 16384; // the store's own limit (14.5)
constexpr int kMaxPadding = 4096;
constexpr int kMaxBlend = 512;
constexpr int kMaxFeather = 512;

Error warning(Error error) {
    error.severity = Error::Severity::Warning;
    return error;
}

Result<int> intInput(const NodeContext &context, const char *name, int lowest, int highest) {
    const QString input = QString::fromLatin1(name);
    const int value = toInt(context.input(input));
    if (value < lowest || value > highest)
        return makeInputError(QStringLiteral("value_out_of_range"), input,
                              QStringLiteral("%1 is outside %2 to %3.").arg(value).arg(lowest).arg(highest));
    return value;
}

Result<PaintDocument> openRevision(const CanvasHandle &handle) {
    return CanvasStore::forSession().load(handle.canvas, handle.revision);
}

// The canvas input of Region: the id and a revision that the compile has already named in full.
Result<CanvasHandle> regionSource(const NodeContext &context) {
    CanvasHandle handle{context.input(QStringLiteral("canvas")).text().trimmed(), context.input(QStringLiteral("revision")).text().trimmed()};
    if (!canvas::isCanvasId(handle.canvas))
        return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("canvas"),
                              QStringLiteral("\"%1\" is not a canvas id.").arg(handle.canvas),
                              QStringLiteral("Use the id `fugue canvas new` prints, such as canvas-0123456789ab."));
    if (!isSha256(handle.revision))
        return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("revision"),
                              QStringLiteral("The revision \"%1\" was not named in full when the workflow was compiled.").arg(handle.revision),
                              QStringLiteral("Run the workflow with `fugue run` or the Run button, which name head once, and wire the canvas and revision as literals."));
    return handle;
}

class CanvasRegionKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<CanvasHandle> source = regionSource(context);
        if (!source)
            return Result<Values>(source.errors());
        const Result<int> padding = intInput(context, "padding", 0, kMaxPadding);
        const Result<int> feather = intInput(context, "feather", 0, kMaxFeather);
        const Result<int> multiple = intInput(context, "multiple", 1, 256);
        const double scale = context.input(QStringLiteral("scale")).number();
        const double cap = context.input(QStringLiteral("max_megapixels")).number();
        for (const Result<int> *checked : {&padding, &feather, &multiple})
            if (!checked->ok())
                return Result<Values>(checked->errors());
        if (!(scale >= 0.25 && scale <= 4))
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("scale"), QStringLiteral("A scale of %1 is outside 0.25 to 4.").arg(scale));
        if (!(cap >= 0 && cap <= 16))
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("max_megapixels"), QStringLiteral("%1 megapixels is outside 0 to 16.").arg(cap));

        Result<PaintDocument> opened = openRevision(source.value());
        if (!opened)
            return Result<Values>(opened.errors());
        const Pixels composite = layerToPixels(opened->flatten());

        Errors warnings;
        std::optional<CropBox> bounds;
        Pixels mask;
        if (const Value given = context.input(QStringLiteral("mask")); given.isMedia()) {
            Result<Pixels> read = maskOf(given, QStringLiteral("mask"));
            if (!read)
                return Result<Values>(read.errors());
            mask = std::move(read.value());
            if (mask.width != composite.width || mask.height != composite.height) {
                warnings << warning(makeInputError(QStringLiteral("mask_resized"), QStringLiteral("mask"),
                                                   QStringLiteral("The mask is %1 x %2 and the canvas %3 x %4; the mask was scaled to the canvas.")
                                                       .arg(mask.width).arg(mask.height).arg(composite.width).arg(composite.height)));
                mask = maskResized(mask, composite.width, composite.height);
            }
            bounds = maskBounds(mask);
            if (!bounds)
                return makeInputError(QStringLiteral("mask_empty"), QStringLiteral("mask"), QStringLiteral("Nothing is painted in the mask."),
                                      QStringLiteral("Paint the part to change."));
        } else {
            bounds = rectBox(toInt(context.input(QStringLiteral("x"))), toInt(context.input(QStringLiteral("y"))),
                             toInt(context.input(QStringLiteral("width"))), toInt(context.input(QStringLiteral("height"))),
                             composite.width, composite.height);
            if (!bounds)
                return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("x"),
                                      QStringLiteral("The rectangle is outside the canvas (%1 x %2).").arg(composite.width).arg(composite.height));
            mask = boxMask(*bounds, composite.width, composite.height);
        }

        const CropBox box = paddedBox(*bounds, padding.value(), composite.width, composite.height);
        const CropSize size = regionSize(box, scale, cap, multiple.value());
        Cropped crop = cropAt(composite, mask, box, size);
        // The feather is in canvas pixels; the region may have been redrawn at another scale.
        const int radius = int(std::lround(double(feather.value()) * size.width / box.width));
        crop.mask = featherMask(crop.mask, radius, FeatherDirection::Outward);

        // The stitch (Canvas Write's and Stitch's): the canvas as the original, and the region's own mask.
        Result<Values> original = imageOutput(composite);
        Result<Values> whole = namedOutput(mask, "mask");
        if (!original || !whole)
            return !original ? Result<Values>(original.errors()) : Result<Values>(whole.errors());
        const Result<MediaHandle> originalFile = namedHandle(original.value().value(QStringLiteral("image")).media(), context, QStringLiteral("canvas-image"));
        const Result<MediaHandle> maskFile = namedHandle(whole.value().value(QStringLiteral("mask")).media(), context, QStringLiteral("canvas-mask"));
        if (!originalFile || !maskFile)
            return !originalFile ? Result<Values>(originalFile.errors()) : Result<Values>(maskFile.errors());
        const Result<Value> stitch = writeStitchHandle(context.workDir, {originalFile.value(), maskFile.value(), crop.box, crop.size});
        const Result<QJsonObject> handle = canvas::writeCanvasHandle(context.workDir, source.value());
        Result<Values> image = imageOutput(std::move(crop.image));
        Result<Values> regionMask = namedOutput(std::move(crop.mask), "mask");
        for (const Errors &errors : {stitch.errors(), handle.errors(), image.errors(), regionMask.errors()})
            if (hasErrors(errors))
                return Result<Values>(errors);

        Values out;
        out.insert(QStringLiteral("image"), image.value().value(QStringLiteral("image")));
        out.insert(QStringLiteral("mask"), regionMask.value().value(QStringLiteral("mask")));
        out.insert(QStringLiteral("stitch"), stitch.value());
        out.insert(QStringLiteral("canvas"), Value(handle.value()));
        out.insert(QStringLiteral("width"), Value(size.width));
        out.insert(QStringLiteral("height"), Value(size.height));
        return Result<Values>(out, warnings);
    }
};

class CanvasWriteKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<CanvasHandle> base = canvas::canvasHandleFrom(context.input(QStringLiteral("canvas")).json());
        if (!base)
            return Result<Values>(withInput(base.errors(), QStringLiteral("canvas")));
        const Result<int> blend = intInput(context, "blend", 0, kMaxBlend);
        if (!blend)
            return Result<Values>(blend.errors());
        const Result<Pixels> redrawn = pixelsOf(context.input(QStringLiteral("image")), QStringLiteral("image"));
        if (!redrawn)
            return Result<Values>(redrawn.errors());
        QString name = context.input(QStringLiteral("name")).text().trimmed();
        if (name.isEmpty())
            name = QStringLiteral("Fill");

        Result<PaintDocument> opened = openRevision(base.value());
        if (!opened)
            return Result<Values>(opened.errors());
        PaintDocument &document = opened.value();
        const int width = document.width(), height = document.height();

        CropBox box{0, 0, width, height};
        Pixels mask(width, height, 1, 1.0f);
        std::optional<CropSize> shape;
        if (const Value stitch = context.input(QStringLiteral("stitch")); !stitch.isEmpty()) {
            const Result<StitchDescriptor> described = readStitchHandle(stitch, context.hashes);
            if (!described)
                return Result<Values>(described.errors());
            box = described->box;
            shape = described->size;
            if (box.x + box.width > width || box.y + box.height > height)
                return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("stitch"),
                                      QStringLiteral("The stitch's box lies outside the canvas (%1 x %2).").arg(width).arg(height));
            const Value own = context.input(QStringLiteral("mask"));
            Result<Pixels> read = maskOf(own.isMedia() ? own : Value(described->mask), own.isMedia() ? "mask" : "stitch");
            if (!read)
                return Result<Values>(read.errors());
            mask = maskResized(read.value(), width, height);
        } else if (const Value own = context.input(QStringLiteral("mask")); own.isMedia()) {
            const Result<Pixels> read = maskOf(own, QStringLiteral("mask"));
            if (!read)
                return Result<Values>(read.errors());
            mask = maskResized(read.value(), width, height);
        }
        if (shape && !shapeFits(*shape, redrawn->width, redrawn->height))
            return makeInputError(QStringLiteral("stitch_mismatch"), QStringLiteral("image"),
                                  QStringLiteral("The picture is %1 x %2 and the region was %3 x %4: its shape differs by more than 2%.")
                                      .arg(redrawn->width).arg(redrawn->height).arg(shape->width).arg(shape->height),
                                  QStringLiteral("Keep the Sampler on the region's latent, so the picture keeps its shape."));

        const Pixels layer = placedLayer(width, height, box, redrawn.value(), mask, blend.value());
        document.loadLayer(LayerKind::Paint, name, true, 1, pixelsToLayer(layer));

        // A candidate revision first, then the head, by compare-and-swap from the revision this
        // work started at (the store's rule, 14.5). Losing the race is a result, not a failure.
        CanvasStore store = CanvasStore::forSession();
        const Result<canvas::Revision> added =
            store.addRevision(base->canvas, document, base->revision, QStringLiteral("canvas-write:") + context.node);
        if (!added)
            return Result<Values>(added.errors());
        const bool keep = context.input(QStringLiteral("candidate")).json().toBool();
        // Kept for review: the head stays where it is and the revision waits as a candidate (accepted off, no warning).
        Result<canvas::HeadMove> moved = keep ? Result<canvas::HeadMove>(canvas::HeadMove{false, QString()}) : store.moveHead(base->canvas, base->revision, added->id);
        if (!moved)
            return Result<Values>(moved.errors());
        if (keep) {
            const Result<canvas::CanvasInfo> now = store.info(base->canvas);
            if (!now)
                return Result<Values>(now.errors());
            moved = canvas::HeadMove{false, now->head};
        }
        const Result<QJsonObject> handle = canvas::writeCanvasHandle(context.workDir, {base->canvas, added->id});
        if (!handle)
            return Result<Values>(handle.errors());

        Errors warnings;
        if (!moved->moved && !keep)
            warnings << warning(makeError(QStringLiteral("canvas_head_moved"),
                                          QStringLiteral("The canvas changed while this ran (its head is now %1), so the new layer was kept as a candidate revision, %2, and the head was not moved.")
                                              .arg(moved->head.left(19), added->id.left(19)),
                                          QStringLiteral("Run again from the canvas as it is now, or keep both.")));
        Values out;
        out.insert(QStringLiteral("canvas"), Value(handle.value()));
        out.insert(QStringLiteral("accepted"), Value(moved->moved));
        out.insert(QStringLiteral("head"), Value(moved->head));
        return Result<Values>(out, warnings);
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

void addCanvasKernels(Kernels &kernels) {
    kernels.add("fugue/canvas-region", std::make_shared<CanvasRegionKernel>());
    kernels.add("fugue/canvas-write", std::make_shared<CanvasWriteKernel>());
}

} // namespace fugue::nodes
