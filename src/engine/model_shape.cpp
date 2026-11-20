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
QHash<QString, ModelShape::Widths> loraLayers(const Shapes &shapes) {
    static const QRegularExpression down(QStringLiteral("^(.*)\\.(lora_A|lora_down)\\.weight$"));
    QStringList names;
    for (auto it = shapes.begin(); it != shapes.end(); ++it)
        if (down.match(it.key()).hasMatch() && !it.key().startsWith(QLatin1String("lora_te"))
            && !it.key().contains(QLatin1String("text_encoder")))
            names << it.key();
    QCollator collator; // blocks.2 before blocks.10, so the spread reaches the last block
    collator.setNumericMode(true);
    std::sort(names.begin(), names.end(), collator);
    QHash<QString, ModelShape::Widths> layers;
    const qsizetype count = names.size();
    const qsizetype kept = std::min<qsizetype>(count, kLoraLayersKept);
    for (qsizetype i = 0; i < kept; ++i) {
        const QString &name = names.at(kept > 1 ? i * (count - 1) / (kept - 1) : 0);
        const QString layer = down.match(name).captured(1);
        // The up piece (lora_B, lora_up) is [output, rank, ...].
        const QString upName = name.contains(QLatin1String(".lora_A.")) ? layer + QStringLiteral(".lora_B.weight")
                                                                         : layer + QStringLiteral(".lora_up.weight");
        const QList<qint64> up = shapes.value(upName);
        const std::optional<int> in = inputWidth(shapes.value(name));
        if (in && !up.isEmpty() && up.at(0) > 0)
            layers.insert(layerKey(layer), ModelShape::Widths{*in, int(up.at(0))});
    }
    return layers;
}

ModelShape shapeOf(const Header &header) {
    const Shapes &shapes = header.shapes;
    ModelShape shape;
    if (isLora(shapes)) { // its tensors are named after the model's: none of the figures below
        shape.kind = ModelKind::Lora;
        shape.layers = loraLayers(shapes);
        return shape;
    }
    shape.textWidth = inputWidth(
        tensor(shapes, {QStringLiteral("cap_embedder.1.weight"), QStringLiteral("txt_in.weight"),
                        QStringLiteral("context_embedder.weight")}));
    const std::optional<QList<qint64>> image =
        tensor(shapes, {QStringLiteral("x_embedder.weight"), QStringLiteral("all_x_embedder.2-1.weight"), QStringLiteral("img_in.weight"),
                        QStringLiteral("x_embedder.proj.weight"), QStringLiteral("input_blocks.0.0.weight")});
    if (const std::optional<int> in = inputWidth(image)) // a linear layer takes 2x2 patches of the channels
        shape.latentChannels = image->size() == 2 ? (*in % 4 == 0 ? std::optional<int>(*in / 4) : std::nullopt) : in;
    const bool diffusion = shape.textWidth || shape.latentChannels;
    if (diffusion) {
        // FLUX.1 (vector_in) and SD3 (context_embedder with joint_blocks); a diffusers FLUX.2 has a
        // context_embedder too, reading an LLM.
        const bool t5 = hasTensor(shapes, QStringLiteral("vector_in."))
                        || (hasTensor(shapes, QStringLiteral("context_embedder.")) && hasTensor(shapes, QStringLiteral("joint_blocks.")));
        shape.textKind = t5 ? TextKind::T5 : TextKind::Llm;
        shape.videoVae = hasTensor(shapes, QStringLiteral("time_text_embed.")) && hasTensor(shapes, QStringLiteral("txt_in."));
        // A checkpoint carries its VAE (first_stage_model.decoder) beside the diffusion model.
        shape.kind = tensor(shapes, {QStringLiteral("decoder.conv_in.weight")}) ? ModelKind::Checkpoint : ModelKind::DiffusionModel;
        for (auto it = shapes.begin(); it != shapes.end(); ++it) // what a LoRA may change
            if (it.key().endsWith(QLatin1String(".weight")) && it.value().size() >= 2 && it.value().at(0) > 0)
                if (const std::optional<int> in = inputWidth(it.value()))
                    shape.layers.insert(layerKey(it.key().chopped(7)), ModelShape::Widths{*in, int(it.value().at(0))});
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
    if (textEncoder.textKind && *textEncoder.textKind != TextKind::Llm)
        return Fit::DoesNotFit;
    return textEncoderFits(diffusionModel, textEncoder);
}

Fit vaeFits(const ModelShape &diffusionModel, const ModelShape &vae) {
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
        if (found == diffusionModel.layers.constEnd())
            continue;
        if (!(*found == it.value())) {
            if (mismatch)
                *mismatch = LayerMismatch{it.key(), it.value(), *found};
            return Fit::DoesNotFit;
        }
        ++matched;
    }
    return matched > 0 ? Fit::Fits : Fit::Unknown;
}

ModelShapes &ModelShapes::shared() {
    static ModelShapes shapes;
    return shapes;
}

std::optional<ModelShape> ModelShapes::of(const QString &path) {
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
