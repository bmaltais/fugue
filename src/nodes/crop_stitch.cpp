#include "nodes/kernels.h"

// Ticket #176: Crop to Mask and Stitch (spec 14.4) and the `fugue/stitch` handle between them. The
// arithmetic is crop_stitch_ops.h's, plain functions on float buffers; this file reads the inputs,
// carries the descriptor and writes the outputs.
//
// The handle is a data value (2.6) of the declared type `fugue/stitch`: a file in the node's
// folder that holds its canonical `meta` and nothing else, so its hash is that of the meta. The
// meta names the original picture and the mask as media handles (so it changes with either), the
// box the crop came from, and the crop's size.

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <optional>

#include "engine/file_hashes.h"
#include "nodes/crop_stitch_ops.h"
#include "nodes/pixel_io.h"

namespace fugue::nodes {

namespace {

const QString kStitchType = QStringLiteral("fugue/stitch");

// Crop to Mask's padding and Stitch's blend, as their manifests' `max`.
constexpr int kMaxPadding = 4096;
constexpr int kMaxBlend = 512;

Error warning(Error error) {
    error.severity = Error::Severity::Warning;
    return error;
}

// A handle that has a file and a hash, for naming in the descriptor: an input from the scheduler
// has both already.
Result<MediaHandle> named(const MediaHandle &handle, const NodeContext &context, const QString &name) {
    if (handle.hasFile() && !handle.hash().isEmpty())
        return handle;
    if (!context.hashes)
        return makeError(QStringLiteral("node_failed"), QStringLiteral("The %1 has no file to name.").arg(name));
    return handle.saved(context.workDir, name, *context.hashes);
}

class CropToMaskKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Value imageValue = context.input("image");
        Result<Pixels> image = pixelsOf(imageValue, "image");
        if (!image.ok())
            return Result<Values>(image.errors());
        const Value maskValue = context.input("mask");
        Result<Pixels> mask = maskOf(maskValue, "mask");
        if (!mask.ok())
            return Result<Values>(mask.errors());
        const int padding = toInt(context.input("padding"));
        const double megapixels = context.input("megapixels").number();
        const int multiple = toInt(context.input("multiple"));
        if (padding < 0 || padding > kMaxPadding)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("padding"),
                                  QStringLiteral("A padding of %1 is outside 0 to %2.").arg(padding).arg(kMaxPadding));
        if (!(megapixels >= 0.0) || megapixels > 16.0)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("megapixels"),
                                  QStringLiteral("%1 megapixels is outside 0 to 16.").arg(megapixels));
        if (multiple < 1 || multiple > 256)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("multiple"),
                                  QStringLiteral("A multiple of %1 is outside 1 to 256.").arg(multiple));

        Errors warnings;
        Pixels fitted = std::move(mask.value());
        if (fitted.width != image->width || fitted.height != image->height) {
            warnings << warning(makeInputError(QStringLiteral("mask_resized"), QStringLiteral("mask"),
                                               QStringLiteral("The mask is %1 x %2 and the picture %3 x %4; the mask was scaled to the picture.")
                                                   .arg(fitted.width).arg(fitted.height).arg(image->width).arg(image->height)));
            fitted = maskResized(fitted, image->width, image->height);
        }
        std::optional<Cropped> crop = cropToMask(image.value(), fitted, padding, megapixels, multiple);
        if (!crop)
            return makeInputError(QStringLiteral("mask_empty"), QStringLiteral("mask"),
                                  QStringLiteral("Nothing is painted in the mask."),
                                  QStringLiteral("Paint the part to change."));

        const Result<MediaHandle> original = named(imageValue.media(), context, QStringLiteral("original"));
        if (!original.ok())
            return Result<Values>(original.errors());
        const Result<MediaHandle> maskHandle = named(maskValue.media(), context, QStringLiteral("mask"));
        if (!maskHandle.ok())
            return Result<Values>(maskHandle.errors());
        const QJsonObject meta{
            {"image", original->toJson()},
            {"mask", maskHandle->toJson()},
            {"box", QJsonObject{{"x", crop->box.x}, {"y", crop->box.y}, {"width", crop->box.width}, {"height", crop->box.height}}},
            {"size", QJsonObject{{"width", crop->size.width}, {"height", crop->size.height}}}};
        const QByteArray bytes = QJsonDocument(meta).toJson(QJsonDocument::Compact);
        const QString path = QDir(context.workDir).filePath(QStringLiteral("stitch.json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
            return makeError(QStringLiteral("node_failed"), QStringLiteral("Cannot write %1.").arg(path));

        Result<Values> cropped = imageOutput(std::move(crop->image));
        Result<Values> croppedMask = namedOutput(std::move(crop->mask), "mask");
        if (!cropped.ok() || !croppedMask.ok())
            return !cropped.ok() ? Result<Values>(cropped.errors()) : Result<Values>(croppedMask.errors());
        Values out;
        out.insert(QStringLiteral("image"), cropped.value().value("image"));
        out.insert(QStringLiteral("mask"), croppedMask.value().value("mask"));
        out.insert(QStringLiteral("stitch"), Value(QJsonObject{{"data", kStitchType}, {"path", path}, {"meta", meta}}));
        out.insert(QStringLiteral("width"), Value(crop->size.width));
        out.insert(QStringLiteral("height"), Value(crop->size.height));
        return Result<Values>(out, warnings);
    }
};

// What a stitch handle names: the picture and mask, each of which must still be the file Crop to
// Mask read; where the crop came from and its size.
struct Descriptor {
    MediaHandle image;
    MediaHandle mask;
    CropBox box;
    CropSize size;
};

Result<Descriptor> descriptorOf(const Value &stitch, FileHashes *hashes) {
    const QJsonObject meta = stitch.json().toObject().value(QStringLiteral("meta")).toObject();
    const auto bad = [] {
        return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("stitch"),
                              QStringLiteral("This is not the stitch handle of a Crop to Mask."),
                              QStringLiteral("Wire Crop to Mask's stitch output to this input."));
    };
    if (stitch.json().toObject().value(QStringLiteral("data")).toString() != kStitchType)
        return bad();
    Descriptor d;
    const Result<MediaHandle> image = MediaHandle::fromJson(meta.value(QStringLiteral("image")).toObject());
    const Result<MediaHandle> mask = MediaHandle::fromJson(meta.value(QStringLiteral("mask")).toObject());
    if (!image || !mask)
        return bad();
    d.image = image.value();
    d.mask = mask.value();
    const QJsonObject box = meta.value(QStringLiteral("box")).toObject();
    const QJsonObject size = meta.value(QStringLiteral("size")).toObject();
    d.box = {box.value("x").toInt(), box.value("y").toInt(), box.value("width").toInt(), box.value("height").toInt()};
    d.size = {size.value("width").toInt(), size.value("height").toInt()};
    if (d.box.width <= 0 || d.box.height <= 0 || d.size.width <= 0 || d.size.height <= 0 || d.box.x < 0 || d.box.y < 0)
        return bad();
    if (hashes)
        for (const MediaHandle &file : {d.image, d.mask})
            if (hashes->hash(file.path()) != file.hash())
                return makeInputError(QStringLiteral("file_not_found"), QStringLiteral("stitch"),
                                      QStringLiteral("The picture or mask %1 is gone or changed since Crop to Mask read it.").arg(file.path()),
                                      QStringLiteral("Run Crop to Mask again."));
    return d;
}

class StitchKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<Descriptor> described = descriptorOf(context.input("stitch"), context.hashes);
        if (!described.ok())
            return Result<Values>(described.errors());
        const Descriptor &d = described.value();
        const int blend = toInt(context.input("blend"));
        if (blend < 0 || blend > kMaxBlend)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("blend"),
                                  QStringLiteral("A blend of %1 is outside 0 to %2.").arg(blend).arg(kMaxBlend));

        const Result<Pixels> redrawn = pixelsOf(context.input("image"), "image");
        if (!redrawn.ok())
            return Result<Values>(redrawn.errors());
        if (!shapeFits(d.size, redrawn->width, redrawn->height))
            return makeInputError(QStringLiteral("stitch_mismatch"), QStringLiteral("image"),
                                  QStringLiteral("The picture is %1 x %2 and the crop was %3 x %4: its shape differs by more than 2%.")
                                      .arg(redrawn->width).arg(redrawn->height).arg(d.size.width).arg(d.size.height),
                                  QStringLiteral("Keep the Sampler on the crop's latent, so the picture keeps the crop's shape."));

        const Result<Pixels> original = pixelsOf(Value(d.image), "stitch");
        if (!original.ok())
            return Result<Values>(original.errors());
        if (d.box.x + d.box.width > original->width || d.box.y + d.box.height > original->height)
            return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("stitch"),
                                  QStringLiteral("The box lies outside the picture it came from."));
        // The handle's mask, or this node's own, at the picture's size.
        const Value override = context.input("mask");
        Result<Pixels> mask = override.isMedia() ? maskOf(override, "mask") : maskOf(Value(d.mask), "stitch");
        if (!mask.ok())
            return Result<Values>(mask.errors());
        const Pixels fitted = maskResized(mask.value(), original->width, original->height);
        return imageOutput(stitch(original.value(), fitted, d.box, redrawn.value(), blend));
    }
};

} // namespace

void addCropStitchKernels(Kernels &kernels) {
    kernels.add("fugue/crop-to-mask", std::make_shared<CropToMaskKernel>());
    kernels.add("fugue/stitch", std::make_shared<StitchKernel>());
}

} // namespace fugue::nodes
