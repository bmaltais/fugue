// Text Generate, Enhance Prompt and Describe Image (spec 14.2): the text nodes, served by a provider's
// text.generate (8.1). The kernels only translate (node_kernel.h): inputs to a JobRequest,
// and the JobResult's answer, cleaned, to outputs with the values actually used (3.3).

#include "nodes/kernels.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>

#include <vips/vips8>

#include "engine/media.h"
#include "engine/prompt_templates.h"
#include "engine/vips.h"

namespace fugue::nodes {

namespace {

const QString kTextGenerate = QStringLiteral("text.generate");

// A picture goes to the model at most this many pixels on a side: vision models read far
// fewer (Qwen-VL resizes to about a megapixel), and the whole file travels as base64.
constexpr int kLongestSide = 1024;

// The answer without the model's reasoning ahead of it: a leading <think> block, or everything
// up to a first closing tag with no opening before it (the chat template wrote the opening,
// Qwen3). Tags later in an answer are part of it.
QString withoutReasoning(QString text) {
    static const QRegularExpression block(QStringLiteral(R"(^\s*<think>.*?</think>)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    text.remove(block);
    const qsizetype close = text.indexOf(QLatin1String("</think>"));
    if (close >= 0 && !text.left(close).contains(QLatin1String("<think>")))
        text = text.mid(close + 8);
    if (text.trimmed().startsWith(QLatin1String("<think>")))
        return {}; // cut off while still thinking
    return text.trimmed();
}

// Enhance Prompt's answer as a bare prompt: no leading label ("Enhanced prompt:", "Here is the
// prompt:"), no code fence, no quotes around the whole.
QString barePrompt(QString text) {
    static const QRegularExpression label(
        QStringLiteral(R"(^[*_#\s]*(?:(?:enhanced|improved|final|expanded|rewritten|new)\s+)?prompt\s*[*_]*\s*[:：]\s*[*_]*\s*|^here(?:'s|\x{2019}s| is)\s+(?:the|your|an?)\s+(?:\w+\s+)?prompt\b[^:\n]{0,30}:\s*)"),
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

// Which node a TextKernel serves. Enhance Prompt and Describe Image share everything but the
// templates they read (a family each) and where the words come from: the person's idea, or the
// picture with a fixed question.
enum class TextMode { Generate, Enhance, Describe };

// What Describe Image asks about the picture; the template (its system prompt) says how to answer.
const QString kDescribeQuestion = QStringLiteral("Describe this picture.");

QStringList familiesOf(TextMode mode) {
    return mode == TextMode::Describe ? QStringList{QStringLiteral("describe")} : promptFamilies();
}

class TextKernel : public ProviderKernel {
public:
    explicit TextKernel(TextMode mode) : m_mode(mode), m_enhance(mode != TextMode::Generate) {}

    // No properties: whether the model takes an image is checked in request(), because a model
    // of this machine does not say (omarchy provider) and must still be asked.
    Requirement requirement(const NodeType &, const Values &inputs) const override {
        Requirement requirement;
        requirement.capability = kTextGenerate;
        requirement.model = inputs.value(QStringLiteral("model")).json().toObject();
        return requirement;
    }

    // The template's instructions decide Enhance Prompt's answer and are read from files, so they are
    // part of the key: editing a template (or the person's file replacing one) is a new result.
    QString keyExtra(const Values &inputs) const override {
        if (!m_enhance)
            return {};
        const QString style = inputs.value(QStringLiteral("style")).text();
        const PromptTemplates templates = PromptTemplates::load();
        const PromptTemplate *chosen = templates.find(style);
        if (!chosen || !familiesOf(m_mode).contains(chosen->family))
            return {};
        return QString::fromLatin1(QCryptographicHash::hash(systemPrompt(*chosen, {}).toUtf8(), QCryptographicHash::Sha256).toHex());
    }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        JobRequest request;
        request.capability = kTextGenerate;
        request.model = selection.model.handle;
        QJsonObject &params = request.params;
        params.insert(QStringLiteral("prompt"),
                      m_mode == TextMode::Describe ? kDescribeQuestion : context.input(QStringLiteral("prompt")).text());
        QString system;
        if (m_enhance) {
            const Result<QString> made = enhanceSystem(context);
            if (!made)
                return made.errors();
            system = made.value();
        } else {
            system = context.input(QStringLiteral("system")).text();
        }
        if (!system.isEmpty())
            params.insert(QStringLiteral("system"), system);
        for (const QString &key : {QStringLiteral("seed"), QStringLiteral("max_tokens"), QStringLiteral("temperature")}) {
            const Value given = context.input(key);
            if (!given.isEmpty())
                params.insert(key, given.json());
        }
        // Enhance Prompt never thinks: Qwen3.5 2B on the DGX spent its whole default answer
        // (4096 tokens) reasoning about a fox and wrote nothing; without it, 41 tokens.
        params.insert(QStringLiteral("thinking"), !m_enhance && context.input(QStringLiteral("thinking")).json().toBool());

        const int longest = selection.model.limits.value(QStringLiteral("max_tokens")).toInt(0);
        if (longest > 0 && params.value(QStringLiteral("max_tokens")).toInt(0) > longest)
            return makeInputError(QStringLiteral("value_out_of_range"), QStringLiteral("max_tokens"),
                                  QStringLiteral("%1 answers with at most %2 tokens, not %3.")
                                      .arg(modelName(selection))
                                      .arg(longest)
                                      .arg(params.value(QStringLiteral("max_tokens")).toInt()),
                                  QStringLiteral("Ask for a shorter answer, or leave it unset."));

        if (m_mode != TextMode::Enhance && context.inputs.contains(QStringLiteral("image"))) {
            const QJsonValue accepted = selection.model.properties.value(QStringLiteral("input"));
            if (accepted.isArray() && !accepted.toArray().contains(QStringLiteral("image")))
                return makeInputError(QStringLiteral("capability_unavailable"), QStringLiteral("image"),
                                      QStringLiteral("%1 does not take images.").arg(modelName(selection)),
                                      QStringLiteral("Pick a model that reads images, or remove the image."));
            if (!initVips())
                return vipsUnavailable();
            Result<QJsonObject> image = QJsonObject();
            try {
                image = fitted(context.input(QStringLiteral("image")).media(), QDir(context.workDir).filePath("image.png"));
            } catch (const vips::VError &error) {
                Error unreadable = vipsError(error, QStringLiteral("media_unreadable"));
                unreadable.message = QStringLiteral("Cannot prepare the image: %1").arg(unreadable.message);
                return unreadable;
            }
            if (!image)
                return image.errors();
            request.inputs.insert(QStringLiteral("image"), image.value());
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
    // The instructions of the chosen template (data: engine/prompt_templates.h, src/prompts) and
    // the options. ComfyUI's enhancer nodes report a chatty answer ("Here is your prompt: ..."), so
    // every template asks for the prompt alone, and outputs() strips what slips through.
    Result<QString> enhanceSystem(const NodeContext &context) const {
        const QString style = context.input(QStringLiteral("style")).text();
        const PromptTemplates templates = PromptTemplates::load();
        const PromptTemplate *chosen = templates.find(style);
        if (!chosen || !familiesOf(m_mode).contains(chosen->family))
            return makeInputError(QStringLiteral("value_not_in_choices"), QStringLiteral("style"),
                                  QStringLiteral("There is no prompt style \"%1\".").arg(style),
                                  QStringLiteral("Choose among: %1.").arg(templates.ids(familiesOf(m_mode)).join(QStringLiteral(", "))));
        PromptOptions options;
        options.language = context.input(QStringLiteral("language")).text();
        options.durationSeconds = context.input(QStringLiteral("duration")).json().toInt();
        options.instructions = context.input(QStringLiteral("instructions")).text();
        return systemPrompt(*chosen, options);
    }

    // The picture to send, {path, format}: as it is when it is a PNG or JPEG standing upright
    // within the square, else shrunk to fit it (never enlarged) and written as an 8-bit RGB PNG
    // to `path`. The format is the one read from the bytes (MediaHandle), not the file's name.
    static Result<QJsonObject> fitted(const MediaHandle &image, const QString &path) {
        const bool small = image.width() <= kLongestSide && image.height() <= kLongestSide;
        if (small && image.hasFile() &&
            (image.format() == QLatin1String("png") || image.format() == QLatin1String("jpeg"))) {
            vips::VImage header = vips::VImage::new_from_file(QFile::encodeName(image.path()).constData());
            if (header.get_typeof("orientation") == 0 || header.get_int("orientation") == 1)
                return QJsonObject{{"path", image.path()}, {"format", image.format()}};
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
        return QJsonObject{{"path", path}, {"format", "png"}};
    }

    static QString modelName(const Selection &selection) { return selection.model.handle.value("name").toString(); }

    const TextMode m_mode;
    const bool m_enhance; // reads a prompt template (Enhance Prompt, Describe Image)
};

} // namespace

void addTextGenerationKernels(Kernels &kernels) {
    kernels.add(QStringLiteral("fugue/text-generate"), std::make_shared<TextKernel>(TextMode::Generate));
    kernels.add(QStringLiteral("fugue/enhance-prompt"), std::make_shared<TextKernel>(TextMode::Enhance));
    kernels.add(QStringLiteral("fugue/describe-image"), std::make_shared<TextKernel>(TextMode::Describe));
}

} // namespace fugue::nodes
