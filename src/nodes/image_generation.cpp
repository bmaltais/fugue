// Generate Image and Edit Image (spec 14.1): the coarse image nodes, served by a provider's
// image.generate and image.edit (8.1). The kernels only translate (node_kernel.h): inputs to a
// JobRequest, and the JobResult to outputs with the values actually used (3.3).

#include "nodes/kernels.h"

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
constexpr double kMegapixel = 1024.0 * 1024.0;
// Edit Image's references: Fugue's starting four (14.1), raised by a model whose limits say
// `max_reference_images` is higher.
constexpr int kReferences = 4;

// The size nearest a megapixel in the shape width x height, in steps of 64 (fitMegapixel).
QSize fitMegapixel(int width, int height) {
    const double aspect = double(width) / height;
    const int w = std::max(1, int(std::lround(std::sqrt(kMegapixel * aspect) / kSizeMultiple))) * kSizeMultiple;
    const int h = std::max(1, int(std::lround(kMegapixel / w / kSizeMultiple))) * kSizeMultiple;
    return {w, h};
}

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
    const auto options = [&] {
        return vips::VImage::option()
            ->set("height", size.height())
            ->set("crop", VIPS_INTERESTING_CENTRE);
    };
    vips::VImage fitted;
    if (image.hasFile()) {
        fitted = vips::VImage::thumbnail(QFile::encodeName(image.path()).constData(), size.width(), options());
    } else {
        const Result<vips::VImage> decoded = image.image();
        if (!decoded)
            return decoded.errors();
        fitted = decoded->thumbnail_image(size.width(), options());
    }
    fitted = fitted.colourspace(VIPS_INTERPRETATION_sRGB);
    if (fitted.bands() > 3)
        fitted = fitted.extract_band(0, vips::VImage::option()->set("n", 3));
    fitted.write_to_file(QFile::encodeName(path).constData());
    return path;
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
            if (!set.isEmpty())
                params.insert(key, set.json());
            else if (defaults.contains(key))
                params.insert(key, defaults.value(key));
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
        return Values{{QStringLiteral("image"), Value(image.value())},
                      {QStringLiteral("seed"), number(QStringLiteral("seed"), context.input(QStringLiteral("seed")))},
                      {QStringLiteral("width"), number(QStringLiteral("width"), Value(image->width()))},
                      {QStringLiteral("height"), number(QStringLiteral("height"), Value(image->height()))},
                      {QStringLiteral("model"), Value(model.isEmpty() ? selection.model.handle : model)}};
    }

private:
    // Edit Image's references, each prepared on its own (prepareBase), into request.inputs.
    Result<std::optional<QSize>> references(const NodeContext &context, const Selection &selection,
                                            JobRequest &request) const {
        const QList<Value> images = context.input(QStringLiteral("images")).items();
        const int most = std::max(kReferences, selection.model.limits.value("max_reference_images").toInt());
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

    // The size against the model's limits (8.1), blamed on the input that set it.
    Errors outsideLimits(const NodeContext &context, const Selection &selection, const QJsonObject &params) const {
        Errors errors;
        const QJsonObject &limits = selection.model.limits;
        for (const QString &side : {QStringLiteral("width"), QStringLiteral("height")}) {
            if (!params.contains(side))
                continue;
            const int value = params.value(side).toInt();
            const int least = limits.value(QStringLiteral("min_") + side).toInt(0);
            const int most = limits.value(QStringLiteral("max_") + side).toInt(0);
            if ((least > 0 && value < least) || (most > 0 && value > most)) {
                const QString blamed = context.inputs.contains(side)                   ? side
                                       : editing()                                     ? QStringLiteral("images")
                                       : context.inputs.contains(QStringLiteral("image")) ? QStringLiteral("image")
                                                                                       : QString();
                errors.append(makeInputError(QStringLiteral("value_out_of_range"), blamed,
                                             QStringLiteral("%1 makes a %2 of %3 to %4 pixels, not %5.")
                                                 .arg(modelName(selection), side)
                                                 .arg(least)
                                                 .arg(most > 0 ? QString::number(most) : QStringLiteral("any"))
                                                 .arg(value)));
            }
        }
        return errors;
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
