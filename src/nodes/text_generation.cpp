// Text Generate and Enhance Prompt (spec 14.2): the text nodes, served by a provider's
// text.generate (8.1). The kernels only translate (node_kernel.h): inputs to a JobRequest,
// and the JobResult's answer, cleaned, to outputs with the values actually used (3.3).

#include "nodes/kernels.h"

#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>

#include <vips/vips8>

#include "engine/media.h"
#include "engine/vips.h"

namespace fugue::nodes {

namespace {

const QString kTextGenerate = QStringLiteral("text.generate");

// A picture goes to the model at most this many pixels on a side: vision models read far
// fewer (Qwen-VL resizes to about a megapixel), and the whole file travels as base64.
constexpr int kLongestSide = 1024;

// Enhance Prompt's styles. What every style asks, then what each adds. ComfyUI's enhancer
// nodes report the same failure: a chatty answer ("Here is your prompt: ..."), so the rules
// ask for the prompt alone, and outputs() strips what slips through.
const QString kRules = QStringLiteral(
    "You turn a short idea for a picture into a prompt for a text-to-image model. Keep every subject, detail "
    "and constraint the idea gives, and do not change what it asks for; add only what makes the picture "
    "concrete. Do not put words, letters or logos in the picture unless the idea asks for them. ");
const QString kAnswer = QStringLiteral("Answer with the prompt alone: no title, no quotes, no notes.");

QString styleRules(const QString &style) {
    if (style == QLatin1String("photograph"))
        return QStringLiteral("Write one paragraph of plain sentences, 60 to 120 words, describing it as a photograph: "
                              "the subject first, then the setting, the framing and camera angle, the lens and depth "
                              "of field, the light and time of day, and the colour and film look. ");
    if (style == QLatin1String("tags"))
        return QStringLiteral("Write a comma-separated list of 20 to 40 short tags, the most important first: subject, "
                              "details, setting, composition, lighting, style. No sentences. ");
    return QStringLiteral("Write one paragraph of plain sentences, 60 to 120 words: the main subject first, then the "
                          "setting, the composition and point of view, the lighting, the colours, and the medium or "
                          "style. ");
}

// The answer without the model's reasoning: <think> blocks, or everything up to a closing
// tag whose opening the chat template put in the prompt (Qwen3).
QString withoutReasoning(QString text) {
    static const QRegularExpression block(QStringLiteral("<think>.*?</think>"),
                                          QRegularExpression::DotMatchesEverythingOption);
    text.remove(block);
    const qsizetype close = text.lastIndexOf(QLatin1String("</think>"));
    if (close >= 0)
        text = text.mid(close + 8);
    if (text.trimmed().startsWith(QLatin1String("<think>")))
        return {}; // cut off while still thinking
    return text.trimmed();
}

// Enhance Prompt's answer as a bare prompt: no leading label ("Enhanced prompt:", "Here is the
// prompt:"), no code fence, no quotes around the whole.
QString barePrompt(QString text) {
    static const QRegularExpression label(
        QStringLiteral(R"(^[*_#\s]*(?:(?:enhanced|improved|final|expanded|rewritten|new)\s+)?prompt\s*[*_]*\s*[:：]\s*[*_]*\s*|^here(?:'s| is)[^:\n]{0,60}:\s*)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression fence(QStringLiteral(R"(^```[a-z]*\s*([\s\S]*?)\s*```$)"));
    text = text.trimmed();
    text.remove(label);
    text = text.trimmed();
    if (const QRegularExpressionMatch fenced = fence.match(text); fenced.hasMatch())
        text = fenced.captured(1).trimmed();
    static const QList<std::pair<QChar, QChar>> quotes{{u'"', u'"'}, {u'\'', u'\''}, {u'“', u'”'},
                                                       {u'«', u'»'}, {u'`', u'`'}};
    for (const auto &[open, close] : quotes)
        if (text.size() >= 2 && text.front() == open && text.back() == close &&
            !text.mid(1, text.size() - 2).contains(close)) {
            text = text.mid(1, text.size() - 2).trimmed();
            break;
        }
    return text;
}

class TextKernel : public ProviderKernel {
public:
    explicit TextKernel(bool enhance) : m_enhance(enhance) {}

    // No properties: whether the model takes an image is checked in request(), because a model
    // of this machine does not say (omarchy provider) and must still be asked.
    Requirement requirement(const NodeType &, const Values &inputs) const override {
        Requirement requirement;
        requirement.capability = kTextGenerate;
        requirement.model = inputs.value(QStringLiteral("model")).json().toObject();
        return requirement;
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        JobRequest request;
        request.capability = kTextGenerate;
        request.model = selection.model.handle;
        QJsonObject &params = request.params;
        params.insert(QStringLiteral("prompt"), context.input(QStringLiteral("prompt")).text());
        const QString system = m_enhance ? enhanceSystem(context) : context.input(QStringLiteral("system")).text();
        if (!system.isEmpty())
            params.insert(QStringLiteral("system"), system);
        for (const QString &key : {QStringLiteral("seed"), QStringLiteral("max_tokens"), QStringLiteral("temperature")}) {
            const Value set = context.input(key);
            if (!set.isEmpty())
                params.insert(key, set.json());
        }
        // Enhance Prompt never thinks: Qwen3.5 2B on the DGX spent its whole default answer
        // (4096 tokens) reasoning about a fox and wrote nothing; without it, 41 tokens.
        params.insert(QStringLiteral("thinking"), !m_enhance && context.input(QStringLiteral("thinking")).json().toBool());

        const int most = selection.model.limits.value(QStringLiteral("max_tokens")).toInt(0);
        if (most > 0 && params.value(QStringLiteral("max_tokens")).toInt(0) > most)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("max_tokens"),
                                  QStringLiteral("%1 answers with at most %2 tokens, not %3.")
                                      .arg(modelName(selection))
                                      .arg(most)
                                      .arg(params.value(QStringLiteral("max_tokens")).toInt()),
                                  QStringLiteral("Ask for a shorter answer, or leave it unset."));

        if (!m_enhance && context.inputs.contains(QStringLiteral("image"))) {
            const QJsonValue takes = selection.model.properties.value(QStringLiteral("input"));
            if (takes.isArray() && !takes.toArray().contains(QStringLiteral("image")))
                return makeInputError(QStringLiteral("capability_unavailable"), QStringLiteral("image"),
                                      QStringLiteral("%1 does not take images.").arg(modelName(selection)),
                                      QStringLiteral("Pick a model that reads images, or remove the image."));
            if (!initVips())
                return vipsUnavailable();
            Result<QString> path = QString();
            try {
                path = fitted(context.input(QStringLiteral("image")).media(), QDir(context.workDir).filePath("image.png"));
            } catch (const vips::VError &error) {
                Error unreadable = vipsError(error, QStringLiteral("media_unreadable"));
                unreadable.message = QStringLiteral("Cannot prepare the image: %1").arg(unreadable.message);
                return unreadable;
            }
            if (!path)
                return path.errors();
            request.inputs.insert(QStringLiteral("image"), QJsonObject{{"path", path.value()}});
        }
        return request;
    }

    Result<Values> outputs(const NodeContext &context, const Selection &selection, const JobResult &result) override {
        QString text = withoutReasoning(result.outputs.value(QStringLiteral("text")).toString());
        if (m_enhance)
            text = barePrompt(text);
        if (text.isEmpty())
            return makeError(QStringLiteral("node_failed"), QStringLiteral("%1 gave an empty answer.").arg(modelName(selection)),
                             QStringLiteral("Run it again, or allow a longer answer: a thinking model can use it all up."));
        const QJsonValue seed = result.used.value(QStringLiteral("seed"));
        const QJsonObject model = result.used.value(QStringLiteral("model")).toObject();
        return Values{{QStringLiteral("text"), Value(text)},
                      {QStringLiteral("seed"), seed.isDouble() ? Value(seed) : context.input(QStringLiteral("seed"))},
                      {QStringLiteral("model"), Value(model.isEmpty() ? selection.model.handle : model)}};
    }

private:
    static QString enhanceSystem(const NodeContext &context) {
        QString system = kRules + styleRules(context.input(QStringLiteral("style")).text()) + kAnswer;
        const QString more = context.input(QStringLiteral("instructions")).text().trimmed();
        if (!more.isEmpty())
            system += QStringLiteral("\n\nAlso: ") + more;
        return system;
    }

    // The picture as it is when it is a PNG or JPEG standing upright within the square, else
    // shrunk to fit it (never enlarged) and written as an 8-bit RGB PNG to `path`.
    static Result<QString> fitted(const MediaHandle &image, const QString &path) {
        const bool small = image.width() <= kLongestSide && image.height() <= kLongestSide;
        if (small && image.hasFile() &&
            (image.format() == QLatin1String("png") || image.format() == QLatin1String("jpeg"))) {
            vips::VImage header = vips::VImage::new_from_file(QFile::encodeName(image.path()).constData());
            if (header.get_typeof("orientation") == 0 || header.get_int("orientation") == 1)
                return image.path();
        }
        const auto options = [] {
            return vips::VImage::option()->set("height", kLongestSide)->set("size", VIPS_SIZE_DOWN);
        };
        vips::VImage shrunk;
        if (image.hasFile()) {
            shrunk = vips::VImage::thumbnail(QFile::encodeName(image.path()).constData(), kLongestSide, options());
        } else {
            const Result<vips::VImage> decoded = image.image();
            if (!decoded)
                return decoded.errors();
            shrunk = decoded->thumbnail_image(kLongestSide, options());
        }
        shrunk = shrunk.colourspace(VIPS_INTERPRETATION_sRGB);
        if (shrunk.bands() > 3)
            shrunk = shrunk.extract_band(0, vips::VImage::option()->set("n", 3));
        shrunk.cast(VIPS_FORMAT_UCHAR).write_to_file(QFile::encodeName(path).constData());
        return path;
    }

    static QString modelName(const Selection &selection) { return selection.model.handle.value("name").toString(); }

    const bool m_enhance;
};

} // namespace

void addTextGenerationKernels(Kernels &kernels) {
    kernels.add(QStringLiteral("fugue/text-generate"), std::make_shared<TextKernel>(false));
    kernels.add(QStringLiteral("fugue/enhance-prompt"), std::make_shared<TextKernel>(true));
}

} // namespace fugue::nodes
