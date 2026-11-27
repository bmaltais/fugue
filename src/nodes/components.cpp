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
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

#include <vips/vips8>

#include "engine/ffmpeg.h"
#include "engine/file_hashes.h"
#include "engine/media.h"
#include "engine/model_catalog.h"
#include "engine/model_library.h"
#include "engine/model_shape.h"
#include "engine/vips.h"
#include "nodes/denoise_cut.h"
#include "nodes/detail_ops.h"
#include "nodes/image_cover.h"
#include "nodes/pixel_io.h"
#include "nodes/resample.h"

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
    QString diffusionModel, textEncoder, vae, lora, highNoise, clipL;
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
                                                   {QStringLiteral("high_noise_diffusion_model_path"), inputs.highNoise},
                                                   {QStringLiteral("clip_l_path"), inputs.clipL}};
    const QStringList partNames{QStringLiteral("base"), QStringLiteral("text_encoder"), QStringLiteral("vae"),
                                QStringLiteral("high_noise"), QStringLiteral("clip_l")};
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
    const auto familyName = [](const QString &family) {
        return family == QLatin1String("qwen3.5") ? QStringLiteral("Qwen3.5") : family == QLatin1String("qwen2.5") ? QStringLiteral("Qwen2.5")
                                                                                : QStringLiteral("Qwen3");
    };
    const auto kind = [](TextKind k) {
        return k == TextKind::T5 ? QStringLiteral("a T5 encoder") : k == TextKind::Clip ? QStringLiteral("a CLIP encoder")
                                                                                        : QStringLiteral("a language model");
    };
    const std::optional<ModelShape> encoder = shapes.of(paths.value(encoderKey).toString());
    if (encoder && loadableTextEncoderFits(*diffusion, *encoder) == Fit::DoesNotFit) {
        const TextKind is = encoder->textKind.value_or(TextKind::Llm);
        const TextKind reads = diffusion->textKind.value_or(TextKind::Llm);
        const QString why = is == TextKind::Clip
                                ? QStringLiteral("it is %1, which goes in the CLIP-L input; this one takes %2.").arg(kind(is), kind(reads))
                            : is == TextKind::T5 && reads == TextKind::T5 && !loadsT5(*diffusion)
                                ? QStringLiteral("it is %1, and Load Models loads the T5 of FLUX.1 and Wan only.").arg(kind(is))
                            : is == TextKind::T5 && reads != TextKind::T5
                                ? QStringLiteral("it is %1, and the model reads %2.").arg(kind(is), kind(reads))
                            : !diffusion->llmFamily.isEmpty() && !encoder->llmFamily.isEmpty() && diffusion->llmFamily != encoder->llmFamily
                                ? QStringLiteral("it is a %1 encoder, and the model reads %2.").arg(familyName(encoder->llmFamily), familyName(diffusion->llmFamily))
                            : is != diffusion->textKind.value_or(TextKind::Llm)
                                ? QStringLiteral("it is %1, and the model reads %2.")
                                      .arg(kind(is), kind(diffusion->textKind.value_or(TextKind::Llm)))
                                : QStringLiteral("it is %1 wide, and the model reads %2.")
                                      .arg(encoder->hiddenSize.value_or(0))
                                      .arg(diffusion->textWidth.value_or(0));
        return mismatch(inputs.textEncoder, QStringLiteral("The text encoder %1 does not fit %2: %3").arg(name("text_encoder"), name("base"), why),
                        QStringLiteral("Pick another in Load Models: its list shows the text encoders that fit first."));
    }
    // FLUX.1 reads a CLIP-L beside its T5; a model that reads none is given none.
    const bool hasClip = paths.contains(QStringLiteral("clip_l_path")) && !paths.value(QStringLiteral("clip_l_path")).toString().isEmpty();
    if (diffusion->clipWidth && diffusion->textKind == TextKind::T5 && !hasClip && !inputs.clipL.isEmpty())
        return mismatch(inputs.clipL, QStringLiteral("%1 reads a CLIP-L text encoder too, and none is set.").arg(name("base")),
                        QStringLiteral("Pick the CLIP-L encoder (clip_l) in Load Models."));
    if (hasClip) {
        const std::optional<ModelShape> clip = shapes.of(paths.value(QStringLiteral("clip_l_path")).toString());
        if (clip && loadableClipFits(*diffusion, *clip) == Fit::DoesNotFit) {
            const QString why = clip->textKind.value_or(TextKind::Clip) != TextKind::Clip
                                    ? QStringLiteral("it is %1, not a CLIP encoder.").arg(kind(clip->textKind.value_or(TextKind::Llm)))
                                : !diffusion->clipWidth
                                    ? QStringLiteral("the model reads no CLIP encoder.")
                                    : QStringLiteral("it is %1 wide, and the model reads %2.")
                                          .arg(clip->hiddenSize.value_or(0))
                                          .arg(*diffusion->clipWidth);
            return mismatch(inputs.clipL, QStringLiteral("The CLIP encoder %1 does not fit %2: %3").arg(name("clip_l"), name("base"), why),
                            QStringLiteral("Pick another in Load Models: its list shows the CLIP encoders that fit first."));
        }
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

// The tokenizer the engine is given, found on this machine: the companion of a model that reads the Qwen3.5 tokenizer
// (#544), the one Load Models put in the model or, for a model made before that, the catalog's. A model that reads none is
// given none. model_missing with the way to get it (Get ready) when it is not here; input_invalid for a path the engine
// would read as a list of settings ("main=FILE,clip-l=FILE").
std::optional<Error> addTokenizer(QJsonObject &context, const QJsonObject &derived, const ModelLibrary &library, FileHashes &hashes) {
    QJsonObject handle = derived.value(QStringLiteral("tokenizer")).toObject();
    if (handle.isEmpty()) {
        const QString base = context.contains(QStringLiteral("diffusion_model_path")) ? context.value(QStringLiteral("diffusion_model_path")).toString()
                                                                                       : context.value(QStringLiteral("model_path")).toString();
        handle = companionTokenizer(base);
    }
    if (handle.isEmpty())
        return std::nullopt;
    const Result<QString> found = library.resolve(handle, hashes);
    if (!found) {
        const CatalogModel *entry = ModelCatalog::load().find(QStringLiteral("qwen3.5-tokenizer"));
        if (entry && entry->file == handle.value(QStringLiteral("name")).toString())
            return makeError(QStringLiteral("model_missing"),
                             QStringLiteral("The MOSS Image model from Load Models reads the official Qwen3.5 tokenizer (%1, %2), and it is not in your tokenizers folder.")
                                 .arg(entry->file, licenseLine(*entry)),
                             QStringLiteral("Click Get ready at the top of the workflow to download it."));
        return found.error();
    }
    if (found.value().contains(QLatin1Char('=')) || found.value().contains(QLatin1Char(',')))
        return makeError(QStringLiteral("input_invalid"),
                         QStringLiteral("The tokenizer file %1 has a \"=\" or a \",\" in its path, which the engine reads as a setting.").arg(found.value()),
                         QStringLiteral("Move or rename the file."));
    context.insert(QStringLiteral("tokenizer"), found.value());
    return std::nullopt;
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
    if (derived.contains(QStringLiteral("clip_l")))
        files.context.insert(QStringLiteral("clip_l_path"), path(derived.value(QStringLiteral("clip_l"))));
    if (derived.contains(QStringLiteral("vae")))
        files.context.insert(QStringLiteral("vae_path"), path(derived.value(QStringLiteral("vae"))));
    if (derived.contains(QStringLiteral("tokenizer")) && !shares) // a paired host finds it in its own folders
        files.context.insert(QStringLiteral("tokenizer"), path(derived.value(QStringLiteral("tokenizer"))));
    for (const QJsonValue &step : derived.value(QStringLiteral("apply")).toArray()) {
        const QJsonObject apply = step.toObject();
        files.loras.append(QJsonObject{{"path", path(apply.value(QStringLiteral("lora")))},
                                       {"multiplier", apply.value(QStringLiteral("strength")).toDouble(1)}});
    }
    if (missing)
        return *missing;
    if (!shares)
        return files;
    if (const std::optional<Error> mismatch = partsMismatch(derived, files.context, {input, input, input, input, {}, input}, files.loras))
        return *mismatch;
    // After the parts' check: a text encoder of the wrong family is the first thing to say.
    if (const std::optional<Error> absent = addTokenizer(files.context, derived, library, *context.hashes)) {
        Error error = *absent;
        error.input = input;
        return error;
    }
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
    // A latent VAE Encode made with a mask (14.4): the picture it encoded and the mask, as media
    // handles. The Sampler sends them to the helper again, so a latent that was sampled has none.
    QJsonObject image;
    QJsonObject mask;

    bool isVideo() const { return frames > 0; }
    bool hasMask() const { return !mask.isEmpty(); }

    static LatentMeta of(const Value &latent) {
        const QJsonObject meta = latent.json().toObject().value(QStringLiteral("meta")).toObject();
        return {meta.value(QStringLiteral("width")).toInt(), meta.value(QStringLiteral("height")).toInt(),
                meta.value(QStringLiteral("model")).toObject(), meta.value(QStringLiteral("empty")).toBool(),
                meta.value(QStringLiteral("frames")).toInt(), meta.value(QStringLiteral("fps")).toInt(),
                meta.value(QStringLiteral("image")).toObject(), meta.value(QStringLiteral("mask")).toObject()};
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
        if (hasMask()) {
            meta.insert(QStringLiteral("image"), image);
            meta.insert(QStringLiteral("mask"), mask);
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

// How the engine sizes the references before the VAE: "auto" is its own (each scaled to at most a
// megapixel and the output's area, to a multiple of 16), "exact" keeps each at the size the graph
// gave it (the engine's ref_image_args). Text Encode's input; the conditioning carries it to the
// Sampler, so both stages send the same one. Auto sends nothing: the request is as it was.
constexpr auto kExactReferences = "exact";
constexpr auto kExactReferenceArgs = "resize_before_vae=false";

void sendReferenceSize(QJsonObject &params, const QString &size) {
    if (size == QLatin1String(kExactReferences))
        params.insert(QStringLiteral("ref_image_args"), QLatin1String(kExactReferenceArgs));
}

// The layout of a MOSS-Image edit (the engine's moss_edit_format): "default" is the engine's own
// (the ai-toolkit layout, vision on when the encoder has it), "Moss-Image-Edit" is ComfyUI's
// TextEncodeMOSSImageEdit format of that name (VAE references only). Other models ignore it. Text
// Encode's input; the conditioning carries it to the Sampler. Default sends nothing.
constexpr auto kMossImageEditFormat = "Moss-Image-Edit";
constexpr auto kMossImageEditEngineFormat = "moss-image-edit";

void sendEditFormat(QJsonObject &params, const QString &format) {
    if (format == QLatin1String(kMossImageEditFormat))
        params.insert(QStringLiteral("moss_edit_format"), QLatin1String(kMossImageEditEngineFormat));
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

// --- a mask on a latent (14.4) ---

// The first channel of `mask` as floats from 0 to 1 (a mask is an image, 2.1; gray is its own).
Result<vips::VImage> maskValues(const MediaHandle &mask) {
    if (!initVips())
        return vipsUnavailable();
    const Result<vips::VImage> loaded = mask.image();
    if (!loaded)
        return loaded.errors();
    try {
        double divisor = 1.0;
        switch (loaded->format()) {
        case VIPS_FORMAT_UCHAR:
        case VIPS_FORMAT_CHAR:
            divisor = 255.0;
            break;
        case VIPS_FORMAT_USHORT:
        case VIPS_FORMAT_SHORT:
            divisor = 65535.0;
            break;
        default:
            break;
        }
        return loaded->extract_band(0).cast(VIPS_FORMAT_FLOAT) / divisor;
    } catch (const vips::VError &error) {
        return vipsError(error, QStringLiteral("media_unreadable"));
    }
}

// What a mask holds: nothing painted, or all of it (the checks of 14.4).
struct MaskRange {
    bool empty = false;
    bool full = false;
};
Result<MaskRange> maskRange(const MediaHandle &mask) {
    const Result<vips::VImage> values = maskValues(mask);
    if (!values)
        return values.errors();
    try {
        MaskRange range;
        range.empty = values->max() <= 0.0;
        range.full = values->min() >= 0.999;
        return range;
    } catch (const vips::VError &error) {
        return vipsError(error, QStringLiteral("media_unreadable"));
    }
}

// `mask`'s first channel as the 8-bit gray PNG the helper reads, scaled to `size` by the nearest
// pixel (the library makes a mask 0 or 1 and scales it to the latent the same way: no soft edge
// is lost that it would keep).
Result<QString> maskToFile(const MediaHandle &mask, QSize size, const QString &path) {
    const Result<vips::VImage> values = maskValues(mask);
    if (!values)
        return values.errors();
    try {
        vips::VImage gray = (values.value() * 255.0 + 0.5).cast(VIPS_FORMAT_UCHAR);
        if (gray.width() != size.width() || gray.height() != size.height()) {
            gray = gray.resize(double(size.width()) / gray.width(),
                               vips::VImage::option()->set("vscale", double(size.height()) / gray.height())
                                   ->set("kernel", VIPS_KERNEL_NEAREST));
            if (gray.width() != size.width() || gray.height() != size.height())
                gray = gray.embed(0, 0, size.width(), size.height(), vips::VImage::option()->set("extend", VIPS_EXTEND_COPY));
        }
        gray.copy(vips::VImage::option()->set("interpretation", VIPS_INTERPRETATION_B_W))
            .write_to_file(QFile::encodeName(path).constData());
        return path;
    } catch (const vips::VError &error) {
        return vipsError(error, QStringLiteral("media_unreadable"));
    }
}

// The picture and mask a latent carries, each of which must still be the file VAE Encode read;
// none (invalid handles) for a latent without a mask.
struct CarriedMask {
    MediaHandle image;
    MediaHandle mask;
    bool present = false;
};
Result<CarriedMask> carriedMaskOf(const Value &latent, FileHashes &hashes) {
    const LatentMeta meta = LatentMeta::of(latent);
    CarriedMask carried;
    if (!meta.hasMask())
        return carried;
    const Result<MediaHandle> image = MediaHandle::fromJson(meta.image);
    if (!image)
        return image.errors();
    const Result<MediaHandle> mask = MediaHandle::fromJson(meta.mask);
    if (!mask)
        return mask.errors();
    for (const MediaHandle &file : {image.value(), mask.value()})
        if (hashes.hash(file.path()) != file.hash())
            return makeInputError(QStringLiteral("file_not_found"), QStringLiteral("latent"),
                                  QStringLiteral("The picture or mask %1 is gone or changed since VAE Encode read it.").arg(file.path()),
                                  QStringLiteral("Run VAE Encode again."));
    carried.image = image.value();
    carried.mask = mask.value();
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
        std::optional<ModelLibrary> own; // none is given outside a check pass
        const ModelLibrary &library = context.library ? *context.library : own.emplace(ModelLibrary::current(context.registry));
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
        if (hasClipL(context))
            paths.insert(QStringLiteral("clip_l_path"), path("clip_l"));
        // Blamed on the part that does not fit, where its picker is.
        if (const std::optional<Error> mismatch = partsMismatch(
                derived(context), paths,
                {QStringLiteral("diffusion_model"), QStringLiteral("text_encoder"), QStringLiteral("vae"), QString(),
                 QStringLiteral("high_noise_diffusion_model"), QStringLiteral("clip_l")}))
            return {*mismatch};
        // MOSS Image's tokenizer is a file of the model, listed missing like the model files (Get ready, #544).
        if (const QJsonObject tokenizer = companionTokenizer(paths.value(QStringLiteral("diffusion_model_path")).toString()); !tokenizer.isEmpty()) {
            const Result<QString> found = library.resolve(tokenizer, *context.hashes, ModelLibrary::Hashing::RememberedOnly);
            if (!found) {
                Error missing = found.error();
                missing.input = QStringLiteral("diffusion_model");
                missing.message = QStringLiteral("MOSS Image reads the official Qwen3.5 tokenizer (%1), and it is not in your tokenizers folder.")
                                      .arg(tokenizer.value(QStringLiteral("name")).toString());
                missing.hint = QStringLiteral("Click Get ready at the top of the workflow to download it.");
                return {missing};
            }
        }
        return {};
    }

    Result<Values> run(const NodeContext &context) override {
        QJsonObject parts = derived(context);
        // A model that reads the Qwen3.5 tokenizer: the catalog's goes in the model as its companion file, so what Text
        // Encode caches depends on it (#542) and a paired host is sent its handle (#544). A saved tokenizer input of
        // Load Models 1.1.0 is a retired input: the compiler drops it.
        const ModelLibrary library = ModelLibrary::current(context.registry);
        const Result<QString> base = library.resolve(context.input(QStringLiteral("diffusion_model")).json().toObject(), *context.hashes,
                                                     ModelLibrary::Hashing::RememberedOnly);
        if (const QJsonObject handle = companionTokenizer(base ? base.value() : QString()); !handle.isEmpty())
            parts.insert(QStringLiteral("tokenizer"), handle);
        return Values{{QStringLiteral("model"), Value(modelHandle(parts))}};
    }

private:
    static bool hasHighNoise(const NodeContext &context) {
        return !context.input(QStringLiteral("high_noise_diffusion_model")).isEmpty();
    }
    static bool hasClipL(const NodeContext &context) { return !context.input(QStringLiteral("clip_l")).isEmpty(); }
    static QJsonObject derived(const NodeContext &context) {
        QJsonObject parts{{"base", context.input(QStringLiteral("diffusion_model")).json()},
                          {"text_encoder", context.input(QStringLiteral("text_encoder")).json()},
                          {"vae", context.input(QStringLiteral("vae")).json()}};
        if (hasClipL(context))
            parts.insert(QStringLiteral("clip_l"), context.input(QStringLiteral("clip_l")).json());
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

    // The picture it will make, for the size shown before a run (#601).
    PictureSizes outputSizes(const NodeContext &context) const override {
        const Value width = context.input(QStringLiteral("width")), height = context.input(QStringLiteral("height"));
        if (width.isEmpty() || height.isEmpty())
            return {};
        return {{QStringLiteral("latent"), QSize(int(width.number()), int(height.number()))}};
    }

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
            sendReferenceSize(params, context.input(QStringLiteral("reference_size")).text());
            sendEditFormat(params, context.input(QStringLiteral("edit_format")).text());
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
            if (const QString size = context.input(QStringLiteral("reference_size")).text(); size == QLatin1String(kExactReferences))
                descriptor.insert(QStringLiteral("reference_size"), size);
            if (const QString format = context.input(QStringLiteral("edit_format")).text(); format == QLatin1String(kMossImageEditFormat))
                descriptor.insert(QStringLiteral("edit_format"), format);
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

constexpr const char *kScheduleCut = "schedule_cut";

class SamplerKernel : public StageKernel {
public:
    SamplerKernel() : StageKernel(QStringLiteral("latent.sample")) {}

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        const Result<ModelFiles> files = modelInput(context, selection);
        if (!files)
            return files.errors();
        const Value latent = context.input(QStringLiteral("latent"));
        const LatentMeta from = LatentMeta::of(latent);
        const int steps = int(context.input(QStringLiteral("steps")).number());
        const double denoise = context.input(QStringLiteral("denoise")).number();
        // The schedule cut applies to a start latent only: an Empty Latent keeps its steps.
        std::optional<DenoiseCut> cut;
        if (context.input(QStringLiteral("denoise_mode")).text() == QLatin1String(kScheduleCut)) {
            if (from.isVideo())
                return makeInputError(QStringLiteral("node_failed"), QStringLiteral("denoise_mode"),
                                      QStringLiteral("A video Sampler has no schedule cut: Wan's high-noise split counts steps."),
                                      QStringLiteral("Leave Denoise mode unset, or use noise_level."));
            if (!from.empty && denoise <= 0.0)
                return makeInputError(QStringLiteral("node_failed"), QStringLiteral("denoise"),
                                      QStringLiteral("A schedule cut at denoise 0 has no step to run."),
                                      QStringLiteral("Raise Denoise above 0, or keep the picture as it is without a Sampler."));
            if (!from.empty)
                cut = denoiseCut(steps, denoise);
        }
        QJsonObject params{{"width", from.width},
                           {"height", from.height},
                           {"seed", context.input(QStringLiteral("seed")).json().toInteger()},
                           {"sample.steps", steps},
                           {"sample.guidance.txt_cfg", context.input(QStringLiteral("guidance")).number()}};
        if (from.isVideo()) {
            params.insert(QStringLiteral("video_frames"), from.frames);
            params.insert(QStringLiteral("fps"), from.fps);
        }
        // Variation seeds (#553): sent only when it changes the picture, so a workflow without it keeps its request.
        if (const double variation = context.input(QStringLiteral("variation_strength")).number(); variation > 0.0) {
            if (from.isVideo())
                return makeInputError(QStringLiteral("node_failed"), QStringLiteral("variation_strength"),
                                      QStringLiteral("A video Sampler has no variation seed: the engine varies the start noise of pictures only."),
                                      QStringLiteral("Set Variation strength to 0 for a clip."));
            params.insert(QStringLiteral("fugue.variation_strength"), variation);
            params.insert(QStringLiteral("fugue.variation_seed"), context.input(QStringLiteral("variation_seed")).json().toInteger());
        }
        if (const Value method = context.input(QStringLiteral("sampler")); !method.isEmpty())
            params.insert(QStringLiteral("sample.method"), method.text());
        if (const Value scheduler = context.input(QStringLiteral("scheduler")); !scheduler.isEmpty())
            params.insert(QStringLiteral("sample.scheduler"), scheduler.text());
        // Only for a schedule that reads it (not Klein's own flux2 one): the engine's default when empty.
        if (const Value shift = context.input(QStringLiteral("flow_shift")); !shift.isEmpty())
            params.insert(QStringLiteral("sample.flow_shift"), shift.number());
        // The edit's references, sent as Text Encode sent them, so both stages make one generation.
        const Result<QList<MediaHandle>> references = referencesOf(context.input(QStringLiteral("conditioning")), *context.hashes);
        if (!references)
            return references.errors();
        if (!references->isEmpty()) {
            const Result<QJsonArray> sent = refImages(references.value(), context.workDir);
            if (!sent)
                return sent.errors();
            params.insert(QStringLiteral("ref_images"), sent.value());
            sendReferenceSize(params, context.input(QStringLiteral("conditioning")).json().toObject().value(QStringLiteral("meta"))
                                          .toObject().value(QStringLiteral("reference_size")).toString());
            sendEditFormat(params, context.input(QStringLiteral("conditioning")).json().toObject().value(QStringLiteral("meta"))
                                       .toObject().value(QStringLiteral("edit_format")).toString());
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
        // A latent VAE Encode made with a mask: the mask at the latent's size and the picture, for
        // the helper to use as the model needs (an inpaint model the picture, any other the latent).
        const Result<CarriedMask> masked = carriedMaskOf(latent, *context.hashes);
        if (!masked)
            return masked.errors();
        JobRequest request = job(*files, params, true);
        request.inputs.insert(QStringLiteral("conditioning"), fileOf(context.input(QStringLiteral("conditioning"))));
        if (masked->present) {
            const Result<QString> mask = maskToFile(masked->mask, QSize(from.width, from.height),
                                                    QDir(context.workDir).filePath(QStringLiteral("mask.png")));
            if (!mask)
                return mask.errors();
            const Result<QString> picture = readable(masked->image, QDir(context.workDir).filePath(QStringLiteral("masked-image.png")));
            if (!picture)
                return picture.errors();
            request.inputs.insert(QStringLiteral("mask"), QJsonObject{{"path", mask.value()}});
            request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", picture.value()}});
            // Differential diffusion (#179): the helper keeps the mask's gray as a strength.
            if (context.input(QStringLiteral("soft_mask")).json().toBool())
                request.params.insert(QStringLiteral("fugue.soft_mask"), true);
        }
        if (carried->present) {
            const Result<QString> frame = firstFrameFile(carried->frame, from, context.workDir, QStringLiteral("conditioning"));
            if (!frame)
                return frame.errors();
            request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", frame.value()}});
        }
        if (!from.empty) { // redrawn from a latent, by the amount denoise sets
            request.inputs.insert(QStringLiteral("init_latent"), fileOf(latent));
            request.params.insert(QStringLiteral("strength"), denoise);
            if (cut) { // ComfyUI's reading: a longer schedule, of which the last `steps` are run
                request.params.insert(QStringLiteral("sample.steps"), cut->steps);
                request.params.insert(QStringLiteral("strength"), cut->strength);
                request.params.insert(QStringLiteral("sample.args"), QStringLiteral("strength_as_noise_level=false"));
            }
        }
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        LatentMeta sampled = LatentMeta::of(context.input(QStringLiteral("latent")));
        sampled.image = {}; // the result is a new picture: the mask is spent
        sampled.mask = {};
        return latentOutput(result, sampled, context.input(QStringLiteral("model")).json());
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
        // A mask is not sent: it stays with the latent for the Sampler. It is checked here.
        if (const Value mask = context.input(QStringLiteral("mask")); !mask.isEmpty()) {
            if (!mask.isMedia() || mask.media().width() != image.width() || mask.media().height() != image.height())
                return makeInputError(QStringLiteral("node_failed"), QStringLiteral("mask"),
                                      QStringLiteral("The mask is not the size of the picture (%1 x %2).").arg(image.width()).arg(image.height()),
                                      QStringLiteral("Paint or make the mask on this picture."));
            const Result<MaskRange> range = maskRange(mask.media());
            if (!range)
                return range.errors();
            if (range->empty)
                return makeInputError(QStringLiteral("mask_empty"), QStringLiteral("mask"),
                                      QStringLiteral("Nothing is painted in the mask."),
                                      QStringLiteral("Paint the part to change, or leave the mask unwired."));
        }
        JobRequest request = job(*files, QJsonObject{{"width", size.width()}, {"height", size.height()}});
        request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", file.value()}});
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        const QSize size = sizeOf(context.input(QStringLiteral("image")).media());
        LatentMeta meta;
        meta.width = size.width();
        meta.height = size.height();
        Errors warnings;
        if (const Value mask = context.input(QStringLiteral("mask")); !mask.isEmpty()) {
            // The Sampler needs both again: the latent carries them as handles.
            meta.image = context.input(QStringLiteral("image")).media().toJson();
            meta.mask = mask.media().toJson();
            const Result<MaskRange> range = maskRange(mask.media());
            if (!range)
                return range.errors();
            if (range->full) {
                Error full = makeInputError(QStringLiteral("mask_full"), QStringLiteral("mask"),
                                            QStringLiteral("Everything is masked: this redraws the whole picture, as plain image to image."),
                                            QStringLiteral("Paint only the part to change."));
                full.severity = Error::Severity::Warning;
                warnings << full;
            }
        }
        const Result<Values> made = latentOutput(result, meta, context.input(QStringLiteral("model")).json());
        if (!made || warnings.isEmpty())
            return made;
        return Result<Values>(made.value(), warnings);
    }

private:
    static QSize sizeOf(const MediaHandle &image) {
        const auto step = [](int side) { return std::max(1, int(std::lround(double(side) / kSizeStep))) * kSizeStep; };
        return {step(image.width()), step(image.height())};
    }

};

// --- Detail Regions (#345) ---

// `pixels` as a PNG at `path`: 8-bit, or 16-bit for the canvas that stays between boxes so the
// pastes do not round it again each time.
Result<QString> writePixels(const Pixels &pixels, const QString &path, bool sixteen) {
    if (!initVips())
        return vipsUnavailable();
    try {
        const vips::VImage image = vips::VImage::new_from_memory_copy(
            pixels.data.data(), pixels.data.size() * sizeof(float), pixels.width, pixels.height, pixels.channels, VIPS_FORMAT_FLOAT);
        const vips::VImage clamped = image.clamp(vips::VImage::option()->set("min", 0.0)->set("max", 1.0));
        const vips::VImage whole = sixteen ? (clamped * 65535.0 + 0.5).cast(VIPS_FORMAT_USHORT) : (clamped * 255.0 + 0.5).cast(VIPS_FORMAT_UCHAR);
        whole.copy(vips::VImage::option()->set("interpretation", sixteen ? VIPS_INTERPRETATION_RGB16 : VIPS_INTERPRETATION_sRGB))
            .write_to_file(QFile::encodeName(path).constData());
        return path;
    } catch (const vips::VError &error) {
        return vipsError(error, QStringLiteral("node_failed"));
    }
}

// One box of the picture to redraw: the crop, the size it is redrawn at, and what its pass uses.
struct DetailStep {
    int index = 0; // the box's place in the list given (skipped ones count)
    CropBox crop;
    CropSize size;
    double denoise = 0;
    qint64 seed = 0;
};

// Detail Regions: every box of `boxes` redrawn on its own, one after the other. The node runs the
// model three times a box (VAE encode, sample, VAE decode: the very requests the VAE Encode,
// Sampler and VAE Decode nodes send, made by those kernels) as a node of several jobs
// (ProviderKernel::next), and pastes each result back before the next box is cut, so a box sees
// the picture as the one before left it. What stays between the jobs is in the node's folder:
// detail.json (the box being done, its stage and the crop sent), canvas.png (the picture so far,
// 16 bits) and the stage files. A box list with nothing to do asks for no job (an empty
// capability), keeps nothing and gives the picture back as it came.
class DetailRegionsKernel : public StageKernel {
public:
    DetailRegionsKernel() : StageKernel(QStringLiteral("latent.sample")) {}

    // Under validate: the boxes written are one `x1 y1 x2 y2` a line. Boxes that are wired are the run's.
    Errors check(const NodeContext &context) const override {
        if (!context.inputs.contains(QStringLiteral("boxes")))
            return {};
        const Result<std::vector<Corners>> boxes = boxesOf(context);
        return boxes ? Errors{} : boxes.errors();
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        const Result<QList<DetailStep>> steps = plan(context);
        if (!steps)
            return steps.errors();
        if (steps->isEmpty()) // nothing to redraw: no model call, the picture as it is
            return JobRequest();
        QDir(context.workDir).mkpath(QStringLiteral("."));
        clean(context, true); // a retry after out_of_memory starts over
        const Result<Pixels> canvas = canvasPixels(context);
        if (!canvas)
            return canvas.errors();
        QJsonObject state{{"pos", 0}, {"phase", "encode"}};
        Result<JobRequest> job = encodeJob(context, selection, steps.value(), 0, canvas.value(), state);
        if (!job)
            return job.errors();
        if (const std::optional<Error> failed = save(context, state))
            return *failed;
        return job;
    }

    Result<std::optional<JobRequest>> next(const NodeContext &context, const Selection &selection, const JobResult &result) override {
        const Result<QList<DetailStep>> steps = plan(context);
        if (!steps)
            return steps.errors();
        if (steps->isEmpty())
            return std::optional<JobRequest>();
        if (context.cancelled && context.cancelled())
            return makeError(QStringLiteral("cancelled"), QStringLiteral("The run was cancelled."));
        QJsonObject state = load(context);
        const int pos = state.value(QStringLiteral("pos")).toInt(-1);
        const QString phase = state.value(QStringLiteral("phase")).toString();
        if (pos < 0 || pos >= steps->size())
            return makeError(QStringLiteral("node_failed"), QStringLiteral("Detail Regions lost its place: its state file is gone."));
        const DetailStep &step = steps->at(pos);

        if (phase == QLatin1String("encode") || phase == QLatin1String("sample")) {
            // The stage's latent, kept under its own name: the next stage writes `latent` again.
            const bool encoded = phase == QLatin1String("encode");
            const Result<JobResult> kept = keep(result, QStringLiteral("latent"), context,
                                                encoded ? QStringLiteral("latent-encoded.bin") : QStringLiteral("latent-sampled.bin"));
            if (!kept)
                return kept.errors();
            NodeContext stage = context;
            stage.inputs = inputsFor(context, encoded ? QStringLiteral("encode") : QStringLiteral("sample"), step, state);
            if (encoded) { // the latent is of the crop, at the size VAE Encode read it
                const Result<MediaHandle> crop = MediaHandle::fromJson(state.value(QStringLiteral("crop")).toObject());
                if (!crop)
                    return crop.errors();
                stage.inputs.insert(QStringLiteral("image"), Value(crop.value()));
            }
            Result<Values> made = encoded ? VaeEncodeKernel().outputs(stage, selection, kept.value())
                                          : SamplerKernel().outputs(stage, selection, kept.value());
            if (!made)
                return made.errors();
            state.insert(QStringLiteral("latent"), made.value().value(QStringLiteral("latent")).json());
            if (!encoded)
                QFile::remove(QDir(context.workDir).filePath(QStringLiteral("latent-encoded.bin")));
            state.insert(QStringLiteral("phase"), encoded ? QStringLiteral("sample") : QStringLiteral("decode"));
            if (const std::optional<Error> failed = save(context, state))
                return *failed;
            Result<JobRequest> job = encoded ? sampleJob(context, selection, step, state, steps->size(), pos)
                                             : decodeJob(context, selection, state, steps->size(), pos);
            if (!job)
                return job.errors();
            return std::optional<JobRequest>(job.value());
        }

        // The decoded crop goes back into the picture.
        const Result<QString> file = written(result, QStringLiteral("image"));
        if (!file)
            return file.errors();
        const Result<Pixels> redrawn = readPixels(file.value());
        if (!redrawn)
            return redrawn.errors();
        Result<Pixels> canvas = canvasPixels(context);
        if (!canvas)
            return canvas.errors();
        Pixels &picture = canvas.value();
        pasteFeathered(picture, step.crop, resample(redrawn.value(), step.crop.width, step.crop.height, ResizeMethod::Bicubic),
                       toInt(context.input(QStringLiteral("feather"))));
        const Result<QString> wrote = writePixels(picture, canvasPath(context), true);
        if (!wrote)
            return wrote.errors();
        QFile::remove(file.value());
        QFile::remove(QDir(context.workDir).filePath(QStringLiteral("latent-sampled.bin")));
        if (context.progress)
            context.progress(double(pos + 1) / steps->size(), QStringLiteral("detail"),
                             QStringLiteral("box %1 of %2").arg(pos + 1).arg(steps->size()));
        if (pos + 1 >= steps->size())
            return std::optional<JobRequest>();
        QJsonObject nextState{{"pos", pos + 1}, {"phase", "encode"}};
        Result<JobRequest> job = encodeJob(context, selection, steps.value(), pos + 1, picture, nextState);
        if (!job)
            return job.errors();
        if (const std::optional<Error> failed = save(context, nextState))
            return *failed;
        return std::optional<JobRequest>(job.value());
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &) override {
        const Result<QList<DetailStep>> steps = plan(context);
        if (!steps)
            return steps.errors();
        Errors warnings;
        const qsizetype skipped = qsizetype(boxesOf(context)->size()) - steps->size();
        if (skipped > 0) {
            Error note = makeInputError(QStringLiteral("box_skipped"), QStringLiteral("boxes"),
                                        QStringLiteral("%1 of the boxes were left as they are: smaller than %2 pixels with the padding.")
                                            .arg(skipped).arg(kMinCropSide));
            note.severity = Error::Severity::Warning;
            warnings << note;
        }
        if (steps->isEmpty()) {
            clean(context, true);
            return Result<Values>(Values{{QStringLiteral("image"), context.input(QStringLiteral("image"))}}, warnings);
        }
        Result<Pixels> canvas = canvasPixels(context);
        if (!canvas)
            return canvas.errors();
        Result<Values> image = imageOutput(std::move(canvas.value()));
        clean(context, true);
        if (!image)
            return image.errors();
        return Result<Values>(image.value(), warnings);
    }

    void abandon(const NodeContext &context) override { clean(context, true); }

private:
    // Each box's share of the node's bar, and where each of its three jobs sits in it.
    static JobRequest placed(JobRequest job, int count, int pos, double from, double span) {
        job.progressFrom = (pos + from) / count;
        job.progressSpan = span / count;
        return job;
    }

    static QString inFolder(const NodeContext &context, const QString &name) { return QDir(context.workDir).filePath(name); }
    static QString canvasPath(const NodeContext &context) { return inFolder(context, QStringLiteral("canvas.png")); }

    // Everything the node leaves in its folder besides results: its own files, and the converted
    // references and the files of a job cut short that the stages leave.
    static void clean(const NodeContext &context, bool helperFiles) {
        for (const char *name : {"canvas.png", "detail.json", "decoded.png", "latent-encoded.bin", "latent-sampled.bin"})
            QFile::remove(inFolder(context, QLatin1String(name)));
        if (helperFiles)
            for (const char *name : {"latent.bin", "image.png"})
                QFile::remove(inFolder(context, QLatin1String(name)));
        for (const QString &name : QDir(context.workDir).entryList({QStringLiteral("crop-*.png"), QStringLiteral("reference-*.png")}, QDir::Files))
            QFile::remove(inFolder(context, name));
    }

    static QJsonObject load(const NodeContext &context) {
        QFile file(inFolder(context, QStringLiteral("detail.json")));
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    }
    // An error when the state cannot be kept: the next job could not find its place.
    static std::optional<Error> save(const NodeContext &context, const QJsonObject &state) {
        QFile file(inFolder(context, QStringLiteral("detail.json")));
        const QByteArray bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size() || !file.flush())
            return makeError(QStringLiteral("node_failed"), QStringLiteral("Cannot write %1: %2").arg(file.fileName(), file.errorString()));
        return std::nullopt;
    }

    // A picture file as pixels, with no media handle (no hash of a file made a moment ago).
    static Result<Pixels> readPixels(const QString &path) {
        if (!initVips())
            return vipsUnavailable();
        try {
            return pixelsFromImage(vips::VImage::new_from_file(QFile::encodeName(path).constData()), QStringLiteral("image"));
        } catch (const vips::VError &error) {
            return vipsError(error, QStringLiteral("media_unreadable"));
        }
    }

    // The picture so far: the canvas, or the input before the first paste.
    static Result<Pixels> canvasPixels(const NodeContext &context) {
        if (!QFileInfo::exists(canvasPath(context)))
            return pixelsOf(context.input(QStringLiteral("image")), QStringLiteral("image"));
        return readPixels(canvasPath(context));
    }

    // `output` of a finished job moved to `name` in the node's folder, and the result saying so.
    static Result<JobResult> keep(JobResult result, const QString &output, const NodeContext &context, const QString &name) {
        const Result<QString> file = written(result, output);
        if (!file)
            return file.errors();
        const QString to = inFolder(context, name);
        QFile::remove(to);
        if (!QFile::rename(file.value(), to))
            return makeError(QStringLiteral("node_failed"), QStringLiteral("Cannot move %1.").arg(file.value()));
        result.outputs.insert(output, QJsonObject{{"path", to}});
        return result;
    }

    static Result<std::vector<Corners>> boxesOf(const NodeContext &context) {
        std::string problem;
        const std::optional<std::vector<Corners>> boxes = parseBoxes(context.input(QStringLiteral("boxes")).text().toStdString(), &problem);
        if (!boxes)
            return makeInputError(QStringLiteral("input_invalid"), QStringLiteral("boxes"),
                                  QStringLiteral("Boxes: %1.").arg(QString::fromStdString(problem)),
                                  QStringLiteral("Wire Find by Text's Boxes here, or write one box a line as x1 y1 x2 y2."));
        return *boxes;
    }

    // The boxes that become a redraw, in order, with the size, denoise and seed of each.
    static Result<QList<DetailStep>> plan(const NodeContext &context) {
        const Result<std::vector<Corners>> boxes = boxesOf(context);
        if (!boxes)
            return boxes.errors();
        const MediaHandle image = context.input(QStringLiteral("image")).media();
        const int padding = toInt(context.input(QStringLiteral("padding")));
        const int target = toInt(context.input(QStringLiteral("target_size")));
        const qint64 seed = context.input(QStringLiteral("seed")).json().toInteger();
        const qint64 shift = toInt(context.input(QStringLiteral("seed_shift")));
        const QString mode = context.input(QStringLiteral("denoise_scale")).text();
        const DenoiseScale scale = mode == QLatin1String("smaller_less") ? DenoiseScale::SmallerLess
                                   : mode == QLatin1String("smaller_more") ? DenoiseScale::SmallerMore
                                                                           : DenoiseScale::Fixed;
        QList<DetailStep> steps;
        for (size_t i = 0; i < boxes->size(); ++i) {
            const Corners &box = boxes->at(i);
            const std::optional<CropBox> padded = detailCrop(box, padding, image.width(), image.height());
            if (!padded)
                continue;
            DetailStep step;
            step.index = int(i);
            step.crop = widenThin(*padded, target, image.width(), image.height());
            step.size = detailSize(step.crop, target, kSizeStep);
            step.denoise = scaledDenoise(context.input(QStringLiteral("denoise")).number(), scale,
                                         clampedArea(box, image.width(), image.height()), double(image.width()) * image.height());
            step.seed = detailSeed(seed, shift, int(i));
            steps.append(step);
        }
        return steps;
    }

    // The inputs the stage's own node would have got.
    static Values inputsFor(const NodeContext &context, const QString &stage, const DetailStep &step, const QJsonObject &state) {
        Values inputs{{QStringLiteral("model"), context.input(QStringLiteral("model"))}};
        if (stage == QLatin1String("encode"))
            return inputs; // the caller adds the crop as `image`
        inputs.insert(QStringLiteral("conditioning"), context.input(QStringLiteral("conditioning")));
        inputs.insert(QStringLiteral("latent"), Value(state.value(QStringLiteral("latent")).toObject()));
        inputs.insert(QStringLiteral("seed"), Value(step.seed));
        inputs.insert(QStringLiteral("steps"), context.input(QStringLiteral("steps")));
        inputs.insert(QStringLiteral("guidance"), context.input(QStringLiteral("guidance")));
        inputs.insert(QStringLiteral("denoise"), Value(step.denoise));
        // ComfyUI's reading, as the reference runs its detailers (a denoise below 1 keeps `steps` steps).
        inputs.insert(QStringLiteral("denoise_mode"), Value(QString::fromLatin1(kScheduleCut)));
        for (const char *name : {"sampler", "scheduler"})
            if (const Value value = context.input(QLatin1String(name)); !value.isEmpty())
                inputs.insert(QLatin1String(name), value);
        return inputs;
    }

    // The job that encodes box `pos`: the padded crop at its size, cut from `canvas` (the picture so
    // far, in memory). The crop's handle goes into `state` for the stage's outputs.
    Result<JobRequest> encodeJob(const NodeContext &context, const Selection &selection, const QList<DetailStep> &steps, int pos,
                                 const Pixels &canvas, QJsonObject &state) {
        const DetailStep &step = steps.at(pos);
        Pixels crop = cutBox(canvas, step.crop);
        if (crop.channels != 3) { // the model draws colour only: the alpha stays with the picture
            Pixels rgb(crop.width, crop.height, 3);
            for (int y = 0; y < crop.height; ++y)
                for (int x = 0; x < crop.width; ++x)
                    for (int c = 0; c < 3; ++c)
                        rgb.at(x, y, c) = crop.at(x, y, std::min(c, crop.channels - 1));
            crop = std::move(rgb);
        }
        const Pixels scaled = resample(crop, step.size.width, step.size.height, ResizeMethod::Bicubic);
        const QString file = inFolder(context, QStringLiteral("crop-%1.png").arg(pos));
        const Result<QString> wrote = writePixels(scaled, file, false);
        if (!wrote)
            return wrote.errors();
        const Result<MediaHandle> handle = MediaHandle::probe(MediaType::Image, file, *context.hashes);
        if (!handle)
            return handle.errors();
        state.insert(QStringLiteral("crop"), handle->toJson());
        NodeContext stage = context;
        stage.inputs = inputsFor(context, QStringLiteral("encode"), step, {});
        stage.inputs.insert(QStringLiteral("image"), Value(handle.value()));
        Result<JobRequest> job = VaeEncodeKernel().request(stage, selection);
        if (!job)
            return job.errors();
        return placed(job.value(), steps.size(), pos, 0.0, 0.1);
    }

    Result<JobRequest> sampleJob(const NodeContext &context, const Selection &selection, const DetailStep &step, const QJsonObject &state,
                                 int count, int pos) {
        NodeContext stage = context;
        stage.inputs = inputsFor(context, QStringLiteral("sample"), step, state);
        Result<JobRequest> job = SamplerKernel().request(stage, selection);
        if (!job)
            return job.errors();
        return placed(job.value(), count, pos, 0.1, 0.8);
    }

    Result<JobRequest> decodeJob(const NodeContext &context, const Selection &selection, const QJsonObject &state, int count, int pos) {
        NodeContext stage = context;
        stage.inputs = Values{{QStringLiteral("latent"), Value(state.value(QStringLiteral("latent")).toObject())}};
        Result<JobRequest> job = VaeDecodeKernel().request(stage, selection);
        if (!job)
            return job.errors();
        return placed(job.value(), count, pos, 0.9, 0.1);
    }
};

// The size, rate and length of a YUV4MPEG2 file (4:2:0), from its header and its size: the file the
// helper reads for a clip is the one ffmpeg made, so this is what the latent will say.
struct ClipFile {
    int width = 0;
    int height = 0;
    int fps = 0;
    int frames = 0;
};

std::optional<ClipFile> clipFileOf(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QByteArray header = file.readLine(1024);
    if (!header.startsWith("YUV4MPEG2"))
        return std::nullopt;
    ClipFile clip;
    int num = 0;
    int den = 1;
    for (const QByteArray &field : header.trimmed().split(' ')) {
        if (field.startsWith('W'))
            clip.width = field.mid(1).toInt();
        else if (field.startsWith('H'))
            clip.height = field.mid(1).toInt();
        else if (field.startsWith('F'))
            std::sscanf(field.constData() + 1, "%d:%d", &num, &den);
    }
    if (clip.width <= 0 || clip.height <= 0 || den <= 0)
        return std::nullopt;
    clip.fps = std::max(1, int(std::lround(double(num) / den)));
    const qint64 frame = 6 + qint64(clip.width) * clip.height + 2 * qint64((clip.width + 1) / 2) * ((clip.height + 1) / 2);
    clip.frames = int((file.size() - header.size()) / frame);
    return clip;
}

// VAE Encode Video: a clip into the latent a Sampler redraws from (video to video, #130). The
// helper reads a YUV4MPEG2 file of the size, length and rate the model will make, so ffmpeg fits
// the video to the node's inputs first: its pending trim and crop are applied, the rate is
// resampled, the picture is scaled to cover the size and cut from the middle (as the first frame
// is), and the length is the frames asked for, cut to Wan's 4 n + 1.
class VaeEncodeVideoKernel : public StageKernel {
public:
    VaeEncodeVideoKernel() : StageKernel(QStringLiteral("latent.encode")) {}

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        const Result<ModelFiles> files = modelInput(context, selection);
        if (!files)
            return files.errors();
        const Result<MediaHandle> source =
            context.input(QStringLiteral("video")).media().plainFile(context.workDir, QStringLiteral("source"), *context.hashes);
        if (!source)
            return source.errors();
        if (!QFileInfo(source->path()).isFile())
            return makeInputError(QStringLiteral("file_not_found"), QStringLiteral("video"),
                                  QStringLiteral("The video file is gone: %1.").arg(source->path()),
                                  QStringLiteral("Run the node that makes it again."));
        const auto step = [](int side) { return std::max(4, int(std::lround(double(side) / kSizeStep))) * kSizeStep; };
        const Value wide = context.input(QStringLiteral("width"));
        const Value high = context.input(QStringLiteral("height"));
        const int width = step(wide.isEmpty() ? source->width() : int(wide.number()));
        const int height = step(high.isEmpty() ? source->height() : int(high.number()));
        const int fps = int(context.input(QStringLiteral("fps")).number());
        const QString out = QDir(context.workDir).filePath(QStringLiteral("clip.y4m"));
        // The rounded length is not known until the video is cut: a video shorter than asked is cut
        // at its end, then again to 4 n + 1.
        int frames = int(context.input(QStringLiteral("frames")).number());
        for (int attempt = 0; attempt < 2; ++attempt) {
            const Result<int> made = cut(context, source->path(), width, height, fps, frames, out);
            if (!made)
                return made.errors();
            const int fit = (made.value() - 1) / 4 * 4 + 1;
            if (fit < 5)
                return makeInputError(QStringLiteral("node_failed"), QStringLiteral("video"),
                                      QStringLiteral("The video has %1 frames at %2 a second; the model needs at least 5.").arg(made.value()).arg(fps),
                                      QStringLiteral("Use a longer video or a higher rate."));
            if (made.value() == fit)
                break;
            frames = fit;
        }
        JobRequest request = job(*files, QJsonObject{{"width", width}, {"height", height}});
        request.inputs.insert(QStringLiteral("video"), QJsonObject{{"path", out}});
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &result) override {
        const std::optional<ClipFile> clip = clipFileOf(QDir(context.workDir).filePath(QStringLiteral("clip.y4m")));
        if (!clip)
            return makeError(QStringLiteral("node_failed"), QStringLiteral("The clip made for the helper cannot be read back."));
        LatentMeta meta;
        meta.width = clip->width;
        meta.height = clip->height;
        meta.frames = clip->frames;
        meta.fps = clip->fps;
        return latentOutput(result, meta, context.input(QStringLiteral("model")).json());
    }

private:
    // `source` as the helper's clip; returns the frames in it.
    static Result<int> cut(const NodeContext &context, const QString &source, int width, int height, int fps, int frames,
                           const QString &out) {
        // Full-range BT.601 4:2:0, the layout the helper writes and reads a clip in.
        const QString filter =
            QStringLiteral("fps=%1,scale=%2:%3:flags=lanczos:force_original_aspect_ratio=increase:out_color_matrix=bt601:out_range=full,"
                           "crop=%2:%3,format=yuvj420p")
                .arg(fps).arg(width).arg(height);
        const QStringList args{"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", source, "-map", "0:v:0", "-an",
                               "-vf", filter, "-frames:v", QString::number(frames), "-f", "yuv4mpegpipe", out};
        const Result<QByteArray> done = ffmpeg::runWatched(args, QStringLiteral("node_failed"), {}, context.cancelled);
        if (!done)
            return done.errors();
        const std::optional<ClipFile> clip = clipFileOf(out);
        if (!clip || clip->frames <= 0)
            return makeError(QStringLiteral("node_failed"), QStringLiteral("ffmpeg made no frames from the video."));
        return clip->frames;
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
    kernels.add(QStringLiteral("fugue/detail-regions"), std::make_shared<DetailRegionsKernel>());
    kernels.add(QStringLiteral("fugue/vae-encode-video"), std::make_shared<VaeEncodeVideoKernel>());
}

} // namespace fugue::nodes
