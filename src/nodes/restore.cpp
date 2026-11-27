// Restore Image (#346, spec 14.1): SeedVR2 on the helper's image.restore. One node, not ComfyUI's
// three (a scale, a preprocess, a conditioning, a postprocess around a sampler): the values between
// them are half-finished pictures and padded latents that mean nothing alone (3.7).
//
// The node makes the picture the size it should end at (a lanczos resize by `factor`, which is also
// the reference of the colour fix), the helper pads it, encodes it in tiles, takes one step and
// decodes it in tiles (helper/README.md), and the colour fix (nodes/restore_color.h) puts the
// input's colours and low frequencies back on the restored detail.

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <vips/vips8>

#include <algorithm>
#include <cmath>
#include <optional>

#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/model_library.h"
#include "engine/model_shape.h"
#include "engine/vips.h"
#include "../../helper/src/restore.h" // the limits, one source for the node and the helper
#include "nodes/kernels.h"
#include "nodes/pixel_io.h"
#include "nodes/restore_color.h"
#include "nodes/upscale_model.h"

namespace fugue::nodes {

namespace {

// 8-bit PNG of float pixels (with alpha when it has it), what the helper reads and the fix reads back.
Result<QString> writePng(const Pixels &pixels, const QString &path) {
    if (!initVips())
        return vipsUnavailable();
    try {
        const vips::VImage image = vips::VImage::new_from_memory(const_cast<float *>(pixels.data.data()),
                                                                  pixels.data.size() * sizeof(float), pixels.width,
                                                                  pixels.height, pixels.channels, VIPS_FORMAT_FLOAT);
        (image * 255.0).linear(1.0, 0.5).floor().cast(VIPS_FORMAT_UCHAR)
            .write_to_file(QFile::encodeName(path).constData());
        return path;
    } catch (const vips::VError &error) {
        return vipsError(error, QStringLiteral("media_unreadable"));
    }
}

Result<Pixels> readPng(const QString &path, FileHashes &hashes) {
    const Result<MediaHandle> handle = MediaHandle::probe(MediaType::Image, path, hashes);
    if (!handle)
        return handle.errors();
    return pixelsOf(Value(handle.value()), QStringLiteral("image"));
}

// Whether a picture of `inWidth` x `inHeight` at `factor` is one the model can restore on a 24 GB card
// (helper/src/restore.h has the limits, the helper checks them again), else the error saying what fits.
std::optional<Error> checkSize(double inWidth, double inHeight, double factor) {
    const double width = std::nearbyint(inWidth * factor), height = std::nearbyint(inHeight * factor);
    const double minimum = double(fugue_helper::kMinRestoreSide);
    if (width < minimum || height < minimum)
        return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("image"),
                              QStringLiteral("The image is %1 x %2, which at a factor of %3 is %4 x %5: too small. SeedVR2 needs at least %6 pixels on each side of the result, so at least %7 x %7 here.")
                                  .arg(inWidth).arg(inHeight).arg(factor).arg(width).arg(height).arg(fugue_helper::kMinRestoreSide)
                                  .arg(std::ceil(minimum / factor)),
                              QStringLiteral("Use a bigger image or a bigger factor."));
    const double limit = double(fugue_helper::kMaxRestorePixels);
    if (width > fugue_helper::kMaxRestoreSide || height > fugue_helper::kMaxRestoreSide || width * height > limit) {
        const double largestFactor = std::floor(std::sqrt(limit / (inWidth * inHeight)));
        const double fits = limit / (factor * factor);
        const QString advice = largestFactor >= 1 ? QStringLiteral("The largest factor for this image is %1.").arg(largestFactor)
                                                  : QStringLiteral("This image is too big even at a factor of 1: make it smaller first.");
        return makeInputError(
            QStringLiteral("value_out_of_range"), largestFactor >= 1 ? QStringLiteral("factor") : QStringLiteral("image"),
            QStringLiteral("A factor of %1 makes a picture of %2 x %3 (%4 MP). A 24 GB card restores at most %5 MP (4098 x 4098); at this factor the image can be up to %6 MP (about %7 x %7). %8")
                .arg(factor).arg(width).arg(height).arg(width * height / 1e6, 0, 'f', 1).arg(limit / 1e6, 0, 'f', 1)
                .arg(fits / 1e6, 0, 'f', 1).arg(std::floor(std::sqrt(fits))).arg(advice));
    }
    return std::nullopt;
}

// The two model files by their tensor names (engine/model_shape.h): a file of another model, or a SeedVR2 one
// that keeps its embeddings elsewhere (the GGUF, the numz fp8), is said here and not after 8 GB are read.
std::optional<Error> checkFiles(const QString &modelPath, const QString &vaePath) {
    const auto unsupported = [](const QString &input, const QString &path, const QString &why) {
        Error error = makeError(QStringLiteral("model_unsupported"), QStringLiteral("%1: %2.").arg(QFileInfo(path).fileName(), why),
                                QStringLiteral("Use the Comfy-Org INT8 ConvRot checkpoint and ema_vae_fp16 (the catalog lists both)."));
        error.input = input;
        return error;
    };
    ModelShapes &shapes = ModelShapes::shared();
    const std::optional<ModelShape> model = shapes.of(modelPath);
    if (!model)
        return unsupported(QStringLiteral("model"), modelPath, QStringLiteral("cannot read its tensor names (a .safetensors file is needed)"));
    if (!model->seedvr2Model)
        return unsupported(QStringLiteral("model"), modelPath, QStringLiteral("this is not a SeedVR2 diffusion model"));
    if (!model->seedvr2StoredEmbeddings)
        return unsupported(QStringLiteral("model"), modelPath,
                           QStringLiteral("this SeedVR2 file does not carry its stored embeddings (positive_conditioning), which the engine needs"));
    const std::optional<ModelShape> vae = shapes.of(vaePath);
    if (!vae || !vae->seedvr2Vae)
        return unsupported(QStringLiteral("vae"), vaePath, QStringLiteral("this is not the SeedVR2 VAE"));
    return std::nullopt;
}

// An overlap the tile cannot hold: the one rule of the pair that a range on each cannot say.
std::optional<Error> overlapProblem(double overlap, double tile) {
    if (std::isfinite(overlap) && overlap >= 0 && overlap * 2 <= tile)
        return std::nullopt;
    return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("overlap"),
                          QStringLiteral("An overlap of %1 is not between 0 and half the tile of %2.").arg(overlap).arg(tile),
                          QStringLiteral("Use an overlap of at most half the tile size."));
}

class RestoreImageKernel : public ProviderKernel {
public:
    // Under validate: the overlap fits the tile, and the model and VAE picked are SeedVR2 files by their headers
    // (read once, as any check of a model's kind does; a file not on this machine is the scheduler's model_missing).
    Errors check(const NodeContext &context) const override {
        if (context.inputs.contains(QStringLiteral("tile")) && context.inputs.contains(QStringLiteral("overlap")))
            if (const std::optional<Error> bad = overlapProblem(context.input(QStringLiteral("overlap")).number(),
                                                                context.input(QStringLiteral("tile")).number()))
                return {*bad};
        if (!context.hashes || !context.inputs.contains(QStringLiteral("model")) || !context.inputs.contains(QStringLiteral("vae")))
            return {};
        std::optional<ModelLibrary> own; // none is given outside a check pass
        const ModelLibrary &library = context.library ? *context.library : own.emplace(ModelLibrary::current(context.registry));
        // Only a file known to be the one picked (its remembered hash is the handle's): a file of the same name with no known
        // hash may be a different variant than the one a paired host would use, so nothing is said of it (#607).
        const auto path = [&](const char *input) {
            const QJsonObject handle = context.input(QLatin1String(input)).json().toObject();
            const Result<QString> found = library.resolve(handle, *context.hashes, ModelLibrary::Hashing::RememberedOnly);
            return found && context.hashes->known(found.value()) == handle.value(QStringLiteral("hash")).toString() ? found.value() : QString();
        };
        const QString model = path("model"), vae = path("vae");
        if (model.isEmpty() || vae.isEmpty())
            return {};
        const std::optional<Error> bad = checkFiles(model, vae);
        return bad ? Errors{*bad} : Errors{};
    }

    Requirement requirement(const NodeType &, const Values &) const override {
        Requirement requirement;
        requirement.capability = QStringLiteral("image.restore");
        return requirement;
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        const double factor = context.input(QStringLiteral("factor")).number();
        const double tile = context.input(QStringLiteral("tile")).number();
        const double overlap = context.input(QStringLiteral("overlap")).number();
        // A wired number can be anything: checked as a double, before any conversion to an int.
        if (!std::isfinite(factor) || factor < 1 || factor > 8)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("factor"),
                                  QStringLiteral("A factor of %1 is outside 1 to 8.").arg(factor));
        if (!std::isfinite(tile) || tile < fugue_helper::kMinTile || tile > fugue_helper::kMaxTile)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("tile"),
                                  QStringLiteral("A tile of %1 is outside %2 to %3.").arg(tile).arg(fugue_helper::kMinTile).arg(fugue_helper::kMaxTile));
        if (const std::optional<Error> bad = overlapProblem(overlap, tile))
            return *bad;

        // The size, from the image's header (nothing is decoded or resampled before it is known to fit).
        const Result<vips::VImage> header = context.input(QStringLiteral("image")).isMedia()
                                                ? context.input(QStringLiteral("image")).media().image()
                                                : Result<vips::VImage>(makeInputError(QStringLiteral("missing_input"), QStringLiteral("image"),
                                                                                      QStringLiteral("Input image has no image.")));
        if (!header)
            return header.errors();
        const double inWidth = header->width(), inHeight = header->height();
        const auto sizeError = checkSize(inWidth, inHeight, factor);
        if (sizeError)
            return *sizeError;
        const int width = int(std::nearbyint(inWidth * factor)), height = int(std::nearbyint(inHeight * factor));

        const Result<QJsonValue> model = modelFile(context, selection, QStringLiteral("model"),
                                                   QStringLiteral("No SeedVR2 model is picked."),
                                                   QStringLiteral("Pick one in the node (the catalog lists the download)."));
        if (!model)
            return model.errors();
        const Result<QJsonValue> vae = modelFile(context, selection, QStringLiteral("vae"),
                                                 QStringLiteral("No SeedVR2 VAE is picked."),
                                                 QStringLiteral("Pick ema_vae_fp16 in the node (the catalog lists the download)."));
        if (!vae)
            return vae.errors();
        if (selection.sharesFilesystem) { // a paired host's files are checked by its helper
            if (const auto bad = checkFiles(model.value().toString(), vae.value().toString()))
                return *bad;
        }

        const Result<Pixels> input = pixelsOf(context.input(QStringLiteral("image")), QStringLiteral("image"));
        if (!input)
            return input.errors();
        // The picture at its final size: what the model restores, and what the colour fix measures against.
        const Result<QString> file = writePng(resample(input.value(), width, height, ResizeMethod::Lanczos),
                                              QDir(context.workDir).filePath(QStringLiteral("input.png")));
        if (!file)
            return file.errors();

        JobRequest request;
        request.capability = QStringLiteral("image.restore");
        request.params = QJsonObject{{"context", QJsonObject{{"model_path", model.value()}, {"vae_path", vae.value()}}},
                                     {"tile_size", int(tile)},
                                     {"overlap", int(overlap)},
                                     {"seed", context.input(QStringLiteral("seed")).json().toInteger()}};
        request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", file.value()}});
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        const QString path = result.outputs.value(QStringLiteral("image")).toObject().value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            return makeError(QStringLiteral("node_failed"), QStringLiteral("The helper finished without an image."));
        const Result<Pixels> restored = readPng(path, *context.hashes);
        if (!restored)
            return restored.errors();
        const Result<Pixels> reference = readPng(QDir(context.workDir).filePath(QStringLiteral("input.png")), *context.hashes);
        if (!reference)
            return reference.errors();
        const QString fix = context.input(QStringLiteral("color_fix")).text();
        Pixels out = fix == QLatin1String("lab")       ? labColorFix(restored.value(), reference.value())
                     : fix == QLatin1String("wavelet") ? waveletReconstruction(restored.value(), reference.value())
                                                       : restored.value();
        // The input's alpha, at the new size, goes back on (the model sees colour only).
        if (reference->channels == 4 && reference->width == out.width && reference->height == out.height) {
            Pixels withAlpha(out.width, out.height, 4);
            for (int y = 0; y < out.height; ++y)
                for (int x = 0; x < out.width; ++x) {
                    for (int c = 0; c < 3; ++c)
                        withAlpha.at(x, y, c) = out.at(x, y, c);
                    withAlpha.at(x, y, 3) = reference->at(x, y, 3);
                }
            out = std::move(withAlpha);
        }
        const int width = out.width, height = out.height;
        Result<Values> image = imageOutput(std::move(out));
        if (!image)
            return image;
        Values values = image.value();
        values.insert(QStringLiteral("width"), Value(double(width)));
        values.insert(QStringLiteral("height"), Value(double(height)));
        return values;
    }
};

} // namespace

void addRestoreKernels(Kernels &kernels) {
    kernels.add("fugue/restore-image", std::make_shared<RestoreImageKernel>());
}

} // namespace fugue::nodes
