#include "model_shape.h"

#include <QCollator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace fugue {

namespace {

// Tensor shapes by name, outermost dimension first (safetensors' order; GGUF stores them the
// other way round and is turned).
using Shapes = QHash<QString, QList<qint64>>;

struct Header {
    Shapes shapes;
    QByteArray architecture;            // GGUF general.architecture
    std::optional<int> embeddingLength; // GGUF <architecture>.embedding_length
};

// The longest header read: a GGUF file's tokenizer metadata is a few MB, a safetensors header
// well under one. Anything beyond is not a model header.
constexpr qint64 kLongestHeader = 64 * 1024 * 1024;
// The most metadata fields, tensors or list items a header may count (a tokenizer has ~250k).
constexpr quint64 kMostEntries = 10 * 1000 * 1000;

// Reads a GGUF header field by field, failing once for good on a short or absurd read.
class GgufReader {
public:
    explicit GgufReader(QFile &file) : m_file(file) {}
    bool ok() const { return m_ok && m_file.pos() <= kLongestHeader; }

    template <typename T> T number() {
        T value{};
        if (m_file.read(reinterpret_cast<char *>(&value), sizeof value) != sizeof value)
            m_ok = false;
        return qFromLittleEndian(value); // GGUF is little-endian
    }
    QByteArray string() {
        const quint64 size = number<quint64>();
        if (!ok() || size > quint64(kLongestHeader)) {
            m_ok = false;
            return {};
        }
        QByteArray bytes = m_file.read(qint64(size));
        if (bytes.size() != qint64(size))
            m_ok = false;
        return bytes;
    }
    // A metadata value of GGUF type `type`: an integer when it is one, else skipped.
    std::optional<qint64> value(quint32 type) {
        switch (type) {
        case 0: return number<quint8>();
        case 1: return number<qint8>();
        case 2: return number<quint16>();
        case 3: return number<qint16>();
        case 4: return number<quint32>();
        case 5: return number<qint32>();
        case 10: return qint64(number<quint64>());
        case 11: return number<qint64>();
        case 6: number<float>(); return std::nullopt;
        case 7: number<quint8>(); return std::nullopt;
        case 12: number<double>(); return std::nullopt;
        case 8: string(); return std::nullopt;
        case 9: {
            const quint32 itemType = number<quint32>();
            const quint64 count = number<quint64>();
            // A list of lists is no model header, and reading one could recurse without end.
            if (!ok() || count > kMostEntries || itemType == 9) {
                m_ok = false;
                return std::nullopt;
            }
            for (quint64 i = 0; i < count && ok(); ++i)
                value(itemType);
            return std::nullopt;
        }
        default:
            m_ok = false;
            return std::nullopt;
        }
    }

private:
    QFile &m_file;
    bool m_ok = true;
};

std::optional<Header> readGguf(QFile &file) {
    GgufReader in(file);
    const quint32 version = in.number<quint32>();
    if (version < 2) // version 1 counted in 32 bits; nothing current writes it
        return std::nullopt;
    const quint64 tensors = in.number<quint64>();
    const quint64 fields = in.number<quint64>();
    if (!in.ok() || tensors > kMostEntries || fields > kMostEntries)
        return std::nullopt;
    Header header;
    QByteArray architecture;
    QHash<QByteArray, qint64> integers;
    for (quint64 i = 0; i < fields && in.ok(); ++i) {
        const QByteArray key = in.string();
        const quint32 type = in.number<quint32>();
        if (key == "general.architecture" && type == 8) {
            architecture = in.string();
            continue;
        }
        if (const std::optional<qint64> number = in.value(type); number && key.endsWith(".embedding_length"))
            integers.insert(key, *number);
    }
    for (quint64 i = 0; i < tensors && in.ok(); ++i) {
        const QString name = QString::fromUtf8(in.string());
        const quint32 rank = in.number<quint32>();
        if (rank > 8)
            return std::nullopt;
        QList<qint64> shape;
        for (quint32 d = 0; d < rank; ++d)
            shape.prepend(qint64(in.number<quint64>()));
        in.number<quint32>(); // the element type
        in.number<quint64>(); // the data offset
        header.shapes.insert(name, shape);
    }
    if (!in.ok())
        return std::nullopt;
    header.architecture = architecture;
    if (const auto found = integers.constFind(architecture + ".embedding_length"); found != integers.constEnd())
        header.embeddingLength = int(*found);
    return header;
}

std::optional<Header> readSafetensors(QFile &file) {
    quint64 size = 0;
    if (file.read(reinterpret_cast<char *>(&size), sizeof size) != sizeof size)
        return std::nullopt;
    size = qFromLittleEndian(size);
    if (size > quint64(kLongestHeader))
        return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(file.read(qint64(size)));
    if (!document.isObject())
        return std::nullopt;
    Header header;
    const QJsonObject tensors = document.object();
    for (auto it = tensors.begin(); it != tensors.end(); ++it) {
        QList<qint64> shape;
        for (const QJsonValue &d : it.value().toObject().value(QStringLiteral("shape")).toArray())
            shape.append(d.toInteger());
        header.shapes.insert(it.key(), shape);
    }
    return header;
}

// The shape of a tensor whose name ends with one of `suffixes`, tried in order; of two with
// the same suffix, the one first by name, so the answer never depends on the hash's order.
std::optional<QList<qint64>> tensor(const Shapes &shapes, const QStringList &suffixes) {
    QStringList names = shapes.keys();
    names.sort();
    for (const QString &suffix : suffixes)
        for (const QString &name : std::as_const(names))
            if (name == suffix || name.endsWith(QLatin1Char('.') + suffix))
                return shapes.value(name);
    return std::nullopt;
}

bool hasTensor(const Shapes &shapes, const QString &part) {
    for (auto it = shapes.begin(); it != shapes.end(); ++it)
        if (it.key().startsWith(part) || it.key().contains(QLatin1Char('.') + part))
            return true;
    return false;
}

// The input width of a linear layer or the input channels of a convolution.
std::optional<int> inputWidth(const std::optional<QList<qint64>> &shape) {
    if (!shape || shape->size() < 2 || shape->at(1) <= 0)
        return std::nullopt;
    return int(shape->at(1));
}

// A LoRA or LyCORIS file: low-rank pieces named for the layers they change (kohya's lora_up and
// lora_down, PEFT's lora_A and lora_B, LoKr, LoHa).
bool isLora(const Shapes &shapes) {
    static const QStringList marks{QStringLiteral("lora_up."),  QStringLiteral("lora_down."), QStringLiteral("lora_A."),
                                   QStringLiteral("lora_B."),   QStringLiteral("lokr_w1"),    QStringLiteral("hada_w1")};
    for (auto it = shapes.begin(); it != shapes.end(); ++it)
        for (const QString &mark : marks)
            if (it.key().contains(mark))
                return true;
    return false;
}

// The layers a LoRA changes and their input widths: the low-rank down piece (lora_A,
// lora_down) is [rank, input, ...]. Text-encoder layers are left out. Kept: kLoraLayersKept,
// spread over the names in order, so the first and last blocks are among them.
// Every layer a LoRA changes, with its widths, by tensor name (text-encoder layers left out):
// PEFT's lora_A [rank, input] and lora_B [output, rank] (".default" between, as PEFT saves),
// kohya's lora_down and lora_up the same, LoHa's hada_w1_a [output, rank] and hada_w1_b [rank,
// input], and LoKr's lokr_w1 and lokr_w2, whose Kronecker product is the layer (4x4 and
// 1024x1024 make 4096x4096).
QList<std::pair<QString, ModelShape::Widths>> changedLayers(const Shapes &shapes) {
    static const QRegularExpression piece(
        QStringLiteral("^(.*)\\.(lora_A|lora_down)(\\.default)?\\.weight$|^(.*)\\.lokr_w1$|^(.*)\\.hada_w1_a$"));
    QList<std::pair<QString, ModelShape::Widths>> layers;
    for (auto it = shapes.begin(); it != shapes.end(); ++it) {
        const QString &name = it.key();
        if (name.startsWith(QLatin1String("lora_te")) || name.contains(QLatin1String("text_encoder")))
            continue;
        const QRegularExpressionMatch match = piece.match(name);
        if (!match.hasMatch())
            continue;
        if (!match.captured(5).isEmpty()) { // LoHa: w1_a [output, rank], w1_b [rank, input]
            const QList<qint64> a = it.value(), b = shapes.value(match.captured(5) + QStringLiteral(".hada_w1_b"));
            if (!a.isEmpty() && a.at(0) > 0)
                if (const std::optional<int> in = inputWidth(b))
                    layers.append({match.captured(5), ModelShape::Widths{*in, int(a.at(0))}});
            continue;
        }
        if (!match.captured(4).isEmpty()) { // LoKr
            const QList<qint64> w1 = it.value(), w2 = shapes.value(match.captured(4) + QStringLiteral(".lokr_w2"));
            if (w1.size() == 2 && w2.size() == 2 && w1.at(0) > 0 && w1.at(1) > 0 && w2.at(0) > 0 && w2.at(1) > 0)
                layers.append({match.captured(4), ModelShape::Widths{int(w1.at(1) * w2.at(1)), int(w1.at(0) * w2.at(0))}});
            continue;
        }
        const QString layer = match.captured(1);
        const QString up = layer + (match.captured(2) == QLatin1String("lora_A") ? QStringLiteral(".lora_B") : QStringLiteral(".lora_up"))
                           + match.captured(3) + QStringLiteral(".weight");
        const QList<qint64> upShape = shapes.value(up);
        if (const std::optional<int> in = inputWidth(it.value()); in && !upShape.isEmpty() && upShape.at(0) > 0)
            layers.append({layer, ModelShape::Widths{*in, int(upShape.at(0))}});
    }
    return layers;
}

// The layers a LoRA changes, as a shape keeps them: kLoraLayersKept, spread over the names in
// number order (blocks.2 before blocks.10), so the first and last blocks are among them.
QHash<QString, ModelShape::Widths> loraLayers(QList<std::pair<QString, ModelShape::Widths>> changed) {
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(changed.begin(), changed.end(), [&](const auto &a, const auto &b) { return collator.compare(a.first, b.first) < 0; });
    QHash<QString, ModelShape::Widths> layers;
    const qsizetype count = changed.size();
    const qsizetype kept = std::min<qsizetype>(count, kLoraLayersKept);
    for (qsizetype i = 0; i < kept; ++i) {
        const auto &[layer, widths] = changed.at(kept > 1 ? i * (count - 1) / (kept - 1) : 0);
        layers.insert(layerKey(layer), widths);
    }
    return layers;
}

// Why stable-diffusion.cpp at the pin cannot load these tensors, or empty (ModelShape::unloadable).
QString unloadableFormat(const Shapes &shapes) {
    bool comfyQuant = false, qweight = false, projDown = false, scaledZeros = false;
    for (auto it = shapes.begin(); it != shapes.end(); ++it) {
        const QString &name = it.key();
        if (name.endsWith(QLatin1String(".weight_codebook"))) // GPTQ files carry comfy_quant too
            return QStringLiteral("GPTQ");
        comfyQuant = comfyQuant || name.endsWith(QLatin1String(".comfy_quant"));
        qweight = qweight || name.endsWith(QLatin1String(".qweight"));
        projDown = projDown || name.endsWith(QLatin1String(".proj_down"));
        scaledZeros = scaledZeros || name.endsWith(QLatin1String(".scaled_zeros"));
    }
    if (comfyQuant)
        return QStringLiteral("ComfyUI mixed quantization");
    if (qweight && projDown)
        return QStringLiteral("SVDQuant (Nunchaku)");
    if (qweight && scaledZeros)
        return QStringLiteral("AWQ");
    return {};
}

// A UNet's layer, as SD 1.5 and SDXL name them (LDM or diffusers, kohya's underscores too).
bool isUnetLayer(const QString &name) {
    for (const char *part : {"input_blocks", "output_blocks", "down_blocks", "up_blocks", "mid_block", "middle_block"})
        if (name.contains(QLatin1String(part)))
            return true;
    return false;
}

ModelShape shapeOf(const Header &header) {
    const Shapes &shapes = header.shapes;
    ModelShape shape;
    shape.unloadable = unloadableFormat(shapes);
    if (isLora(shapes)) { // its tensors are named after the model's: none of the figures below
        shape.kind = ModelKind::Lora;
        const QList<std::pair<QString, ModelShape::Widths>> changed = changedLayers(shapes);
        shape.layers = loraLayers(changed);
        QHash<int, int> widths; // how many layers take each input width
        for (const auto &[layer, width] : changed)
            ++widths[width.in];
        // A UNet's or a transformer's, by every tensor's name, the text encoder's left out.
        bool unet = false, any = false;
        for (auto it = shapes.begin(); it != shapes.end(); ++it) {
            const QString &name = it.key();
            shape.nativeLoraNames = shape.nativeLoraNames || name.startsWith(QLatin1String("diffusion_model."));
            if (name.startsWith(QLatin1String("lora_te")) || name.contains(QLatin1String("text_encoder")))
                continue;
            any = true;
            unet = unet || isUnetLayer(name);
        }
        if (any)
            shape.unet = unet;
        int most = 0;
        for (auto it = widths.begin(); it != widths.end(); ++it)
            if (it.value() > most) {
                most = it.value();
                shape.hidden = it.key();
            }
        return shape;
    }
    shape.textWidth = inputWidth(
        tensor(shapes, {QStringLiteral("cap_embedder.1.weight"), QStringLiteral("txt_in.weight"),
                        QStringLiteral("context_embedder.weight"), QStringLiteral("text_embedding.0.weight")}));
    const std::optional<QList<qint64>> image =
        tensor(shapes, {QStringLiteral("x_embedder.weight"), QStringLiteral("all_x_embedder.2-1.weight"), QStringLiteral("img_in.weight"),
                        QStringLiteral("x_embedder.proj.weight"), QStringLiteral("input_blocks.0.0.weight"),
                        QStringLiteral("patch_embedding.weight")});
    if (image && image->size() == 2 && image->at(0) > 0) // a transformer's: its block width
        shape.hidden = int(image->at(0));
    if (const std::optional<int> in = inputWidth(image)) // a linear layer takes 2x2 patches of the channels
        shape.latentChannels = image->size() == 2 ? (*in % 4 == 0 ? std::optional<int>(*in / 4) : std::nullopt) : in;
    const bool diffusion = shape.textWidth || shape.latentChannels;
    if (diffusion) {
        // Wan (text_embedding and a 3D patch_embedding) reads UMT5, a T5, and takes a video VAE.
        const bool wan = hasTensor(shapes, QStringLiteral("text_embedding.0.")) && hasTensor(shapes, QStringLiteral("patch_embedding."));
        // An image-to-video model takes its start frame in the input: 16 noise channels, 4 for the
        // mask and 16 for the frame's latent (36), though it makes 16, as Wan's VAE has.
        if (wan && shape.latentChannels == 36)
            shape.latentChannels = 16;
        // FLUX.1 (vector_in) and SD3 (context_embedder with joint_blocks); a diffusers FLUX.2 has a
        // context_embedder too, reading an LLM.
        const bool t5 = wan || hasTensor(shapes, QStringLiteral("vector_in."))
                        || (hasTensor(shapes, QStringLiteral("context_embedder.")) && hasTensor(shapes, QStringLiteral("joint_blocks.")));
        shape.textKind = t5 ? TextKind::T5 : TextKind::Llm;
        shape.videoVae = wan || (hasTensor(shapes, QStringLiteral("time_text_embed.")) && hasTensor(shapes, QStringLiteral("txt_in.")));
        // The LLM it reads: Qwen-Image Qwen2.5-VL; Z-Image (cap_embedder) and FLUX.2 (txt_in with
        // no vector_in) Qwen3.
        if (*shape.videoVae && !wan)
            shape.llmFamily = QStringLiteral("qwen2.5");
        else if (!t5 && (hasTensor(shapes, QStringLiteral("cap_embedder.")) || hasTensor(shapes, QStringLiteral("txt_in."))))
            shape.llmFamily = QStringLiteral("qwen3");
        shape.unet = hasTensor(shapes, QStringLiteral("input_blocks.")) || hasTensor(shapes, QStringLiteral("down_blocks."));
        // A checkpoint carries its VAE (first_stage_model.decoder) beside the diffusion model.
        shape.kind = tensor(shapes, {QStringLiteral("decoder.conv_in.weight")}) ? ModelKind::Checkpoint : ModelKind::DiffusionModel;
        for (auto it = shapes.begin(); it != shapes.end(); ++it) // what a LoRA may change
            if (it.key().endsWith(QLatin1String(".weight")) && it.value().size() >= 2 && it.value().at(0) > 0)
                if (const std::optional<int> in = inputWidth(it.value())) {
                    const QString key = layerKey(it.key().chopped(7));
                    shape.layers.insert(key, ModelShape::Widths{*in, int(it.value().at(0))});
                    shape.layerFamilies.insert(layerFamily(key));
                }
    } else if (const std::optional<QList<qint64>> decoder =
                   tensor(shapes, {QStringLiteral("decoder.conv_in.weight"), QStringLiteral("decoder.conv1.weight")})) {
        shape.latentChannels = inputWidth(decoder);
        shape.videoVae = decoder->size() == 5;
        shape.kind = ModelKind::Vae;
    }
    shape.hiddenSize = header.embeddingLength;
    if (!shape.hiddenSize)
        shape.hiddenSize = inputWidth(tensor(shapes, {QStringLiteral("embed_tokens.weight"), QStringLiteral("token_embd.weight"),
                                                   QStringLiteral("token_embedding.weight"), QStringLiteral("shared.weight")}));
    if (shape.hiddenSize && !diffusion) {
        if (!shape.kind)
            shape.kind = ModelKind::TextEncoder;
        const QByteArray &arch = header.architecture;
        if (arch.startsWith("t5") || hasTensor(shapes, QStringLiteral("encoder.block.")))
            shape.textKind = TextKind::T5;
        else if (arch == "clip" || hasTensor(shapes, QStringLiteral("text_model.")))
            shape.textKind = TextKind::Clip;
        else
            shape.textKind = TextKind::Llm;
        if (shape.textKind == TextKind::Llm) {
            // GGUF says its architecture; safetensors' layers tell (8.10's pairings, helper.md).
            if (arch == "qwen3" || (arch.isEmpty() && hasTensor(shapes, QStringLiteral("self_attn.q_norm.")) &&
                                    !hasTensor(shapes, QStringLiteral("linear_attn."))))
                shape.llmFamily = QStringLiteral("qwen3");
            else if (arch.startsWith("qwen35") || (arch.isEmpty() && hasTensor(shapes, QStringLiteral("linear_attn."))))
                shape.llmFamily = QStringLiteral("qwen3.5");
            else if (arch.startsWith("qwen2") || (arch.isEmpty() && hasTensor(shapes, QStringLiteral("model.layers.")) &&
                                                  hasTensor(shapes, QStringLiteral("visual."))))
                shape.llmFamily = QStringLiteral("qwen2.5");
        }
    }
    return shape;
}

} // namespace

QString layerKey(const QString &tensorName) {
    QString name = tensorName;
    for (const char *prefix : {"model.diffusion_model.", "diffusion_model.", "base_model.model.", "transformer.", "lora_unet_"})
        if (name.startsWith(QLatin1String(prefix)))
            name.remove(0, qsizetype(std::strlen(prefix)));
    static const QRegularExpression separators(QStringLiteral("[._]+"));
    return name.replace(separators, QStringLiteral("."));
}

QString layerFamily(const QString &key) {
    static const QRegularExpression number(QStringLiteral("\\d+"));
    return QString(key).replace(number, QStringLiteral("#"));
}

QString modelKindName(ModelKind kind) {
    switch (kind) {
    case ModelKind::DiffusionModel: return QStringLiteral("diffusion-model");
    case ModelKind::TextEncoder: return QStringLiteral("text-encoder");
    case ModelKind::Vae: return QStringLiteral("vae");
    case ModelKind::Lora: return QStringLiteral("lora");
    case ModelKind::Checkpoint: return QStringLiteral("checkpoint");
    }
    return {};
}

std::optional<ModelShape> readModelShape(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    // GGUF says what it is in its first bytes; safetensors does not, so its name says.
    std::optional<Header> header;
    if (file.read(4) == "GGUF")
        header = readGguf(file);
    else if (path.endsWith(QLatin1String(".safetensors"), Qt::CaseInsensitive) && file.seek(0))
        header = readSafetensors(file);
    if (!header)
        return std::nullopt;
    return shapeOf(*header);
}

Fit textEncoderFits(const ModelShape &diffusionModel, const ModelShape &textEncoder) {
    if (diffusionModel.textKind && textEncoder.textKind && *diffusionModel.textKind != *textEncoder.textKind)
        return Fit::DoesNotFit;
    if (!diffusionModel.textWidth || !textEncoder.hiddenSize || *textEncoder.hiddenSize <= 0)
        return Fit::Unknown;
    const int width = *diffusionModel.textWidth;
    const int hidden = *textEncoder.hiddenSize;
    return width % hidden == 0 && width / hidden >= 1 && width / hidden <= 3 ? Fit::Fits : Fit::DoesNotFit;
}

Fit loadableTextEncoderFits(const ModelShape &diffusionModel, const ModelShape &textEncoder) {
    if (!textEncoder.unloadable.isEmpty())
        return Fit::DoesNotFit;
    // Wan's UMT5 (t5xxl_path) is the one T5 Load Models takes: a video model that reads T5.
    const bool wan = diffusionModel.textKind == TextKind::T5 && diffusionModel.videoVae.value_or(false);
    if (textEncoder.textKind && *textEncoder.textKind != TextKind::Llm && !(wan && *textEncoder.textKind == TextKind::T5))
        return Fit::DoesNotFit;
    if (!diffusionModel.llmFamily.isEmpty() && !textEncoder.llmFamily.isEmpty()
        && diffusionModel.llmFamily != textEncoder.llmFamily)
        return Fit::DoesNotFit;
    return textEncoderFits(diffusionModel, textEncoder);
}

Fit vaeFits(const ModelShape &diffusionModel, const ModelShape &vae) {
    if (!vae.unloadable.isEmpty())
        return Fit::DoesNotFit;
    if (diffusionModel.videoVae && vae.videoVae && *diffusionModel.videoVae != *vae.videoVae)
        return Fit::DoesNotFit;
    if (!diffusionModel.latentChannels || !vae.latentChannels)
        return Fit::Unknown;
    return *diffusionModel.latentChannels == *vae.latentChannels ? Fit::Fits : Fit::DoesNotFit;
}

bool ModelShapes::known(const QString &path, std::optional<ModelShape> *shape) {
    const QFileInfo info(path);
    QMutexLocker lock(&m_mutex);
    const auto found = m_entries.constFind(path);
    if (found == m_entries.constEnd() || found->size != info.size()
        || found->modified != info.lastModified().toMSecsSinceEpoch())
        return false;
    *shape = found->shape;
    return true;
}

Fit loraFits(const ModelShape &diffusionModel, const ModelShape &lora, LayerMismatch *mismatch) {
    int matched = 0;
    for (auto it = lora.layers.begin(); it != lora.layers.end(); ++it) {
        const auto found = diffusionModel.layers.constFind(it.key());
        if (found == diffusionModel.layers.constEnd()) {
            // A block of a family the model has, at an index it lacks: made for a deeper model.
            if (diffusionModel.layerFamilies.contains(layerFamily(it.key()))) {
                if (mismatch)
                    *mismatch = LayerMismatch{it.key(), it.value(), {}};
                return Fit::DoesNotFit;
            }
            continue;
        }
        if (!(*found == it.value())) {
            if (mismatch)
                *mismatch = LayerMismatch{it.key(), it.value(), *found};
            return Fit::DoesNotFit;
        }
        ++matched;
    }
    if (matched > 0)
        return Fit::Fits;
    // Named as ComfyUI saves one and none of it found: made for another model.
    if (lora.nativeLoraNames && !diffusionModel.layers.isEmpty() && !lora.layers.isEmpty())
        return Fit::DoesNotFit;
    // Other namings (kohya's, diffusers') stable-diffusion.cpp renames itself; their structure
    // still tells: a UNet's LoRA on a transformer (or the other way round), or another width.
    if (lora.unet && diffusionModel.unet && *lora.unet != *diffusionModel.unet)
        return Fit::DoesNotFit;
    if (lora.hidden && diffusionModel.hidden && !diffusionModel.unet.value_or(true) && *lora.hidden != *diffusionModel.hidden)
        return Fit::DoesNotFit;
    return Fit::Unknown;
}

ModelShapes &ModelShapes::shared() {
    static ModelShapes shapes;
    return shapes;
}

std::optional<ModelShape> ModelShapes::of(const QString &path) {
    if (path.isEmpty()) // a checkpoint has no separate diffusion model: nothing to read
        return std::nullopt;
    const QFileInfo info(path);
    const qint64 size = info.size();
    const qint64 modified = info.lastModified().toMSecsSinceEpoch();
    {
        QMutexLocker lock(&m_mutex);
        const auto found = m_entries.constFind(path);
        if (found != m_entries.constEnd() && found->size == size && found->modified == modified)
            return found->shape;
    }
    const std::optional<ModelShape> shape = readModelShape(path); // outside the lock: it reads a file
    QMutexLocker lock(&m_mutex);
    m_entries.insert(path, Entry{size, modified, shape});
    return shape;
}

} // namespace fugue
