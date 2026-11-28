#include "nodes/kernels.h"

// Ticket #176: Crop to Mask and Stitch (spec 14.4) and the `fugue/stitch` handle between them. The
// arithmetic is crop_stitch_ops.h's, plain functions on float buffers; this file reads the inputs,
// carries the descriptor and writes the outputs.
//
// The handle is a data value (2.6) of the declared type `fugue/stitch`: a file in the node's
// folder that holds its canonical `meta` and nothing else, so its hash is that of the meta. The
// meta names the original picture and the mask as media handles (so it changes with either), the
// box the crop came from, and the crop's size.

#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <numeric>
#include <optional>

#include "engine/file_hashes.h"
#include "nodes/crop_stitch_ops.h"
#include "nodes/model_step.h"
#include "nodes/pixel_io.h"
#include "nodes/stitch_handle.h"

namespace fugue::nodes {

namespace {

// Crop to Mask's padding and Stitch's blend, as their manifests' `max`.
constexpr int kMaxPadding = 4096;
constexpr int kMaxBlend = 512;

Error warning(Error error) {
    error.severity = Error::Severity::Warning;
    return error;
}

class CropToMaskKernel : public NodeKernel {
public:
    // The size step is not an input: a model that resolves later is another size (#711).
    QString keyExtraFor(const NodeContext &context) const override { return sizeStepKey(context); }

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
        // The step the model aligns to (#711): the engine rounds a picture's sides to it, so a crop at another multiple
        // comes back from the redraw in another shape. A `multiple` that is not one of its multiples keeps both: their common multiple.
        const int modelStep = context.inputs.contains(QStringLiteral("model"))
                                  ? modelSizeStep(context, context.input("model").json().toObject())
                                  : kDefaultSizeStep;
        // A common multiple can pass kMaxCropSide (255 and 64 share 16320): then the smallest multiple of the model's step that holds `multiple`.
        const int common = modelStep > kDefaultSizeStep && multiple % modelStep != 0 ? std::lcm(multiple, modelStep) : multiple;
        const int stepped = common <= kMaxCropSide ? common : (multiple + modelStep - 1) / modelStep * modelStep;
        std::optional<Cropped> crop = cropToMask(image.value(), fitted, padding, megapixels, stepped);
        if (!crop)
            return makeInputError(QStringLiteral("mask_empty"), QStringLiteral("mask"),
                                  QStringLiteral("Nothing is painted in the mask."),
                                  QStringLiteral("Paint the part to change."));

        const Result<MediaHandle> original = namedHandle(imageValue.media(), context, QStringLiteral("original"));
        if (!original.ok())
            return Result<Values>(original.errors());
        const Result<MediaHandle> maskHandle = namedHandle(maskValue.media(), context, QStringLiteral("mask"));
        if (!maskHandle.ok())
            return Result<Values>(maskHandle.errors());
        const Result<Value> handle = writeStitchHandle(
            context.workDir, {original.value(), maskHandle.value(), crop->box, crop->size});
        if (!handle.ok())
            return Result<Values>(handle.errors());

        Result<Values> cropped = imageOutput(std::move(crop->image));
        Result<Values> croppedMask = namedOutput(std::move(crop->mask), "mask");
        if (!cropped.ok() || !croppedMask.ok())
            return !cropped.ok() ? Result<Values>(cropped.errors()) : Result<Values>(croppedMask.errors());
        Values out;
        out.insert(QStringLiteral("image"), cropped.value().value("image"));
        out.insert(QStringLiteral("mask"), croppedMask.value().value("mask"));
        out.insert(QStringLiteral("stitch"), handle.value());
        out.insert(QStringLiteral("width"), Value(crop->size.width));
        out.insert(QStringLiteral("height"), Value(crop->size.height));
        return Result<Values>(out, warnings);
    }
};

class StitchKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const Result<StitchDescriptor> described = readStitchHandle(context.input("stitch"), context.hashes);
        if (!described.ok())
            return Result<Values>(described.errors());
        const StitchDescriptor &d = described.value();
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
