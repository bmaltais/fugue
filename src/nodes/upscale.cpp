// Upscale Video, Load Upscale Model and Upscale with Model (spec 14.1, #134). Upscale Video: a video made bigger by a factor with no model. The frames are
// never decoded here: ffmpeg's scale filter resamples them as it streams, so a long video is never
// in memory and the frame rate, the length and the sound are those of the input. The filter's
// flags are the same five methods as Resize's (ffmpeg's own kernels, not torch's: the pixels are
// not ComfyUI's, and a video has no ComfyUI reference). The result is H.264 at CRF 12 in Matroska,
// close enough to lossless that Save Video's own encode is the only visible one, and far smaller
// than FFV1 at the new size.

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <vips/vips8>

#include <algorithm>
#include <cmath>

#include "engine/atomic_file.h"
#include "engine/ffmpeg.h"
#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/model_library.h"
#include "engine/vips.h"
#include "nodes/kernels.h"

namespace fugue::nodes {

namespace {

// Resize's method names as libswscale's flags.
QString scaleFlag(const QString &method) {
    if (method == QLatin1String("nearest"))
        return QStringLiteral("neighbor");
    return method; // bilinear, bicubic, lanczos and area are its names too
}

// The size a side becomes: scaled, rounded down to even (4:2:0 needs it), at least 2.
int evenSide(int side, double scale) { return std::max(2, int(std::floor(side * scale / 2.0)) * 2); }

class UpscaleVideoKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        const MediaHandle input = context.input("video").media();
        const Result<MediaHandle> source = input.plainFile(context.workDir, QStringLiteral("source"), *context.hashes);
        if (!source)
            return source.errors();
        if (!QFileInfo(source->path()).isFile())
            return makeInputError(QStringLiteral("file_not_found"), QStringLiteral("video"),
                                  QStringLiteral("The video file is gone: %1.").arg(source->path()),
                                  QStringLiteral("Run the node that makes it again."));
        const double scale = context.input("scale").number();
        const QString method = context.input("method").text();
        const int width = evenSide(source->width(), scale), height = evenSide(source->height(), scale);
        if (width > 16384 || height > 16384)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("scale"),
                                  QStringLiteral("A scale of %1 makes a video of %2 x %3, above 16384.").arg(scale).arg(width).arg(height));

        const QString out = QDir(context.workDir).filePath(QStringLiteral("video.mkv"));
        const QStringList args{"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", source->path(),
                               "-map", "0:v:0", "-map", "0:a:0?",
                               "-vf", QStringLiteral("scale=%1:%2:flags=%3").arg(width).arg(height).arg(scaleFlag(method)),
                               "-c:v", "libx264", "-preset", "medium", "-crf", "12", "-pix_fmt", "yuv420p",
                               "-c:a", "copy", "-f", "matroska", out};
        const double duration = source->duration();
        const Result<QByteArray> done = ffmpeg::runWatched(
            args, QStringLiteral("node_failed"),
            [&](double seconds) {
                if (context.progress && duration > 0)
                    context.progress(std::min(1.0, seconds / duration), QStringLiteral("upscale"), QString());
            },
            context.cancelled);
        if (!done)
            return done.errors();
        const Result<MediaHandle> file = MediaHandle::probe(MediaType::Video, out, *context.hashes);
        if (!file)
            return file.errors();
        return Values{{"video", Value(file.value())}};
    }
};

// Load Upscale Model: the handle its picker holds, passed on.
// Two or four bands are gray or color with alpha, as the pixel nodes read them (pixel.cpp).
bool hasAlpha(const vips::VImage &image) { return image.bands() == 2 || image.bands() == 4; }

class LoadUpscaleModelKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        return Values{{QStringLiteral("model"), context.input(QStringLiteral("model"))}};
    }
};

// Upscale with Model: the helper's image.upscale (8.1) on the model's file. The helper takes RGB, so
// an alpha channel is kept here: resized to the result's size (lanczos) and put back, as ComfyUI's
// upscale nodes drop it and the library's own would only smooth it.
class UpscaleWithModelKernel : public ProviderKernel {
public:
    Requirement requirement(const NodeType &, const Values &) const override {
        Requirement requirement;
        requirement.capability = QStringLiteral("image.upscale"); // the helper offers it for any model file
        return requirement;
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        const QJsonObject model = context.input(QStringLiteral("model")).json().toObject();
        if (model.isEmpty())
            return makeInputError(QStringLiteral("missing_input"), QStringLiteral("model"),
                                  QStringLiteral("No upscale model is picked."),
                                  QStringLiteral("Pick one in Upscale with Model, or wire a Load Upscale Model."));
        QJsonValue modelPath;
        const ModelLibrary library = ModelLibrary::current(context.registry);
        if (selection.sharesFilesystem) {
            const Result<QString> found = library.resolve(model, *context.hashes);
            if (!found) {
                Error missing = found.error();
                missing.input = QStringLiteral("model");
                return missing;
            }
            modelPath = found.value();
        } else { // a paired host finds the file in its own folders (8.10)
            QJsonObject sent{{"name", model.value(QStringLiteral("name")).toString()}};
            if (const QString hash = model.value(QStringLiteral("hash")).toString(); !hash.isEmpty())
                sent.insert(QStringLiteral("sha256"), hash);
            if (const Result<QString> found = library.resolve(model, *context.hashes, ModelLibrary::Hashing::RememberedOnly))
                sent.insert(QStringLiteral("size"), double(QFileInfo(found.value()).size()));
            modelPath = sent;
        }
        const Result<QString> file = writeRgb(context.input(QStringLiteral("image")).media(),
                                              QDir(context.workDir).filePath(QStringLiteral("input.png")));
        if (!file)
            return file.errors();
        JobRequest request;
        request.capability = QStringLiteral("image.upscale");
        request.params = QJsonObject{{"context", QJsonObject{{"model_path", modelPath}}},
                                     {"tile_size", int(context.input(QStringLiteral("tile")).number())}};
        request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", file.value()}});
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        const QString path = result.outputs.value(QStringLiteral("image")).toObject().value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            return makeError(QStringLiteral("node_failed"), QStringLiteral("The helper finished without an image."));
        if (!initVips())
            return vipsUnavailable();
        try {
            vips::VImage upscaled = vips::VImage::new_from_file(QFile::encodeName(path).constData()).copy_memory();
            const Result<vips::VImage> source = context.input(QStringLiteral("image")).media().image();
            if (!source)
                return source.errors();
            if (const vips::VImage original = source.value(); hasAlpha(original)) {
                const vips::VImage alpha = original.extract_band(original.bands() - 1)
                                               .cast(VIPS_FORMAT_FLOAT)
                                               .resize(double(upscaled.width()) / original.width(),
                                                       vips::VImage::option()
                                                           ->set("vscale", double(upscaled.height()) / original.height())
                                                           ->set("kernel", VIPS_KERNEL_LANCZOS3));
                const double white = original.format() == VIPS_FORMAT_USHORT ? 65535.0 : 255.0;
                const vips::VImage scaled = (alpha * (255.0 / white)).linear(1.0, 0.5).floor().cast(VIPS_FORMAT_UCHAR);
                upscaled = upscaled.extract_band(0, vips::VImage::option()->set("n", 3)).bandjoin(scaled);
            }
            const Result<MediaHandle> image = MediaHandle::fromImage(upscaled);
            if (!image)
                return image.errors();
            return Values{{QStringLiteral("image"), Value(image.value())},
                          {QStringLiteral("width"), Value(double(upscaled.width()))},
                          {QStringLiteral("height"), Value(double(upscaled.height()))}};
        } catch (const vips::VError &error) {
            return vipsError(error, QStringLiteral("media_unreadable"));
        }
    }

private:
    // The image upright, in sRGB, without alpha, as an 8-bit PNG: what the helper reads.
    static Result<QString> writeRgb(const MediaHandle &image, const QString &path) {
        if (!initVips())
            return vipsUnavailable();
        try {
            const Result<vips::VImage> decoded = image.image();
            if (!decoded)
                return decoded.errors();
            vips::VImage rgb = decoded->autorot().colourspace(VIPS_INTERPRETATION_sRGB);
            if (hasAlpha(rgb))
                rgb = rgb.flatten();
            if (rgb.bands() > 3)
                rgb = rgb.extract_band(0, vips::VImage::option()->set("n", 3));
            rgb.cast(VIPS_FORMAT_UCHAR).write_to_file(QFile::encodeName(path).constData());
            return path;
        } catch (const vips::VError &error) {
            return vipsError(error, QStringLiteral("media_unreadable"));
        }
    }
};

} // namespace

void addUpscaleKernels(Kernels &kernels) {
    kernels.add("fugue/upscale-video", std::make_shared<UpscaleVideoKernel>());
    kernels.add("fugue/load-upscale-model", std::make_shared<LoadUpscaleModelKernel>());
    kernels.add("fugue/upscale-with-model", std::make_shared<UpscaleWithModelKernel>());
}

} // namespace fugue::nodes
