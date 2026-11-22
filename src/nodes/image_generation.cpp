// Generate Image and Edit Image (spec 14.1): the coarse image nodes, served by a provider's
// image.generate and image.edit (8.1). The kernels only translate (node_kernel.h): inputs to a
// JobRequest, and the JobResult to outputs with the values actually used (3.3).

#include "nodes/image_cover.h"
#include "nodes/image_size.h"
#include "nodes/kernels.h"
#include "nodes/mask_ops.h"
#include "nodes/pixel_io.h"

#include <QDir>
#include <QJsonArray>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <optional>

#include <vips/vips8>

#include "engine/media.h"
#include "engine/vips.h"

namespace fugue::nodes {

namespace {

const QString kGenerate = QStringLiteral("image.generate");
const QString kEdit = QStringLiteral("image.edit");

// Fantasi's choices (generator.h, generator.cpp at 6aa8dc2), kept as Fugue's starting ones.
constexpr int kSizeMultiple = 64;          // image sizes go in steps of 64
// Edit Image's references: Fugue's starting four (14.1), when the model reports no
// `max_reference_images`; a reported limit holds, higher or lower.
constexpr int kReferences = 4;

// The size nearest a megapixel in the shape width x height, in steps of 64 (fitMegapixel), as
// Image Size makes it.
QSize fitMegapixel(int width, int height) { return sizeForMegapixels(double(width) / height, 1.0, kSizeMultiple); }

// A side in the shape of `aspect` (width / height) next to a given one, in steps of 64.
int alongside(double length, double aspect) {
    return std::max(1, int(std::lround(length * aspect / kSizeMultiple))) * kSizeMultiple;
}

// Whether a picture goes to the model as it is (prepareBase): a PNG standing upright, in steps
// of 64 and near a megapixel (0.4 to 1.6).
bool worksAsItIs(const MediaHandle &image) {
    if (!image.hasFile() || image.format() != QLatin1String("png"))
        return false;
    const double area = double(image.width()) * image.height();
    if (image.width() % kSizeMultiple != 0 || image.height() % kSizeMultiple != 0 || area > 1.6 * kMegapixel ||
        area < 0.4 * kMegapixel)
        return false;
    vips::VImage header = vips::VImage::new_from_file(QFile::encodeName(image.path()).constData());
    return header.get_typeof("orientation") == 0 || header.get_int("orientation") == 1;
}

// The size a picture goes to the model at when nothing else sets it.
QSize ownSize(const MediaHandle &image) {
    return worksAsItIs(image) ? QSize(image.width(), image.height()) : fitMegapixel(image.width(), image.height());
}

// The picture at exactly `size`: as it is when it already is, else scaled to cover the size,
// cut from the middle and written as an 8-bit RGB PNG to `path` (prepareBase).
Result<QString> prepared(const MediaHandle &image, QSize size, const QString &path) {
    if (image.width() == size.width() && image.height() == size.height() && worksAsItIs(image))
        return image.path();
    return coverToFile(image, size, path);
}

// What a mask holds: nothing painted, or all of it (the checks of 14.4).
struct MaskRange {
    bool empty = false;
    bool full = false;
};
MaskRange rangeOf(const Pixels &mask) {
    float least = 1.0f;
    float most = 0.0f;
    for (const float v : mask.data) {
        least = std::min(least, v);
        most = std::max(most, v);
    }
    return MaskRange{most <= 0.0f, least >= 0.999f};
}

// Whether the model's `inputs` property lists the mask (8.1).
bool takesMask(const Selection &selection) {
    return selection.model.properties.value(QStringLiteral("inputs")).toArray().contains(QStringLiteral("mask"));
}

class ImageKernel : public ProviderKernel {
public:
    explicit ImageKernel(QString capability) : m_capability(std::move(capability)) {}

    // No properties: any model of the capability serves (image.edit already means an edit
    // model), and inputs that are not known yet under validate change nothing here.
    Requirement requirement(const NodeType &, const Values &inputs) const override {
        Requirement requirement;
        requirement.capability = m_capability;
        requirement.model = inputs.value(QStringLiteral("model")).json().toObject();
        // A mask is a preference, not a need: with "any ready model" one that takes a mask wins, and
        // a model that cannot is refused in request() with mask_unsupported, naming the others.
        if (!editing() && inputs.value(QStringLiteral("mask")).isMedia())
            requirement.preferred = QJsonObject{{"inputs", QJsonArray{"mask"}}};
        return requirement;
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        // Before anything reads an image header: an input from the result cache was never
        // probed, so this may be the process's first use of libvips.
        if (!initVips())
            return vipsUnavailable();
        JobRequest request;
        request.capability = m_capability;
        request.model = selection.model.handle;
        QJsonObject &params = request.params;
        params.insert(QStringLiteral("prompt"), context.input(promptInput()).text());
        const QString negative = context.input(QStringLiteral("negative_prompt")).text();
        if (!negative.isEmpty())
            params.insert(QStringLiteral("negative_prompt"), negative);
        const Value seed = context.input(QStringLiteral("seed"));
        if (!seed.isEmpty())
            params.insert(QStringLiteral("seed"), seed.json().toInteger());

        // The pictures, and the size they set: the first reference's shape, or the start
        // image fitted to what the node sets.
        Result<std::optional<QSize>> size = std::optional<QSize>();
        try {
            size = editing() ? references(context, selection, request) : startImage(context, request);
        } catch (const vips::VError &error) {
            Error unreadable = vipsError(error, QStringLiteral("media_unreadable"));
            unreadable.message = QStringLiteral("Cannot prepare an input image: %1").arg(unreadable.message);
            return unreadable;
        }
        if (!size)
            return size.errors();
        if (!editing()) {
            const Result<Nothing> masked = maskInput(context, selection, *size, request);
            if (!masked)
                return masked.errors();
        }
        if (*size) {
            params.insert(QStringLiteral("width"), (*size)->width());
            params.insert(QStringLiteral("height"), (*size)->height());
        }

        // The model's defaults, with what the node sets on top (Fantasi's generate()). Sent
        // explicitly, so the values used come back whatever the server would pick.
        const QJsonObject &defaults = selection.model.defaults;
        for (const QString &key : {QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("steps"),
                                   QStringLiteral("guidance")}) {
            const Value set = context.input(key);
            if (params.contains(key))
                continue;
            const bool size = key == QLatin1String("width") || key == QLatin1String("height");
            if (!set.isEmpty())
                params.insert(key, set.json());
            else if (defaults.contains(key) && (!size || defaults.value(key).toDouble() > 0))
                params.insert(key, defaults.value(key)); // a size of 0 is no default: the server's own
        }
        if (const Errors outside = outsideLimits(context, selection, params); hasErrors(outside))
            return outside;
        return request;
    }

    // The values used are the provider's report (8.1); what it leaves out is what was asked
    // for: the seed input, the model chosen, and the image's own size.
    Result<Values> outputs(const NodeContext &context, const Selection &selection, const JobResult &result) override {
        const QString path = result.outputs.value(QStringLiteral("image")).toObject().value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 finished without an image.").arg(modelName(selection)));
        const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, path, *context.hashes);
        if (!image)
            return image.errors();

        const QJsonObject &used = result.used;
        const auto number = [&used](const QString &key, const Value &otherwise) {
            const QJsonValue value = used.value(key);
            return value.isDouble() ? Value(value) : otherwise;
        };
        const QJsonObject model = used.value(QStringLiteral("model")).toObject();
        // With a mask the picture is put back everywhere the mask is not (14.4): the model redraws the
        // blocks of its latent that the mask touches, and every pixel passes through its VAE.
        Value drawn(image.value());
        Errors warnings;
        if (!editing() && context.input(QStringLiteral("mask")).isMedia() && context.input(QStringLiteral("image")).isMedia()) {
            const Result<Value> composited = putBack(context, image.value(), warnings);
            if (!composited)
                return composited.errors();
            drawn = composited.value();
        }
        const Values values{{QStringLiteral("image"), drawn},
                      {QStringLiteral("seed"), number(QStringLiteral("seed"), context.input(QStringLiteral("seed")))},
                      {QStringLiteral("width"), number(QStringLiteral("width"), Value(image->width()))},
                      {QStringLiteral("height"), number(QStringLiteral("height"), Value(image->height()))},
                      {QStringLiteral("model"), Value(model.isEmpty() ? selection.model.handle : model)}};
        return warnings.isEmpty() ? Result<Values>(values) : Result<Values>(values, warnings);
    }

private:
    // Edit Image's references, each prepared on its own (prepareBase), into request.inputs.
    Result<std::optional<QSize>> references(const NodeContext &context, const Selection &selection,
                                            JobRequest &request) const {
        const QList<Value> images = context.input(QStringLiteral("images")).items();
        const int reported = selection.model.limits.value("max_reference_images").toInt(0);
        const int most = reported > 0 ? reported : kReferences;
        if (images.size() > most) {
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("images"),
                                  QStringLiteral("%1 takes at most %2 images, not %3.")
                                      .arg(modelName(selection))
                                      .arg(most)
                                      .arg(images.size()),
                                  QStringLiteral("Remove images, or use a model that takes more."));
        }
        std::optional<QSize> size;
        QJsonArray sent;
        for (qsizetype i = 0; i < images.size(); ++i) {
            const MediaHandle image = images.at(i).media();
            const QSize own = ownSize(image);
            if (!size)
                size = own;
            const Result<QString> path =
                prepared(image, own, QDir(context.workDir).filePath(QStringLiteral("reference-%1.png").arg(i + 1)));
            if (!path)
                return path.errors();
            sent.append(QJsonObject{{"path", path.value()}});
        }
        request.inputs.insert(QStringLiteral("images"), sent);
        return size;
    }

    // Generate Image's start image, fitted to the size, into request.inputs with its strength.
    // A side the node leaves unset follows the image's shape.
    Result<std::optional<QSize>> startImage(const NodeContext &context, JobRequest &request) const {
        if (!context.inputs.contains(QStringLiteral("image")))
            return std::optional<QSize>();
        const MediaHandle image = context.input(QStringLiteral("image")).media();
        const double aspect = double(image.width()) / image.height();
        const bool width = context.inputs.contains(QStringLiteral("width"));
        const bool height = context.inputs.contains(QStringLiteral("height"));
        const int w = int(context.input(QStringLiteral("width")).number());
        const int h = int(context.input(QStringLiteral("height")).number());
        const QSize size = width && height ? QSize(w, h)
                           : width         ? QSize(w, alongside(w, 1 / aspect))
                           : height        ? QSize(alongside(h, aspect), h)
                                           : ownSize(image);
        const Result<QString> path = prepared(image, size, QDir(context.workDir).filePath(QStringLiteral("start.png")));
        if (!path)
            return path.errors();
        request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", path.value()}});
        request.params.insert(QStringLiteral("strength"), context.input(QStringLiteral("strength")).number());
        return std::optional<QSize>(size);
    }

    // Generate Image's mask (14.4), into request.inputs beside the start image: refused unless the
    // model takes one, or when nothing is painted, and cut to the start image's size the way the image
    // is. `size` is the start image's size, unset without one.
    Result<Nothing> maskInput(const NodeContext &context, const Selection &selection, const std::optional<QSize> &size,
                              JobRequest &request) const {
        const Value mask = context.input(QStringLiteral("mask"));
        if (!mask.isMedia())
            return Nothing{};
        if (!size)
            return makeInputError(QStringLiteral("missing_input"), QStringLiteral("image"),
                                  QStringLiteral("A mask needs a start image to redraw."),
                                  QStringLiteral("Connect the picture the mask was made on to Start image."));
        if (!takesMask(selection)) {
            Error error = makeInputError(
                QStringLiteral("mask_unsupported"), QStringLiteral("mask"),
                QStringLiteral("%1 on %2 cannot take a mask; it would redraw the whole picture.")
                    .arg(modelName(selection), selection.instance),
                selection.preferredOffers.isEmpty()
                    ? QStringLiteral("No provider here has a model that takes a mask. Use an Omarchy model whose "
                                     "sd-server lists mask_image, or redraw the part with VAE Encode (mask) and a "
                                     "Sampler on the Fugue helper.")
                    : QStringLiteral("Pick a model that takes one: %1.").arg(selection.preferredOffers.join(QStringLiteral(", "))));
            for (const QString &offer : selection.preferredOffers)
                error.valid.append(offer);
            return error;
        }
        const MediaHandle image = context.input(QStringLiteral("image")).media();
        if (mask.media().width() != image.width() || mask.media().height() != image.height())
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("mask"),
                                  QStringLiteral("The mask is not the size of the start image (%1 x %2).")
                                      .arg(image.width())
                                      .arg(image.height()),
                                  QStringLiteral("Paint or make the mask on the start image."));
        try {
            const Result<Pixels> pixels = maskOf(mask, QStringLiteral("mask"));
            if (!pixels)
                return pixels.errors();
            if (rangeOf(pixels.value()).empty)
                return makeInputError(QStringLiteral("mask_empty"), QStringLiteral("mask"),
                                      QStringLiteral("Nothing is painted in the mask."),
                                      QStringLiteral("Paint the part to change, or leave the mask unwired."));
            const Result<QString> path =
                coverMaskToFile(mask.media(), *size, QDir(context.workDir).filePath(QStringLiteral("mask.png")));
            if (!path)
                return path.errors();
            request.inputs.insert(QStringLiteral("mask"), QJsonObject{{"path", path.value()}});
        } catch (const vips::VError &error) {
            Error unreadable = vipsError(error, QStringLiteral("media_unreadable"));
            unreadable.message = QStringLiteral("Cannot prepare the mask: %1").arg(unreadable.message);
            return unreadable;
        }
        return Nothing{};
    }

    // The model's picture put back over the start image through the mask (Composite Masked's
    // kernel): out = start x (1 - m) + drawn x m, the start image and the mask cut as they went to the
    // model, so what is outside the mask is the start image's, to the pixel. A mask that reaches the
    // whole picture is a warning (plain image to image).
    Result<Value> putBack(const NodeContext &context, const MediaHandle &drawn, Errors &warnings) const {
        if (!initVips())
            return vipsUnavailable();
        const QSize size(drawn.width(), drawn.height());
        try {
            const Result<QString> start =
                prepared(context.input(QStringLiteral("image")).media(), size, QDir(context.workDir).filePath(QStringLiteral("start.png")));
            if (!start)
                return start.errors();
            const Result<QString> cut = coverMaskToFile(context.input(QStringLiteral("mask")).media(), size,
                                                        QDir(context.workDir).filePath(QStringLiteral("mask-put-back.png")));
            if (!cut)
                return cut.errors();
            const Result<MediaHandle> startHandle = MediaHandle::probe(MediaType::Image, start.value(), *context.hashes);
            const Result<MediaHandle> maskHandle = MediaHandle::probe(MediaType::Image, cut.value(), *context.hashes);
            if (!startHandle)
                return startHandle.errors();
            if (!maskHandle)
                return maskHandle.errors();
            const Result<Pixels> original = pixelsOf(Value(startHandle.value()), QStringLiteral("image"));
            const Result<Pixels> through = maskOf(Value(maskHandle.value()), QStringLiteral("mask"));
            const Result<Pixels> redrawn = pixelsOf(Value(drawn), QStringLiteral("image"));
            if (!original)
                return original.errors();
            if (!through)
                return through.errors();
            if (!redrawn)
                return redrawn.errors();
            if (rangeOf(through.value()).full) {
                Error full = makeInputError(QStringLiteral("mask_full"), QStringLiteral("mask"),
                                            QStringLiteral("Everything is masked: this redraws the whole picture, as plain image to image."),
                                            QStringLiteral("Paint only the part to change."));
                full.severity = Error::Severity::Warning;
                warnings << full;
            }
            const Result<Values> made =
                imageOutput(compositeMasked(original.value(), redrawn.value(), &through.value(), 0, 0));
            if (!made)
                return made.errors();
            return made->value(QStringLiteral("image"));
        } catch (const vips::VError &error) {
            Error unreadable = vipsError(error, QStringLiteral("media_unreadable"));
            unreadable.message = QStringLiteral("Cannot put the picture back through the mask: %1").arg(unreadable.message);
            return unreadable;
        }
    }

    // The size against the model's limits (8.1), blamed on the input that set it.
    Errors outsideLimits(const NodeContext &context, const Selection &selection, const QJsonObject &params) const {
        return outsideSizeLimits(modelName(selection), selection.model.limits, params, [&](const QString &side) {
            return context.inputs.contains(side)                      ? side
                   : editing()                                        ? QStringLiteral("images")
                   : context.inputs.contains(QStringLiteral("image")) ? QStringLiteral("image")
                                                                      : QString();
        });
    }

    static QString modelName(const Selection &selection) { return selection.model.handle.value("name").toString(); }

    bool editing() const { return m_capability == kEdit; }
    // Edit Image's instruction goes out as the capability's prompt (14.1).
    QString promptInput() const { return editing() ? QStringLiteral("instruction") : QStringLiteral("prompt"); }

    const QString m_capability;
};

} // namespace

void addImageGenerationKernels(Kernels &kernels) {
    kernels.add(QStringLiteral("fugue/generate-image"), std::make_shared<ImageKernel>(kGenerate));
    kernels.add(QStringLiteral("fugue/edit-image"), std::make_shared<ImageKernel>(kEdit));
}

} // namespace fugue::nodes
