// The component nodes (spec 14.3): image generation in stages, each a job of the helper's stage
// capabilities (8.10): Text Encode (text.encode), Sampler (latent.sample), VAE Decode
// (latent.decode) and VAE Encode (latent.encode); and the nodes that only make values: Load
// Models, Apply LoRA and Empty Latent.
//
// The model. stable-diffusion.cpp loads a model as one context of all its files, and checks at
// load that every component is there ("model metadata validation failed" without the VAE or the
// text encoder), so every stage sends all of them. Load Models makes one `fugue/model` handle of
// the three (a derived handle, 2.5: {"base": <diffusion model>, "text_encoder", "vae"}); Apply
// LoRA adds a step to its `apply`. A plain `fugue/model` handle is a complete checkpoint file.
// Every stage of one model sends the same context, so the helper keeps one context loaded for
// the whole graph. The LoRAs go with the generation options of the stages they change (encode
// and sample), not with the context.
//
// Video (#105) is the same stages: a model that makes video (Wan) is run by the same capabilities,
// the helper telling by the model files. A video latent is a latent whose descriptor also holds
// `frames` and `fps` (Empty Video Latent makes it), the Sampler sends them on, and VAE Decode Video
// takes the helper's decoded frames (a YUV4MPEG2 file, 8.10) into a video file. Wan 2.2's two
// diffusion models (high and low noise) are Load Models' `high_noise` part; its UMT5 text encoder
// goes in as `t5xxl_path`.
//
// The values between stages (2.6) are the helper's files, with a content hash (the scheduler
// adds it): conditioning, and latents whose `meta` holds the image size and, for a sampled or
// encoded latent, the model, which VAE Decode decodes with. An Empty Latent is only a size: the
// Sampler starts from noise of that size.
//
// What a stage sends is chosen so that the stages give the same pixels as one `generate` run of
// the same helper (the patch's promise, 8.10). The one difference: Text Encode always makes the
// negative set, since whether it is needed depends on the Sampler's guidance, which it does not
// know; sampling at guidance 1 skips it, as a single run at guidance 1 never makes it.

#include "nodes/kernels.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <optional>

#include <vips/vips8>

#include "engine/ffmpeg.h"
#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/model_library.h"
#include "engine/model_shape.h"
#include "engine/vips.h"
#include "nodes/image_cover.h"

namespace fugue::nodes {

namespace {

const QString kModelType = QStringLiteral("fugue/model");
const QString kLatentType = QStringLiteral("fugue/latent");
const QString kConditioningType = QStringLiteral("fugue/conditioning");

// What Text Encode sends as the guidance. Only "not 1" matters: then the encode makes the
// negative set too (request.cpp at the pin: use_uncond when the text guidance is not the image
// guidance, which is 1 unset). At guidance 1 that is a second, short encode the Sampler skips.
constexpr double kGuidanceThatMakesTheNegativeSet = 2;

// Image sizes go to the helper in steps of 16, which every model family it runs divides.
constexpr int kSizeStep = 16;

// A model's files as the helper takes them: the context options, and the LoRAs as the
// generation option "loras".
struct ModelFiles {
    QJsonObject context;
    QJsonArray loras;
};

// A `fugue/model` handle as a derived one: a checkpoint (a plain handle) becomes the base.
QJsonObject derivedOf(const QJsonObject &model) {
    return model.contains(QStringLiteral("derived")) ? model.value(QStringLiteral("derived")).toObject()
                                                     : QJsonObject{{"base", model}};
}

QJsonObject modelHandle(const QJsonObject &derived) { return QJsonObject{{"resource", kModelType}, {"derived", derived}}; }

// model_unsupported when a part is in a format the helper cannot load, model_mismatch when a
// model's text encoder, VAE or LoRA does not fit its diffusion model, by their headers
// (engine/model_shape.h): said here, before the helper fails to load them with less to say.
// The inputs a mismatch of each part is blamed on.
struct PartInputs {
    QString diffusionModel, textEncoder, vae, lora, highNoise;
};

// `loras` are the model's LoRAs with their paths, as ModelFiles has them, in Apply LoRA's order.
std::optional<Error> partsMismatch(const QJsonObject &derived, const QJsonObject &paths, const PartInputs &inputs,
                                   const QJsonArray &loras = {}) {
    ModelShapes &shapes = ModelShapes::shared(); // Load Models' check reads them on every canvas check
    // A file in a format the helper cannot load (the helper would abort building the model).
    // A T5 text encoder (Wan's UMT5) goes in as t5xxl_path, any other as llm_path.
    const QString encoderKey = paths.contains(QStringLiteral("t5xxl_path")) ? QStringLiteral("t5xxl_path") : QStringLiteral("llm_path");
    const QList<std::pair<QString, QString>> parts{{QStringLiteral("diffusion_model_path"), inputs.diffusionModel},
                                                   {encoderKey, inputs.textEncoder},
                                                   {QStringLiteral("vae_path"), inputs.vae},
                                                   {QStringLiteral("high_noise_diffusion_model_path"), inputs.highNoise}};
    const QStringList partNames{QStringLiteral("base"), QStringLiteral("text_encoder"), QStringLiteral("vae"),
                                QStringLiteral("high_noise")};
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (!paths.contains(parts.at(i).first))
            continue;
        const std::optional<ModelShape> part = shapes.of(paths.value(parts.at(i).first).toString());
        if (part && !part->unloadable.isEmpty()) {
            Error error = makeError(
                QStringLiteral("model_unsupported"),
                QStringLiteral("Fugue's helper cannot load %1: it is in %2, a quantization it has no support for.")
                    .arg(derived.value(partNames.at(i)).toObject().value(QStringLiteral("name")).toString(), part->unloadable),
                QStringLiteral("Pick the same model in another format (bf16, fp16, fp8_scaled or GGUF) in Load Models."));
            error.input = parts.at(i).second;
            return error;
        }
    }
    const std::optional<ModelShape> diffusion = shapes.of(paths.value(QStringLiteral("diffusion_model_path")).toString());
    if (!diffusion)
        return std::nullopt;
    const auto name = [&](const char *part) {
        return derived.value(QLatin1String(part)).toObject().value(QStringLiteral("name")).toString();
    };
    const auto mismatch = [](const QString &input, const QString &message, const QString &hint) {
        Error error = makeError(QStringLiteral("model_mismatch"), message, hint);
        error.input = input;
        return error;
    };
    const auto kind = [](TextKind k) {
        return k == TextKind::T5 ? QStringLiteral("a T5 encoder") : k == TextKind::Clip ? QStringLiteral("a CLIP encoder")
                                                                                        : QStringLiteral("a language model");
    };
    const std::optional<ModelShape> encoder = shapes.of(paths.value(encoderKey).toString());
    if (encoder && loadableTextEncoderFits(*diffusion, *encoder) == Fit::DoesNotFit) {
        const TextKind is = encoder->textKind.value_or(TextKind::Llm);
        const QString why = is != TextKind::Llm
                                ? QStringLiteral("it is %1, and Load Models takes a language model or a T5 encoder.").arg(kind(is))
                            : is != diffusion->textKind.value_or(TextKind::Llm)
                                ? QStringLiteral("it is %1, and the model reads %2.")
                                      .arg(kind(is), kind(diffusion->textKind.value_or(TextKind::Llm)))
                                : QStringLiteral("it is %1 wide, and the model reads %2.")
                                      .arg(encoder->hiddenSize.value_or(0))
                                      .arg(diffusion->textWidth.value_or(0));
        return mismatch(inputs.textEncoder, QStringLiteral("The text encoder %1 does not fit %2: %3").arg(name("text_encoder"), name("base"), why),
                        QStringLiteral("Pick another in Load Models: its list shows the text encoders that fit first."));
    }
    const std::optional<ModelShape> vae = shapes.of(paths.value(QStringLiteral("vae_path")).toString());
    if (vae && vaeFits(*diffusion, *vae) == Fit::DoesNotFit) {
        const QString why = vae->videoVae != diffusion->videoVae
                                ? (vae->videoVae.value_or(false) ? QStringLiteral("it is a video VAE, and the model takes an image one.")
                                                                 : QStringLiteral("the model takes a video VAE (Qwen-Image's or Wan's)."))
                                : QStringLiteral("it takes %1-channel latents, and the model makes %2.")
                                      .arg(vae->latentChannels.value_or(0))
                                      .arg(diffusion->latentChannels.value_or(0));
        return mismatch(inputs.vae, QStringLiteral("The VAE %1 does not fit %2: %3").arg(name("vae"), name("base"), why),
                        QStringLiteral("Pick another in Load Models: its list shows the VAEs that fit first."));
    }
    // Each LoRA of the model (Apply LoRA's steps, in order), against the diffusion model's layers.
    const QJsonArray steps = derived.value(QStringLiteral("apply")).toArray();
    for (qsizetype i = 0; i < steps.size() && i < loras.size(); ++i) {
        const std::optional<ModelShape> lora = shapes.of(loras.at(i).toObject().value(QStringLiteral("path")).toString());
        LayerMismatch layer;
        if (!lora || loraFits(*diffusion, *lora, &layer) != Fit::DoesNotFit)
            continue;
        const QString loraName = steps.at(i).toObject().value(QStringLiteral("lora")).toObject().value(QStringLiteral("name")).toString();
        const QString why = layer.layer.isEmpty() // none of its layers is in the model
                                ? QStringLiteral("the layers it changes are not in the model; it was made for another.")
                            : layer.model.in == 0 // a block the model does not have
                                ? QStringLiteral("it changes %1, a block the model does not have; it was made for a deeper one.")
                                      .arg(layer.layer)
                                : QStringLiteral("it changes a layer %1 to %2 wide that is %3 to %4 in the model.")
                                      .arg(layer.lora.in)
                                      .arg(layer.lora.out)
                                      .arg(layer.model.in)
                                      .arg(layer.model.out);
        return mismatch(inputs.lora, QStringLiteral("The LoRA %1 does not fit %2: %3").arg(loraName, name("base"), why),
                        QStringLiteral("Pick another in Apply LoRA: its list shows the LoRAs that fit first."));
    }
    return std::nullopt;
}

// A model file as a paired host takes it (8.10): its name, hash and, when the file is here, size.
// The host finds the file in its own folders and answers model_missing itself.
QJsonObject remoteHandle(const QJsonValue &value, const ModelLibrary &library, FileHashes &hashes) {
    const QJsonObject handle = value.toObject();
    QJsonObject sent{{"name", handle.value(QStringLiteral("name")).toString()}};
    if (const QString hash = handle.value(QStringLiteral("hash")).toString(); !hash.isEmpty())
        sent.insert(QStringLiteral("sha256"), hash);
    // The size tells the host which of two files of one name is meant; known when the file is here
    // (a check that reads no file, Hashing::RememberedOnly).
    if (const Result<QString> found = library.resolve(handle, hashes, ModelLibrary::Hashing::RememberedOnly))
        sent.insert(QStringLiteral("size"), double(QFileInfo(found.value()).size()));
    return sent;
}

// The context option a text encoder goes in as: Wan's UMT5 (a T5, by its tensors) as `t5xxl_path`,
// any other as `llm_path`. Found by name on this machine, without reading it, as for a paired host;
// a file that is not here is taken for a language model.
QString encoderKeyOf(const QString &path) {
    const std::optional<ModelShape> shape = ModelShapes::shared().of(path);
    return shape && shape->textKind == TextKind::T5 ? QStringLiteral("t5xxl_path") : QStringLiteral("llm_path");
}

QString textEncoderKey(const QJsonObject &encoder, const ModelLibrary &library, FileHashes &hashes) {
    const Result<QString> found = library.resolve(encoder, hashes, ModelLibrary::Hashing::RememberedOnly);
    return found ? encoderKeyOf(found.value()) : QStringLiteral("llm_path");
}

// The files of the `fugue/model` handle in `input`, found on this machine (8.9). model_missing,
// blamed on the input, when one is not here. For a provider that does not share the filesystem
// (`shares` false) the files are handles the host resolves, and nothing is looked for or checked
// here: the host checks.
Result<ModelFiles> modelFiles(const QJsonObject &model, const QString &input, const NodeContext &context,
                              bool shares = true) {
    const ModelLibrary library = ModelLibrary::current(context.registry);
    std::optional<Error> missing;
    const auto path = [&](const QJsonValue &handle) -> QJsonValue {
        if (!shares)
            return remoteHandle(handle, library, *context.hashes);
        const Result<QString> found = library.resolve(handle.toObject(), *context.hashes);
        if (!found && !missing) {
            missing = found.error();
            missing->input = input;
        }
        return found ? found.value() : QString();
    };
    const QJsonObject derived = derivedOf(model);
    const QJsonObject base = derived.value(QStringLiteral("base")).toObject();
    const bool checkpoint = base.value(QStringLiteral("resource")).toString() == kModelType; // every part in one file
    ModelFiles files;
    files.context.insert(checkpoint ? QStringLiteral("model_path") : QStringLiteral("diffusion_model_path"), path(base));
    if (derived.contains(QStringLiteral("high_noise")))
        files.context.insert(QStringLiteral("high_noise_diffusion_model_path"), path(derived.value(QStringLiteral("high_noise"))));
    if (derived.contains(QStringLiteral("text_encoder"))) {
        const QJsonObject encoder = derived.value(QStringLiteral("text_encoder")).toObject();
        files.context.insert(textEncoderKey(encoder, library, *context.hashes), path(encoder));
    }
    if (derived.contains(QStringLiteral("vae")))
        files.context.insert(QStringLiteral("vae_path"), path(derived.value(QStringLiteral("vae"))));
    for (const QJsonValue &step : derived.value(QStringLiteral("apply")).toArray()) {
        const QJsonObject apply = step.toObject();
        files.loras.append(QJsonObject{{"path", path(apply.value(QStringLiteral("lora")))},
                                       {"multiplier", apply.value(QStringLiteral("strength")).toDouble(1)}});
    }
    if (missing)
        return *missing;
    if (!shares)
        return files;
    if (const std::optional<Error> mismatch = partsMismatch(derived, files.context, {input, input, input, input, {}}, files.loras))
        return *mismatch;
    return files;
}

// A latent's descriptor (2.6): the image size, the model that made it (none for an Empty
// Latent), and whether it is only a size.
struct LatentMeta {
    int width = 0;
    int height = 0;
    QJsonObject model;
    bool empty = false;
    int frames = 0; // a video latent's length and rate (2.6); 0 for an image
    int fps = 0;

    bool isVideo() const { return frames > 0; }

    static LatentMeta of(const Value &latent) {
        const QJsonObject meta = latent.json().toObject().value(QStringLiteral("meta")).toObject();
        return {meta.value(QStringLiteral("width")).toInt(), meta.value(QStringLiteral("height")).toInt(),
                meta.value(QStringLiteral("model")).toObject(), meta.value(QStringLiteral("empty")).toBool(),
                meta.value(QStringLiteral("frames")).toInt(), meta.value(QStringLiteral("fps")).toInt()};
    }
    QJsonObject toJson() const {
        QJsonObject meta{{"width", width}, {"height", height}};
        if (!model.isEmpty())
            meta.insert(QStringLiteral("model"), model);
        if (empty)
            meta.insert(QStringLiteral("empty"), true);
        if (isVideo()) {
            meta.insert(QStringLiteral("frames"), frames);
            meta.insert(QStringLiteral("fps"), fps);
        }
        return meta;
    }
};

// A data value's file as a job input ({"path"}).
QJsonObject fileOf(const Value &data) {
    return QJsonObject{{"path", data.json().toObject().value(QStringLiteral("path")).toString()}};
}

// The file the helper wrote for `output`, or node_failed.
Result<QString> written(const JobResult &result, const QString &output) {
    const QString path = result.outputs.value(output).toObject().value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        return makeError(QStringLiteral("node_failed"), QStringLiteral("The helper finished without a %1.").arg(output));
    return path;
}

// A data handle (2.6) for a file in the node's folder; the scheduler adds its hash.
QJsonObject dataHandle(const QString &type, const QString &path, const QJsonObject &descriptor = {}) {
    QJsonObject handle{{"data", type}, {"path", path}};
    if (!descriptor.isEmpty())
        handle.insert(QStringLiteral("meta"), descriptor);
    return handle;
}

// A latent the helper wrote for a stage of `model`, of the size (and for a video, the length and
// rate) `size` says.
Result<Values> latentOutput(const JobResult &result, const LatentMeta &size, const QJsonValue &model) {
    const Result<QString> path = written(result, QStringLiteral("latent"));
    if (!path)
        return path.errors();
    LatentMeta made = size;
    made.model = model.toObject();
    made.empty = false;
    return Values{{QStringLiteral("latent"), Value(dataHandle(kLatentType, path.value(), made.toJson()))}};
}

// A file the helper reads (stb_image: PNG or JPEG, which ignores EXIF orientation): the
// image's own when it is one and upright, else the image written as an 8-bit RGB PNG to
// `converted`, upright.
Result<QString> readable(const MediaHandle &image, const QString &converted) {
    // Before anything reads an image: an input from the result cache was never probed.
    if (!initVips())
        return vipsUnavailable();
    const QString format = image.format();
    try {
        if (image.hasFile() && (format == QLatin1String("png") || format == QLatin1String("jpeg"))) {
            vips::VImage header = vips::VImage::new_from_file(QFile::encodeName(image.path()).constData());
            if (header.get_typeof("orientation") == 0 || header.get_int("orientation") == 1)
                return image.path();
        }
        const Result<vips::VImage> decoded = image.image();
        if (!decoded)
            return decoded.errors();
        vips::VImage rgb = decoded->autorot().colourspace(VIPS_INTERPRETATION_sRGB);
        if (rgb.bands() > 3)
            rgb = rgb.extract_band(0, vips::VImage::option()->set("n", 3));
        rgb.cast(VIPS_FORMAT_UCHAR).write_to_file(QFile::encodeName(converted).constData());
        return converted;
    } catch (const vips::VError &error) {
        return vipsError(error, QStringLiteral("media_unreadable"));
    }
}

// The references of an edit as the generation option ref_images ([{"image": PATH}]), each a
// file the helper reads; a converted one is written into `workDir`.
Result<QJsonArray> refImages(const QList<MediaHandle> &references, const QString &workDir) {
    QJsonArray sent;
    for (qsizetype i = 0; i < references.size(); ++i) {
        const Result<QString> file =
            readable(references.at(i), QDir(workDir).filePath(QStringLiteral("reference-%1.png").arg(i + 1)));
        if (!file)
            return file.errors();
        sent.append(QJsonObject{{"image", file.value()}});
    }
    return sent;
}

// The references a conditioning carries (Text Encode puts them in its descriptor, as ComfyUI's
// ReferenceLatent puts reference latents on a conditioning). Each must still be the picture
// Text Encode had: a cached conditioning can outlive a file it names.
Result<QList<MediaHandle>> referencesOf(const Value &conditioning, FileHashes &hashes) {
    QList<MediaHandle> references;
    const QJsonArray carried =
        conditioning.json().toObject().value(QStringLiteral("meta")).toObject().value(QStringLiteral("references")).toArray();
    for (const QJsonValue &item : carried) {
        const Result<MediaHandle> image = MediaHandle::fromJson(item.toObject());
        if (!image)
            return image.errors();
        if (hashes.hash(image->path()) != image->hash())
            return makeInputError(QStringLiteral("file_not_found"), QStringLiteral("conditioning"),
                                  QStringLiteral("The reference %1 is gone or changed since Text Encode read it.").arg(image->path()),
                                  QStringLiteral("Run Text Encode again with the pictures as they are now."));
        references.append(image.value());
    }
    return references;
}

// The size and length an image-to-video first frame was encoded for (Text Encode puts it in its
// descriptor with the picture, the Sampler checks it against its latent).
LatentMeta clipOf(const QJsonObject &carried) {
    LatentMeta clip;
    clip.width = carried.value(QStringLiteral("width")).toInt();
    clip.height = carried.value(QStringLiteral("height")).toInt();
    clip.frames = carried.value(QStringLiteral("frames")).toInt();
    clip.fps = carried.value(QStringLiteral("fps")).toInt();
    return clip;
}

// `frame` scaled to cover the clip's size and cut from the middle, as the PNG the helper reads
// as the first frame (Generate Video makes it the same way).
Result<QString> firstFrameFile(const MediaHandle &frame, const LatentMeta &clip, const QString &workDir, const QString &input) {
    if (frame.width() <= 0 || frame.height() <= 0)
        return makeInputError(QStringLiteral("media_unreadable"), input, QStringLiteral("The first frame has no size."));
    if (!initVips())
        return vipsUnavailable();
    try {
        return coverToFile(frame, QSize(clip.width, clip.height), QDir(workDir).filePath(QStringLiteral("first-frame.png")));
    } catch (const vips::VError &error) {
        Error unreadable = vipsError(error, QStringLiteral("media_unreadable"));
        unreadable.message = QStringLiteral("Cannot prepare the first frame: %1").arg(unreadable.message);
        return unreadable;
    }
}

// The first frame a conditioning carries, with the clip it was encoded for; none (an invalid
// handle) for a text-to-video one. It must still be the picture Text Encode had.
struct CarriedFrame {
    MediaHandle frame;
    LatentMeta clip;
    bool present = false;
};
Result<CarriedFrame> firstFrameOf(const Value &conditioning, FileHashes &hashes) {
    const QJsonObject meta = conditioning.json().toObject().value(QStringLiteral("meta")).toObject();
    CarriedFrame carried;
    if (!meta.contains(QStringLiteral("first_frame")))
        return carried;
    const Result<MediaHandle> frame = MediaHandle::fromJson(meta.value(QStringLiteral("first_frame")).toObject());
    if (!frame)
        return frame.errors();
    if (hashes.hash(frame->path()) != frame->hash())
        return makeInputError(QStringLiteral("file_not_found"), QStringLiteral("conditioning"),
                              QStringLiteral("The first frame %1 is gone or changed since Text Encode read it.").arg(frame->path()),
                              QStringLiteral("Run Text Encode again with the picture as it is now."));
    carried.frame = frame.value();
    carried.clip = clipOf(meta.value(QStringLiteral("clip")).toObject());
    carried.present = true;
    return carried;
}

// --- the nodes that only make values ---

class LoadModelsKernel : public NodeKernel {
public:
    // A text encoder or VAE that does not fit the diffusion model, from the files' headers, so
    // the node says so before a run (#85). Reads no file to find them: one with the handle's
    // name is taken to be it, as any check does (8.9).
    Errors check(const NodeContext &context) const override {
        const ModelLibrary library = ModelLibrary::current(context.registry);
        const auto path = [&](const char *input) {
            const Result<QString> found = library.resolve(context.input(QLatin1String(input)).json().toObject(),
                                                          *context.hashes, ModelLibrary::Hashing::RememberedOnly);
            return found ? found.value() : QString();
        };
        const QString encoder = path("text_encoder");
        QJsonObject paths{{"diffusion_model_path", path("diffusion_model")}, {"vae_path", path("vae")}};
        paths.insert(encoderKeyOf(encoder), encoder);
        if (hasHighNoise(context))
            paths.insert(QStringLiteral("high_noise_diffusion_model_path"), path("high_noise_diffusion_model"));
        // Blamed on the part that does not fit, where its picker is.
        if (const std::optional<Error> mismatch = partsMismatch(
                derived(context), paths,
                {QStringLiteral("diffusion_model"), QStringLiteral("text_encoder"), QStringLiteral("vae"), QString(),
                 QStringLiteral("high_noise_diffusion_model")}))
            return {*mismatch};
        return {};
    }

    Result<Values> run(const NodeContext &context) override {
        return Values{{QStringLiteral("model"), Value(modelHandle(derived(context)))}};
    }

private:
    static bool hasHighNoise(const NodeContext &context) {
        return !context.input(QStringLiteral("high_noise_diffusion_model")).isEmpty();
    }
    static QJsonObject derived(const NodeContext &context) {
        QJsonObject parts{{"base", context.input(QStringLiteral("diffusion_model")).json()},
                          {"text_encoder", context.input(QStringLiteral("text_encoder")).json()},
                          {"vae", context.input(QStringLiteral("vae")).json()}};
        if (hasHighNoise(context))
            parts.insert(QStringLiteral("high_noise"), context.input(QStringLiteral("high_noise_diffusion_model")).json());
        return parts;
    }
};

class ApplyLoraKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        QJsonObject derived = derivedOf(context.input(QStringLiteral("model")).json().toObject());
        QJsonArray apply = derived.value(QStringLiteral("apply")).toArray();
        apply.append(QJsonObject{{"lora", context.input(QStringLiteral("lora")).json()},
                                 {"strength", context.input(QStringLiteral("strength")).number()}});
        derived.insert(QStringLiteral("apply"), apply);
        return Values{{QStringLiteral("model"), Value(modelHandle(derived))}};
    }
};

// A size (and for a video the length and rate) and nothing else: the file holds it, so its hash
// differs by them.
class EmptyLatentKernel : public NodeKernel {
public:
    explicit EmptyLatentKernel(bool video = false) : m_video(video) {}

    Result<Values> run(const NodeContext &context) override {
        LatentMeta size;
        size.width = int(context.input(QStringLiteral("width")).number());
        size.height = int(context.input(QStringLiteral("height")).number());
        if (m_video) {
            size.frames = int(context.input(QStringLiteral("frames")).number());
            size.fps = int(context.input(QStringLiteral("fps")).number());
        }
        size.empty = true;
        QJsonObject described{{"width", size.width}, {"height", size.height}};
        if (m_video) {
            described.insert(QStringLiteral("frames"), size.frames);
            described.insert(QStringLiteral("fps"), size.fps);
        }
        const QByteArray bytes = QJsonDocument(described).toJson(QJsonDocument::Compact);
        const QString path = QDir(context.workDir).filePath(QStringLiteral("latent.json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
            return makeError(QStringLiteral("node_failed"), QStringLiteral("Cannot write %1.").arg(path));
        return Values{{QStringLiteral("latent"), Value(dataHandle(kLatentType, path, size.toJson()))}};
    }

private:
    const bool m_video;
};

// --- the stages ---

// A stage: its capability, and any ready model of it (the helper offers one, with an empty
// handle: the model files go in the job).
class StageKernel : public ProviderKernel {
public:
    explicit StageKernel(QString capability) : m_capability(std::move(capability)) {}

    Requirement requirement(const NodeType &, const Values &) const override {
        Requirement requirement;
        requirement.capability = m_capability;
        return requirement;
    }

protected:
    // The files of the node's `model` input.
    static Result<ModelFiles> modelInput(const NodeContext &context, const Selection &selection) {
        return modelFiles(context.input(QStringLiteral("model")).json().toObject(), QStringLiteral("model"), context,
                          selection.sharesFilesystem);
    }

    // A job of this stage for `files`, with the generation options `params`; with the model's
    // LoRAs when `loras` (the stages they change: encode and sample).
    JobRequest job(const ModelFiles &files, QJsonObject params, bool loras = false) const {
        params.insert(QStringLiteral("context"), files.context);
        if (loras && !files.loras.isEmpty())
            params.insert(QStringLiteral("loras"), files.loras);
        JobRequest request;
        request.capability = m_capability;
        request.params = params;
        return request;
    }

private:
    const QString m_capability;
};

class TextEncodeKernel : public StageKernel {
public:
    TextEncodeKernel() : StageKernel(QStringLiteral("text.encode")) {}

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        const Result<ModelFiles> files = modelInput(context, selection);
        if (!files)
            return files.errors();
        QJsonObject params{{"prompt", context.input(QStringLiteral("prompt")).text()},
                           {"negative_prompt", context.input(QStringLiteral("negative_prompt")).text()},
                           {"sample.guidance.txt_cfg", kGuidanceThatMakesTheNegativeSet}};
        // The references change the encode too: a model that reads them in its text encoder
        // (Qwen-Image Edit), and the guidance sets an edit makes (request.cpp at the pin).
        if (const QList<MediaHandle> references = referencesInput(context); !references.isEmpty()) {
            const Result<QJsonArray> sent = refImages(references, context.workDir);
            if (!sent)
                return sent.errors();
            params.insert(QStringLiteral("ref_images"), sent.value());
        }
        JobRequest request = job(*files, params, true);
        // An image-to-video model makes its start from the first frame in the encode (Wan 2.2 I2V's
        // c_concat), which needs the size and length of the clip: the Clip input's.
        if (const Value frame = context.input(QStringLiteral("first_frame")); !frame.isEmpty()) {
            const Result<LatentMeta> clip = clipInput(context);
            if (!clip)
                return clip.errors();
            const Result<QString> file = firstFrameFile(frame.media(), clip.value(), context.workDir, QStringLiteral("first_frame"));
            if (!file)
                return file.errors();
            request.params.insert(QStringLiteral("width"), clip->width);
            request.params.insert(QStringLiteral("height"), clip->height);
            request.params.insert(QStringLiteral("video_frames"), clip->frames);
            request.params.insert(QStringLiteral("fps"), clip->fps);
            request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", file.value()}});
        }
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        const Result<QString> path = written(result, QStringLiteral("conditioning"));
        if (!path)
            return path.errors();
        QJsonObject descriptor;
        // The first frame goes on the conditioning with the clip it was made for, so the Sampler
        // sends the same ones (Wan 2.2 TI2V starts its sampling from the frame).
        if (const Value frame = context.input(QStringLiteral("first_frame")); !frame.isEmpty()) {
            const Result<LatentMeta> clip = clipInput(context);
            if (!clip)
                return clip.errors();
            descriptor.insert(QStringLiteral("first_frame"), frame.media().toJson());
            descriptor.insert(QStringLiteral("clip"), QJsonObject{{"width", clip->width}, {"height", clip->height},
                                                                   {"frames", clip->frames}, {"fps", clip->fps}});
        }
        if (const QList<MediaHandle> references = referencesInput(context); !references.isEmpty()) {
            QJsonArray carried;
            for (const MediaHandle &image : references)
                carried.append(image.toJson());
            descriptor.insert(QStringLiteral("references"), carried);
        }
        return Values{{QStringLiteral("conditioning"), Value(dataHandle(kConditioningType, path.value(), descriptor))}};
    }

private:
    // The clip a first frame is for: an Empty Video Latent (or a sampled video one).
    static Result<LatentMeta> clipInput(const NodeContext &context) {
        const Value latent = context.input(QStringLiteral("clip"));
        if (latent.isEmpty())
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("clip"),
                                  QStringLiteral("A first frame needs the size and length of the clip."),
                                  QStringLiteral("Wire the Empty Video Latent that the Sampler starts from to Clip."));
        const LatentMeta clip = LatentMeta::of(latent);
        if (!clip.isVideo())
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("clip"),
                                  QStringLiteral("This latent is an image: a first frame is for a video."),
                                  QStringLiteral("Use an Empty Video Latent."));
        return clip;
    }

    static QList<MediaHandle> referencesInput(const NodeContext &context) {
        QList<MediaHandle> references;
        for (const Value &item : context.input(QStringLiteral("references")).items())
            references.append(item.media());
        return references;
    }
};

bool sameClip(const LatentMeta &a, const LatentMeta &b) {
    return a.width == b.width && a.height == b.height && a.frames == b.frames && a.fps == b.fps;
}

class SamplerKernel : public StageKernel {
public:
    SamplerKernel() : StageKernel(QStringLiteral("latent.sample")) {}

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        const Result<ModelFiles> files = modelInput(context, selection);
        if (!files)
            return files.errors();
        const Value latent = context.input(QStringLiteral("latent"));
        const LatentMeta from = LatentMeta::of(latent);
        // A video starts from noise: nothing makes a sampled video latent a start yet.
        if (from.isVideo() && !from.empty)
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("latent"),
                                  QStringLiteral("A video cannot be redrawn from a sampled video yet."),
                                  QStringLiteral("Start from an Empty Video Latent."));
        QJsonObject params{{"width", from.width},
                           {"height", from.height},
                           {"seed", context.input(QStringLiteral("seed")).json().toInteger()},
                           {"sample.steps", int(context.input(QStringLiteral("steps")).number())},
                           {"sample.guidance.txt_cfg", context.input(QStringLiteral("guidance")).number()}};
        if (from.isVideo()) {
            params.insert(QStringLiteral("video_frames"), from.frames);
            params.insert(QStringLiteral("fps"), from.fps);
        }
        if (const Value method = context.input(QStringLiteral("sampler")); !method.isEmpty())
            params.insert(QStringLiteral("sample.method"), method.text());
        if (const Value scheduler = context.input(QStringLiteral("scheduler")); !scheduler.isEmpty())
            params.insert(QStringLiteral("sample.scheduler"), scheduler.text());
        // The edit's references, sent as Text Encode sent them, so both stages make one generation.
        const Result<QList<MediaHandle>> references = referencesOf(context.input(QStringLiteral("conditioning")), *context.hashes);
        if (!references)
            return references.errors();
        if (!references->isEmpty()) {
            const Result<QJsonArray> sent = refImages(references.value(), context.workDir);
            if (!sent)
                return sent.errors();
            params.insert(QStringLiteral("ref_images"), sent.value());
        }
        // The first frame of an image-to-video conditioning, for the clip it was encoded for.
        const Result<CarriedFrame> carried = firstFrameOf(context.input(QStringLiteral("conditioning")), *context.hashes);
        if (!carried)
            return carried.errors();
        if (carried->present && !sameClip(carried->clip, from))
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("latent"),
                                  QStringLiteral("Text Encode made the first frame for a clip of %1 x %2, %3 frames at %4 a second; this latent is %5 x %6, %7 frames at %8.")
                                      .arg(carried->clip.width).arg(carried->clip.height).arg(carried->clip.frames).arg(carried->clip.fps)
                                      .arg(from.width).arg(from.height).arg(from.frames).arg(from.fps),
                                  QStringLiteral("Wire the same Empty Video Latent to Text Encode's Clip and to the Sampler."));
        JobRequest request = job(*files, params, true);
        request.inputs.insert(QStringLiteral("conditioning"), fileOf(context.input(QStringLiteral("conditioning"))));
        if (carried->present) {
            const Result<QString> frame = firstFrameFile(carried->frame, from, context.workDir, QStringLiteral("conditioning"));
            if (!frame)
                return frame.errors();
            request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", frame.value()}});
        }
        if (!from.empty) { // redrawn from a latent, by the amount denoise sets
            request.inputs.insert(QStringLiteral("init_latent"), fileOf(latent));
            request.params.insert(QStringLiteral("strength"), context.input(QStringLiteral("denoise")).number());
        }
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        return latentOutput(result, LatentMeta::of(context.input(QStringLiteral("latent"))),
                            context.input(QStringLiteral("model")).json());
    }
};

// VAE Decode (an image) and VAE Decode Video (a clip): one capability, the helper making the kind
// the model makes. A latent of the other kind is said so here, before the helper decodes it.
class VaeDecodeKernel : public StageKernel {
public:
    explicit VaeDecodeKernel(bool video = false) : StageKernel(QStringLiteral("latent.decode")), m_video(video) {}

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        const Value latent = context.input(QStringLiteral("latent"));
        const LatentMeta sampled = LatentMeta::of(latent);
        if (sampled.model.isEmpty())
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("latent"),
                                  QStringLiteral("An Empty Latent has no image in it yet."),
                                  QStringLiteral("Sample it first: wire it through a Sampler."));
        if (sampled.isVideo() != m_video)
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("latent"),
                                  m_video ? QStringLiteral("This latent is an image, and VAE Decode Video decodes a video.")
                                          : QStringLiteral("This latent is a video, and VAE Decode decodes an image."),
                                  m_video ? QStringLiteral("Use VAE Decode, or start from an Empty Video Latent.")
                                          : QStringLiteral("Use VAE Decode Video."));
        const Result<ModelFiles> files = modelFiles(sampled.model, QStringLiteral("latent"), context, selection.sharesFilesystem);
        if (!files)
            return files.errors();
        QJsonObject params{{"width", sampled.width}, {"height", sampled.height}};
        if (m_video) { // every stage of a generation takes the same options
            params.insert(QStringLiteral("video_frames"), sampled.frames);
            params.insert(QStringLiteral("fps"), sampled.fps);
        }
        JobRequest request = job(*files, params);
        request.inputs.insert(QStringLiteral("latent"), fileOf(latent));
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        if (m_video)
            return video(context, result);
        const Result<QString> path = written(result, QStringLiteral("image"));
        if (!path)
            return path.errors();
        const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, path.value(), *context.hashes);
        if (!image)
            return image.errors();
        return Values{{QStringLiteral("image"), Value(image.value())}};
    }

private:
    // The helper's frames (YUV4MPEG2, 8.10) as a video file: lossless, as every intermediate video of
    // the engine is (FFV1 in Matroska), for Save Video to encode as the user chooses.
    static Result<Values> video(const NodeContext &context, const JobResult &result) {
        const Result<QString> frames = written(result, QStringLiteral("video"));
        if (!frames)
            return frames.errors();
        const QString path = QDir(context.workDir).filePath(QStringLiteral("video.mkv"));
        const Result<QByteArray> done = ffmpeg::run(
            QStringLiteral("ffmpeg"),
            {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", frames.value(), "-c:v", "ffv1", "-f", "matroska", path},
            QStringLiteral("node_failed"));
        if (!done)
            return done.errors();
        const Result<MediaHandle> clip = MediaHandle::probe(MediaType::Video, path, *context.hashes);
        if (!clip)
            return clip.errors();
        return Values{{QStringLiteral("video"), Value(clip.value())}};
    }

    const bool m_video;
};

class VaeEncodeKernel : public StageKernel {
public:
    VaeEncodeKernel() : StageKernel(QStringLiteral("latent.encode")) {}

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        const Result<ModelFiles> files = modelInput(context, selection);
        if (!files)
            return files.errors();
        const MediaHandle image = context.input(QStringLiteral("image")).media();
        const Result<QString> file = readable(image, QDir(context.workDir).filePath(QStringLiteral("image.png")));
        if (!file)
            return file.errors();
        // The helper scales the image to the size it is given: its own, in steps of 16.
        const QSize size = sizeOf(image);
        JobRequest request = job(*files, QJsonObject{{"width", size.width()}, {"height", size.height()}});
        request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", file.value()}});
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        const QSize size = sizeOf(context.input(QStringLiteral("image")).media());
        LatentMeta meta;
        meta.width = size.width();
        meta.height = size.height();
        return latentOutput(result, meta, context.input(QStringLiteral("model")).json());
    }

private:
    static QSize sizeOf(const MediaHandle &image) {
        const auto step = [](int side) { return std::max(1, int(std::lround(double(side) / kSizeStep))) * kSizeStep; };
        return {step(image.width()), step(image.height())};
    }

};

} // namespace

void addComponentKernels(Kernels &kernels) {
    kernels.add(QStringLiteral("fugue/load-models"), std::make_shared<LoadModelsKernel>());
    kernels.add(QStringLiteral("fugue/apply-lora"), std::make_shared<ApplyLoraKernel>());
    kernels.add(QStringLiteral("fugue/empty-latent"), std::make_shared<EmptyLatentKernel>());
    kernels.add(QStringLiteral("fugue/empty-video-latent"), std::make_shared<EmptyLatentKernel>(true));
    kernels.add(QStringLiteral("fugue/text-encode"), std::make_shared<TextEncodeKernel>());
    kernels.add(QStringLiteral("fugue/sampler"), std::make_shared<SamplerKernel>());
    kernels.add(QStringLiteral("fugue/vae-decode"), std::make_shared<VaeDecodeKernel>());
    kernels.add(QStringLiteral("fugue/vae-decode-video"), std::make_shared<VaeDecodeKernel>(true));
    kernels.add(QStringLiteral("fugue/vae-encode"), std::make_shared<VaeEncodeKernel>());
}

} // namespace fugue::nodes
