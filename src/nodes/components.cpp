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

#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/model_library.h"
#include "engine/model_shape.h"
#include "engine/vips.h"

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
    QString diffusionModel, textEncoder, vae, lora;
};

// `loras` are the model's LoRAs with their paths, as ModelFiles has them, in Apply LoRA's order.
std::optional<Error> partsMismatch(const QJsonObject &derived, const QJsonObject &paths, const PartInputs &inputs,
                                   const QJsonArray &loras = {}) {
    ModelShapes &shapes = ModelShapes::shared(); // Load Models' check reads them on every canvas check
    // A file in a format the helper cannot load (the helper would abort building the model).
    const QList<std::pair<QString, QString>> parts{{QStringLiteral("diffusion_model_path"), inputs.diffusionModel},
                                                   {QStringLiteral("llm_path"), inputs.textEncoder},
                                                   {QStringLiteral("vae_path"), inputs.vae}};
    const QStringList partNames{QStringLiteral("base"), QStringLiteral("text_encoder"), QStringLiteral("vae")};
    for (qsizetype i = 0; i < parts.size(); ++i) {
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
    const std::optional<ModelShape> encoder = shapes.of(paths.value(QStringLiteral("llm_path")).toString());
    if (encoder && loadableTextEncoderFits(*diffusion, *encoder) == Fit::DoesNotFit) {
        const TextKind is = encoder->textKind.value_or(TextKind::Llm);
        const QString why = is != TextKind::Llm
                                ? QStringLiteral("it is %1, and Load Models takes a language model.").arg(kind(is))
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

// The files of the `fugue/model` handle in `input`, found on this machine (8.9). model_missing,
// blamed on the input, when one is not here.
Result<ModelFiles> modelFiles(const QJsonObject &model, const QString &input, const NodeContext &context) {
    const ModelLibrary library = ModelLibrary::current(context.registry);
    std::optional<Error> missing;
    const auto path = [&](const QJsonValue &handle) {
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
    if (derived.contains(QStringLiteral("text_encoder")))
        files.context.insert(QStringLiteral("llm_path"), path(derived.value(QStringLiteral("text_encoder"))));
    if (derived.contains(QStringLiteral("vae")))
        files.context.insert(QStringLiteral("vae_path"), path(derived.value(QStringLiteral("vae"))));
    for (const QJsonValue &step : derived.value(QStringLiteral("apply")).toArray()) {
        const QJsonObject apply = step.toObject();
        files.loras.append(QJsonObject{{"path", path(apply.value(QStringLiteral("lora")))},
                                       {"multiplier", apply.value(QStringLiteral("strength")).toDouble(1)}});
    }
    if (missing)
        return *missing;
    if (const std::optional<Error> mismatch = partsMismatch(derived, files.context, {input, input, input, input}, files.loras))
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

    static LatentMeta of(const Value &latent) {
        const QJsonObject meta = latent.json().toObject().value(QStringLiteral("meta")).toObject();
        return {meta.value(QStringLiteral("width")).toInt(), meta.value(QStringLiteral("height")).toInt(),
                meta.value(QStringLiteral("model")).toObject(), meta.value(QStringLiteral("empty")).toBool()};
    }
    QJsonObject toJson() const {
        QJsonObject meta{{"width", width}, {"height", height}};
        if (!model.isEmpty())
            meta.insert(QStringLiteral("model"), model);
        if (empty)
            meta.insert(QStringLiteral("empty"), true);
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

// A latent the helper wrote for a stage of `model`, at `width` x `height`.
Result<Values> latentOutput(const JobResult &result, int width, int height, const QJsonValue &model) {
    const Result<QString> path = written(result, QStringLiteral("latent"));
    if (!path)
        return path.errors();
    const LatentMeta made{width, height, model.toObject(), false};
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
        const QJsonObject paths{{"diffusion_model_path", path("diffusion_model")},
                                {"llm_path", path("text_encoder")},
                                {"vae_path", path("vae")}};
        const QJsonObject derived{{"base", context.input(QStringLiteral("diffusion_model")).json()},
                                  {"text_encoder", context.input(QStringLiteral("text_encoder")).json()},
                                  {"vae", context.input(QStringLiteral("vae")).json()}};
        // Blamed on the part that does not fit, where its picker is.
        if (const std::optional<Error> mismatch =
                partsMismatch(derived, paths,
                              {QStringLiteral("diffusion_model"), QStringLiteral("text_encoder"), QStringLiteral("vae"), QString()}))
            return {*mismatch};
        return {};
    }

    Result<Values> run(const NodeContext &context) override {
        const QJsonObject derived{{"base", context.input(QStringLiteral("diffusion_model")).json()},
                                  {"text_encoder", context.input(QStringLiteral("text_encoder")).json()},
                                  {"vae", context.input(QStringLiteral("vae")).json()}};
        return Values{{QStringLiteral("model"), Value(modelHandle(derived))}};
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

// A size and nothing else: the file holds it, so its hash differs by size.
class EmptyLatentKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        LatentMeta size;
        size.width = int(context.input(QStringLiteral("width")).number());
        size.height = int(context.input(QStringLiteral("height")).number());
        size.empty = true;
        const QByteArray bytes = QJsonDocument(QJsonObject{{"width", size.width}, {"height", size.height}})
                                     .toJson(QJsonDocument::Compact);
        const QString path = QDir(context.workDir).filePath(QStringLiteral("latent.json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
            return makeError(QStringLiteral("node_failed"), QStringLiteral("Cannot write %1.").arg(path));
        return Values{{QStringLiteral("latent"), Value(dataHandle(kLatentType, path, size.toJson()))}};
    }
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
    static Result<ModelFiles> modelInput(const NodeContext &context) {
        return modelFiles(context.input(QStringLiteral("model")).json().toObject(), QStringLiteral("model"), context);
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

    Result<JobRequest> request(const NodeContext &context, const Selection &) override {
        const Result<ModelFiles> files = modelInput(context);
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
        return job(*files, params, true);
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        const Result<QString> path = written(result, QStringLiteral("conditioning"));
        if (!path)
            return path.errors();
        QJsonObject descriptor;
        if (const QList<MediaHandle> references = referencesInput(context); !references.isEmpty()) {
            QJsonArray carried;
            for (const MediaHandle &image : references)
                carried.append(image.toJson());
            descriptor.insert(QStringLiteral("references"), carried);
        }
        return Values{{QStringLiteral("conditioning"), Value(dataHandle(kConditioningType, path.value(), descriptor))}};
    }

private:
    static QList<MediaHandle> referencesInput(const NodeContext &context) {
        QList<MediaHandle> references;
        for (const Value &item : context.input(QStringLiteral("references")).items())
            references.append(item.media());
        return references;
    }
};

class SamplerKernel : public StageKernel {
public:
    SamplerKernel() : StageKernel(QStringLiteral("latent.sample")) {}

    Result<JobRequest> request(const NodeContext &context, const Selection &) override {
        const Result<ModelFiles> files = modelInput(context);
        if (!files)
            return files.errors();
        const Value latent = context.input(QStringLiteral("latent"));
        const LatentMeta from = LatentMeta::of(latent);
        QJsonObject params{{"width", from.width},
                           {"height", from.height},
                           {"seed", context.input(QStringLiteral("seed")).json().toInteger()},
                           {"sample.steps", int(context.input(QStringLiteral("steps")).number())},
                           {"sample.guidance.txt_cfg", context.input(QStringLiteral("guidance")).number()}};
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
        JobRequest request = job(*files, params, true);
        request.inputs.insert(QStringLiteral("conditioning"), fileOf(context.input(QStringLiteral("conditioning"))));
        if (!from.empty) { // redrawn from a latent, by the amount denoise sets
            request.inputs.insert(QStringLiteral("init_latent"), fileOf(latent));
            request.params.insert(QStringLiteral("strength"), context.input(QStringLiteral("denoise")).number());
        }
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        const LatentMeta from = LatentMeta::of(context.input(QStringLiteral("latent")));
        return latentOutput(result, from.width, from.height, context.input(QStringLiteral("model")).json());
    }
};

class VaeDecodeKernel : public StageKernel {
public:
    VaeDecodeKernel() : StageKernel(QStringLiteral("latent.decode")) {}

    Result<JobRequest> request(const NodeContext &context, const Selection &) override {
        const Value latent = context.input(QStringLiteral("latent"));
        const LatentMeta sampled = LatentMeta::of(latent);
        if (sampled.model.isEmpty())
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("latent"),
                                  QStringLiteral("An Empty Latent has no image in it yet."),
                                  QStringLiteral("Sample it first: wire it through a Sampler."));
        const Result<ModelFiles> files = modelFiles(sampled.model, QStringLiteral("latent"), context);
        if (!files)
            return files.errors();
        JobRequest request = job(*files, QJsonObject{{"width", sampled.width}, {"height", sampled.height}});
        request.inputs.insert(QStringLiteral("latent"), fileOf(latent));
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        const Result<QString> path = written(result, QStringLiteral("image"));
        if (!path)
            return path.errors();
        const Result<MediaHandle> image = MediaHandle::probe(MediaType::Image, path.value(), *context.hashes);
        if (!image)
            return image.errors();
        return Values{{QStringLiteral("image"), Value(image.value())}};
    }
};

class VaeEncodeKernel : public StageKernel {
public:
    VaeEncodeKernel() : StageKernel(QStringLiteral("latent.encode")) {}

    Result<JobRequest> request(const NodeContext &context, const Selection &) override {
        const Result<ModelFiles> files = modelInput(context);
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
        return latentOutput(result, size.width(), size.height(), context.input(QStringLiteral("model")).json());
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
    kernels.add(QStringLiteral("fugue/text-encode"), std::make_shared<TextEncodeKernel>());
    kernels.add(QStringLiteral("fugue/sampler"), std::make_shared<SamplerKernel>());
    kernels.add(QStringLiteral("fugue/vae-decode"), std::make_shared<VaeDecodeKernel>());
    kernels.add(QStringLiteral("fugue/vae-encode"), std::make_shared<VaeEncodeKernel>());
}

} // namespace fugue::nodes
