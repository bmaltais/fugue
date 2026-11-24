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

#include <vips/vips8>

#include <algorithm>
#include <cmath>

#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/vips.h"
#include "nodes/kernels.h"
#include "nodes/pixel_io.h"
#include "nodes/restore_color.h"
#include "nodes/upscale_model.h"

namespace fugue::nodes {

namespace {

constexpr int kMaxSide = 16384; // as Resize's manifest `max`

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

class RestoreImageKernel : public ProviderKernel {
public:
    Requirement requirement(const NodeType &, const Values &) const override {
        Requirement requirement;
        requirement.capability = QStringLiteral("image.restore");
        return requirement;
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        const int tile = int(context.input(QStringLiteral("tile")).number());
        const int overlap = int(context.input(QStringLiteral("overlap")).number());
        if (overlap * 2 > tile)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("overlap"),
                                  QStringLiteral("An overlap of %1 is more than half the tile of %2.").arg(overlap).arg(tile),
                                  QStringLiteral("Use an overlap of at most half the tile size."));
        const Result<QJsonValue> model = modelFile(context, selection, QStringLiteral("model"),
                                                   QStringLiteral("No SeedVR2 model is picked."),
                                                   QStringLiteral("Pick one in the node (the catalog lists the downloads)."));
        if (!model)
            return model.errors();
        const Result<QJsonValue> vae = modelFile(context, selection, QStringLiteral("vae"),
                                                 QStringLiteral("No SeedVR2 VAE is picked."),
                                                 QStringLiteral("Pick ema_vae_fp16 in the node (the catalog lists the download)."));
        if (!vae)
            return vae.errors();

        const Result<Pixels> input = pixelsOf(context.input(QStringLiteral("image")), QStringLiteral("image"));
        if (!input)
            return input.errors();
        const double factor = context.input(QStringLiteral("factor")).number();
        const int width = std::max(1, int(std::nearbyint(input->width * factor)));
        const int height = std::max(1, int(std::nearbyint(input->height * factor)));
        if (width > kMaxSide || height > kMaxSide)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("factor"),
                                  QStringLiteral("A factor of %1 makes a picture of %2 x %3, above %4.").arg(factor).arg(width).arg(height).arg(kMaxSide));
        // The picture at its final size: what the model restores, and what the colour fix measures against.
        const Result<QString> file = writePng(resample(input.value(), width, height, ResizeMethod::Lanczos),
                                              QDir(context.workDir).filePath(QStringLiteral("input.png")));
        if (!file)
            return file.errors();

        JobRequest request;
        request.capability = QStringLiteral("image.restore");
        request.params = QJsonObject{{"context", QJsonObject{{"model_path", model.value()}, {"vae_path", vae.value()}}},
                                     {"tile_size", tile},
                                     {"overlap", overlap},
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
