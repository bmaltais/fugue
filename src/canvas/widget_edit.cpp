#include "canvas/widget_edit.h"
#include "workflow/image_inputs.h"

#include "canvas/canvas_model.h"
#include "canvas/input_links.h"
#include "engine/file_hashes.h"
#include "engine/model_library.h"
#include "engine/registry.h"
#include "engine/settings.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace fugue;

namespace {

constexpr double maxSeed = 9007199254740991.0; // 2^53 - 1 (2.1)

QString label(const InputSpec &spec) { return spec.label.isEmpty() ? spec.name : spec.label; }

double stepOf(const InputSpec &spec) {
    if (spec.step && *spec.step > 0)
        return *spec.step;
    return spec.integer ? 1 : 0.1;
}

// Rounds to the step's decimals, so 0.1 + 0.2 is 0.3.
double rounded(double value, const InputSpec &spec) {
    if (spec.integer)
        return std::round(value);
    const QString text = QString::number(stepOf(spec), 'g', 15);
    const int point = text.indexOf('.');
    const int decimals = point < 0 || text.contains('e') ? 0 : int(text.size() - point - 1);
    const double scale = std::pow(10.0, std::min(decimals, 12));
    return std::round(value * scale) / scale;
}

double clamped(double value, const InputSpec &spec) {
    if (spec.min)
        value = std::max(value, *spec.min);
    if (spec.max)
        value = std::min(value, *spec.max);
    return value;
}

// The increment list of a node's layout entry (6.2's increment seeds).
QStringList incrementing(const Document &document, const QString &node) {
    QStringList names;
    const QJsonArray list =
        document.layout.value("nodes").toObject().value(node).toObject().value("increment").toArray();
    for (const QJsonValue &each : list)
        names << each.toString();
    return names;
}

void setIncrementing(Document &document, const QString &node, const QString &input, bool on) {
    QJsonObject layout = document.layout;
    QJsonObject nodes = layout.value("nodes").toObject();
    QJsonObject entry = nodes.value(node).toObject();
    QStringList names = incrementing(document, node);
    names.removeAll(input);
    if (on)
        names << input;
    names.sort();
    if (names.isEmpty())
        entry.remove("increment");
    else
        entry.insert("increment", QJsonArray::fromStringList(names));
    nodes.insert(node, entry);
    layout.insert("nodes", nodes);
    document.layout = layout;
}

} // namespace

WidgetEdit::WidgetEdit(const Registry *registry, CanvasModel *canvas, QObject *parent)
    : QObject(parent), m_registry(registry), m_canvas(canvas) {
    // One file at a time: two models read at once from one disk each take twice as long.
    m_hashPool.setMaxThreadCount(1);
    connect(canvas, &CanvasModel::documentReplaced, this, [this] {
        if (m_picks.isEmpty())
            return;
        m_picks.clear();
        ++m_hashRevision;
        emit hashingChanged();
    });
    connect(canvas, &CanvasModel::scopeChanged, this, [this] {
        storePicks();
        ++m_hashRevision;
        emit hashingChanged();
    });
    connect(canvas, &CanvasModel::documentReset, this, [this] {
        if (!editing())
            return;
        m_session = {};
        emit sessionEnded();
    });
}

WidgetEdit::Target WidgetEdit::target(const QString &node, const QString &input) const {
    const InputSpec *spec = m_canvas->inputSpec(node, input);
    if (!spec || !m_registry || spec->type.list)
        return {};
    Target target;
    target.kind = m_registry->portKind(spec->type.type);
    target.widget = target.kind == PortKind::Resource ? QStringLiteral("model") : spec->widget();
    if (target.widget.isEmpty())
        return {};
    target.spec = spec;
    return target;
}

QVariantMap WidgetEdit::spec(const QString &node, const QString &input) const {
    const Target t = target(node, input);
    if (!t.spec)
        return {};
    const InputSpec &s = *t.spec;
    QVariantMap map{{"widget", t.widget},
                    {"type", s.type.type.toString()},
                    {"label", label(s)},
                    {"integer", s.integer},
                    {"optional", s.optional},
                    {"step", stepOf(s)}};
    if (s.min)
        map.insert("min", *s.min);
    if (s.max)
        map.insert("max", *s.max);
    if (s.choices)
        map.insert("choices", *s.choices);
    if (!s.defaultValue.isUndefined())
        map.insert("default", s.defaultValue.toVariant());
    return map;
}

QVariant WidgetEdit::value(const QString &node, const QString &input) const {
    const InputValue value = m_canvas->m_document.graph.nodes.value(node).inputs.value(input);
    if (value.isEmpty() || value.kind() == InputValue::Kind::Link)
        return {};
    return value.toJson().toVariant();
}

QString WidgetEdit::source(const QString &node, const QString &input) const {
    const InputValue value = m_canvas->m_document.graph.nodes.value(node).inputs.value(input);
    if (value.kind() != InputValue::Kind::Link)
        return {};
    const PortRef from = value.from();
    const int row = m_canvas->rowOf(from.node);
    const QString title = row >= 0 ? m_canvas->m_rows[size_t(row)].title : from.node;
    const CanvasModel::Port *port = m_canvas->findPort(from.node, from.port, true);
    return QStringLiteral("%1 · %2").arg(title, port ? port->label : from.port);
}

QString WidgetEdit::parse(const Target &t, const QString &text, QJsonValue *value, bool *unsetIt) const {
    const InputSpec &s = *t.spec;
    *unsetIt = false;
    const bool canUnset = s.optional || !s.defaultValue.isUndefined();
    if (t.widget == "spin" || t.widget == "slider" || t.widget == "seed") {
        QString number = text.trimmed();
        if (number.isEmpty()) {
            if (!canUnset)
                return tr("%1 needs a value.").arg(label(s));
            *unsetIt = true;
            return {};
        }
        // A comma is the decimal separator in many languages; there is no grouping here. A
        // comma before exactly three digits (1,000) or two separators (1.000,5) reads as
        // grouping in some language, so it is refused rather than taken for another number.
        static const QRegularExpression grouped(QStringLiteral("^[+-]?\\d+,\\d{3}$"));
        if (number.count(',') + number.count('.') > 1 || grouped.match(number).hasMatch())
            return tr("%1 has a thousands separator or two decimal separators; type it without grouping, "
                      "as 1000 or 0.5.").arg(number);
        number.replace(',', '.');
        bool ok = false;
        const double d = number.toDouble(&ok);
        if (!ok || !std::isfinite(d))
            return tr("%1 is not a number.").arg(text.trimmed());
        *value = d;
        return {};
    }
    if (t.widget == "model") {
        const QString name = text.trimmed();
        if (name.isEmpty()) {
            if (!s.optional)
                return tr("%1 needs a value.").arg(label(s));
            *unsetIt = true;
            return {};
        }
        *value = QJsonObject{{"resource", s.type.type.toString()}, {"name", name}};
        return {};
    }
    const QString textValue = t.widget == "color" ? text.trimmed() : text;
    if (textValue.isEmpty() && s.optional && s.defaultValue.isUndefined()) {
        *unsetIt = true;
        return {};
    }
    *value = textValue;
    return {};
}

QString WidgetEdit::checkValue(const Target &t, const QJsonValue &value) const {
    const Errors errors = checkLiteral(*t.spec, value, t.kind);
    if (errors.isEmpty())
        return {};
    const Error &first = errors.first();
    return first.hint.isEmpty() ? first.message : first.message + ' ' + first.hint;
}

QString WidgetEdit::check(const QString &node, const QString &input, const QString &text) const {
    const Target t = target(node, input);
    if (!t.spec)
        return tr("%1 cannot be edited here.").arg(input);
    if (m_canvas->m_document.graph.nodes.value(node).inputs.value(input).kind() == InputValue::Kind::Link)
        return tr("%1 is linked; take the link out to type a value.").arg(label(*t.spec));
    QJsonValue value;
    bool unsetIt = false;
    if (const QString problem = parse(t, text, &value, &unsetIt); !problem.isEmpty())
        return problem;
    return unsetIt ? QString() : checkValue(t, value);
}

WidgetEdit::~WidgetEdit() {
    // A model being hashed, or headers being read, are not waited for: the next pick or open
    // reads them again.
    m_stopHashing->store(true);
    m_hashPool.waitForDone();
}

void WidgetEdit::write(const QString &node, const QString &input, const QJsonValue &value, const QString &text) {
    Document document = m_canvas->scopeDocument();
    if (!document.graph.nodes.contains(node))
        return;
    // Any other value for the input replaces a file still being hashed for it.
    if (dropPick(node, input)) {
        ++m_hashRevision;
        emit hashingChanged();
    }
    Node &n = document.graph.nodes[node];
    if (value.isUndefined())
        n.inputs.remove(input);
    else
        n.inputs.insert(input, InputValue::fromJson(value));
    // The session's own value goes into its open edit; anything else is an edit of its own,
    // which ends the session first (CanvasModel::beginEdit).
    const bool inSession = editing() && m_session.node == node && m_session.input == input;
    const bool wasWriting = m_writing;
    m_writing = inSession;
    m_canvas->commit(document, text);
    m_writing = wasWriting;
}

QString WidgetEdit::stepText(const QString &node, const QString &input) const {
    const InputSpec *spec = m_canvas->inputSpec(node, input);
    return tr("Edit %1").arg(spec ? label(*spec) : input);
}

QString WidgetEdit::setText(const QString &node, const QString &input, const QString &text) {
    if (const QString problem = check(node, input, text); !problem.isEmpty())
        return problem;
    QJsonValue value;
    bool unsetIt = false;
    parse(target(node, input), text, &value, &unsetIt);
    write(node, input, unsetIt ? QJsonValue(QJsonValue::Undefined) : value, stepText(node, input));
    return {};
}

QString WidgetEdit::setValue(const QString &node, const QString &input, const QVariant &value) {
    const Target t = target(node, input);
    if (!t.spec)
        return tr("%1 cannot be edited here.").arg(input);
    if (m_canvas->m_document.graph.nodes.value(node).inputs.value(input).kind() == InputValue::Kind::Link)
        return tr("%1 is linked; take the link out to type a value.").arg(label(*t.spec));
    const QJsonValue json = QJsonValue::fromVariant(value);
    if (json.isUndefined() || json.isNull())
        return tr("%1 needs a value.").arg(label(*t.spec));
    if (const QString problem = checkValue(t, json); !problem.isEmpty())
        return problem;
    write(node, input, json, stepText(node, input));
    return {};
}

QString WidgetEdit::setPath(const QString &node, const QString &input, const QVariant &url) {
    const QUrl asUrl = url.toUrl();
    const QString path = asUrl.isLocalFile() ? asUrl.toLocalFile() : url.toString();
    return setText(node, input, path);
}

QString WidgetEdit::setColor(const QString &node, const QString &input, const QColor &color) {
    if (!color.isValid())
        return tr("%1 is not a color.").arg(color.name());
    QString text = color.name(QColor::HexRgb);
    if (color.alpha() != 255) // HexArgb is #aarrggbb; 6.2 wants the alpha last
        text += color.name(QColor::HexArgb).mid(1, 2);
    return setText(node, input, text);
}

bool WidgetEdit::canPasteImage(const QString &node, const QString &input) const {
    return m_canvas->images() && m_canvas->images()->accepts(node, input);
}

bool WidgetEdit::pasteImage(const QString &node) {
    return m_canvas->images() && m_canvas->images()->pasteInto(node);
}

QVariantList WidgetEdit::recentInputs() const {
    return m_canvas->images() ? m_canvas->images()->recent() : QVariantList();
}

bool WidgetEdit::useInput(const QString &node, const QString &path) {
    return m_canvas->images() && m_canvas->images()->use(node, path);
}

void WidgetEdit::unset(const QString &node, const QString &input) {
    if (!target(node, input).spec || !m_canvas->m_document.graph.nodes.contains(node))
        return;
    write(node, input, QJsonValue::Undefined, stepText(node, input));
}

// --- sessions ------------------------------------------------------------------

void WidgetEdit::begin(const QString &node, const QString &input) {
    if (editing())
        finish();
    if (!target(node, input).spec || !m_canvas->m_document.graph.nodes.contains(node))
        return;
    // Opened before the session exists, so it is not taken for another edit.
    m_canvas->beginEdit();
    m_session = {node, input, m_canvas->m_document.graph.nodes.value(node).inputs.value(input), true};
}

QString WidgetEdit::preview(const QString &text) {
    if (!editing())
        return tr("The edit has ended.");
    if (!m_canvas->m_document.graph.nodes.contains(m_session.node))
        return tr("The node is gone.");
    return setText(m_session.node, m_session.input, text);
}

QString WidgetEdit::previewValue(const QVariant &value) {
    if (!editing())
        return tr("The edit has ended.");
    if (!m_canvas->m_document.graph.nodes.contains(m_session.node))
        return tr("The node is gone.");
    return setValue(m_session.node, m_session.input, value);
}

void WidgetEdit::finish() {
    if (!editing())
        return;
    const QString text = stepText(m_session.node, m_session.input);
    m_session = {};
    if (m_canvas->m_editDepth > 0)
        m_canvas->endEdit(text);
}

void WidgetEdit::endSession() {
    if (!editing())
        return;
    finish();
    emit sessionEnded();
}

void WidgetEdit::cancel() {
    if (!editing())
        return;
    if (m_canvas->m_document.graph.nodes.contains(m_session.node)) {
        const QJsonValue before = m_session.before.isEmpty() ? QJsonValue(QJsonValue::Undefined)
                                                             : m_session.before.toJson();
        write(m_session.node, m_session.input, before, stepText(m_session.node, m_session.input));
    }
    finish();
}

// --- numbers -------------------------------------------------------------------

double WidgetEdit::stepped(const QString &node, const QString &input, double from, int steps) const {
    const Target t = target(node, input);
    if (!t.spec)
        return from;
    return clamped(rounded(from + steps * stepOf(*t.spec), *t.spec), *t.spec);
}

double WidgetEdit::valueAt(const QString &node, const QString &input, double share) const {
    const Target t = target(node, input);
    if (!t.spec)
        return 0;
    const InputSpec &s = *t.spec;
    const double low = s.min.value_or(0), high = s.max.value_or(1);
    const double step = stepOf(s);
    const double v = low + std::round(std::clamp(share, 0.0, 1.0) * (high - low) / step) * step;
    return clamped(rounded(v, s), s);
}

double WidgetEdit::fraction(const QString &node, const QString &input, double value) const {
    const Target t = target(node, input);
    if (!t.spec)
        return 0;
    const double low = t.spec->min.value_or(0), high = t.spec->max.value_or(1);
    return high > low ? std::clamp((value - low) / (high - low), 0.0, 1.0) : 0;
}

QString WidgetEdit::numberText(double value) {
    if (std::abs(value) <= maxSeed && value == std::floor(value))
        return QString::number(qint64(value));
    return QString::number(value, 'g', 15);
}

// --- seeds ---------------------------------------------------------------------

QString WidgetEdit::seedMode(const QString &node, const QString &input) const {
    if (target(node, input).widget != QLatin1String("seed"))
        return {};
    const InputValue value = m_canvas->m_document.graph.nodes.value(node).inputs.value(input);
    if (value.kind() == InputValue::Kind::Auto)
        return QStringLiteral("random");
    return incrementing(m_canvas->m_document, node).contains(input) ? QStringLiteral("increment")
                                                                     : QStringLiteral("fixed");
}

double WidgetEdit::randomSeed(const InputSpec &spec) const {
    const double low = std::max(0.0, spec.min.value_or(0)), high = std::min(maxSeed, spec.max.value_or(maxSeed));
    if (high <= low)
        return low;
    return low + double(QRandomGenerator::global()->bounded(quint64(high - low) + 1));
}

void WidgetEdit::setSeedMode(const QString &node, const QString &input, const QString &mode) {
    const Target t = target(node, input);
    if (t.widget != QLatin1String("seed") || !QStringList{"fixed", "random", "increment"}.contains(mode))
        return;
    Document document = m_canvas->scopeDocument();
    Node &n = document.graph.nodes[node];
    if (n.inputs.value(input).kind() == InputValue::Kind::Link)
        return;
    if (mode == QLatin1String("random"))
        n.inputs.insert(input, InputValue::autoRandom());
    else if (n.inputs.value(input).kind() == InputValue::Kind::Auto)
        n.inputs.insert(input, InputValue::literal(lastSeed(node, input).value_or(randomSeed(*t.spec))));
    setIncrementing(document, node, input, mode == QLatin1String("increment"));
    m_canvas->commit(document, stepText(node, input));
}

std::optional<double> WidgetEdit::lastSeed(const QString &node, const QString &input) const {
    // The value a run reported on the output of the same name ("Seed used"), as shown.
    bool ok = false;
    const double seed = m_canvas->m_run.value(node).values.value(input).toString().toDouble(&ok);
    return ok ? std::optional<double>(seed) : std::nullopt;
}

void WidgetEdit::advanceSeeds() {
    Document document = m_canvas->scopeDocument();
    for (auto it = document.graph.nodes.begin(); it != document.graph.nodes.end(); ++it) {
        for (const QString &input : incrementing(document, it.key())) {
            const Target t = target(it.key(), input);
            const InputValue value = it.value().inputs.value(input);
            if (t.widget != QLatin1String("seed")
                || (!value.isEmpty() && value.kind() != InputValue::Kind::Literal))
                continue;
            const double now = value.isEmpty() ? t.spec->defaultValue.toDouble() : value.literal().toDouble();
            double next = now + 1;
            if (t.spec->max && next > *t.spec->max)
                next = t.spec->min.value_or(0);
            it.value().inputs.insert(input, InputValue::literal(next));
        }
    }
    m_canvas->commit(document, tr("Increment seeds"));
}

// --- the model picker ----------------------------------------------------------

QStringList WidgetEdit::capabilities(const QString &node, const QString &input) const {
    if (!m_registry)
        return {};
    const Document &document = m_canvas->scopeDocument();
    QString type = document.graph.nodes.value(node).type;
    int major = document.graph.nodes.value(node).version.major;
    if (!m_registry->nodeType(type)) {
        // A group's exposed input: the node type of the inner input it drives.
        const GroupDefinition *group = document.group(type);
        if (!group)
            return {};
        for (const GroupInput &exposed : group->inputs)
            if (exposed.name == input) {
                const Node &inner = group->nodes.value(exposed.targets.value(0).node);
                type = inner.type;
                major = inner.version.major;
            }
    }
    const NodeType *installed = m_registry->nodeTypeFor(type, major);
    QStringList list;
    if (installed)
        for (const QJsonValue &capability : installed->capabilities.toArray())
            list << capability.toString();
    return list;
}

QString WidgetEdit::modelsKey(const QString &node, const QString &input) const {
    const Target t = target(node, input);
    if (t.widget != QLatin1String("model"))
        return {};
    return t.spec->type.type.toString() + QLatin1Char('\n') + capabilities(node, input).join(QLatin1Char(','));
}

void WidgetEdit::requestModels(const QString &node, const QString &input) {
    const QString key = modelsKey(node, input);
    if (key.isEmpty())
        return;
    const PortType &portType = target(node, input).spec->type.type;
    const QString type = portType.toString();
    m_diskModels.insert(key, listDiskModels(library(), portType, hashes()));
    requestFits(node, input);
    if (!m_source)
        m_source = listOfferedModels;
    m_loading.insert(key);
    ++m_modelsRevision;
    emit modelsChanged();
    QPointer<WidgetEdit> self(this);
    m_source(type, capabilities(node, input), [self, key](QList<Model> models) {
        if (!self)
            return;
        self->m_models.insert(key, std::move(models));
        self->m_loading.remove(key);
        ++self->m_modelsRevision;
        emit self->modelsChanged();
    });
}

namespace {
const QString kDiffusionModel = QStringLiteral("fugue/diffusion-model");
const QString kModel = QStringLiteral("fugue/model");
const QString kTextEncoder = QStringLiteral("fugue/text-encoder");
const QString kVae = QStringLiteral("fugue/vae");
const QString kLora = QStringLiteral("fugue/lora");
} // namespace

// The most links followed back to a diffusion model: a chain of Apply LoRAs longer than that
// gets no list order.
constexpr int kMostModelSteps = 16;

QJsonObject WidgetEdit::diffusionModelOf(const QString &node, int depth) const {
    if (depth > kMostModelSteps)
        return {};
    const auto &inputs = m_canvas->scopeDocument().graph.nodes.value(node).inputs;
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        const InputSpec *spec = m_canvas->inputSpec(node, it.key());
        if (!spec)
            continue;
        const QString type = spec->type.type.toString();
        if (type == kDiffusionModel && it.value().kind() == InputValue::Kind::Resource)
            return it.value().handle();
        if (type == kModel && it.value().kind() == InputValue::Kind::Link) // Apply LoRA's, from Load Models
            if (QJsonObject found = diffusionModelOf(it.value().from().node, depth + 1); !found.isEmpty())
                return found;
    }
    return {};
}

QString WidgetEdit::fitsKey(const QString &node, const QString &input) { return node + QLatin1Char('\n') + input; }

void WidgetEdit::requestFits(const QString &node, const QString &input) {
    const QString key = fitsKey(node, input);
    const Target t = target(node, input);
    const QString part = t.spec->type.type.toString();
    // A part is judged against the diffusion model; a diffusion model or checkpoint only by
    // whether the helper can load it at all.
    const bool judgedAlone = part == kDiffusionModel || part == kModel;
    if (part != kTextEncoder && part != kVae && part != kLora && !judgedAlone) {
        m_fits.remove(key);
        return;
    }
    Fits &fits = m_fits[key];
    fits.byPath.clear(); // those of another diffusion model, or files no longer listed
    fits.model.clear();
    fits.asked = ++m_fitsAsked;
    // The diffusion model on the level shown: the node's (Load Models), or the one its model
    // comes from (Apply LoRA). A check reads no model file to find it: the one with the
    // handle's name will do.
    QString diffusion;
    if (!judgedAlone) {
        const QJsonObject handle = diffusionModelOf(node);
        const Result<QString> found =
            handle.isEmpty() ? Result<QString>(Error{}) : library().resolve(handle, hashes(), ModelLibrary::Hashing::RememberedOnly);
        if (!found)
            return;
        diffusion = found.value();
        fits.model = handle.value(QStringLiteral("name")).toString();
    }
    QStringList paths;
    for (const Model &model : m_diskModels.value(modelsKey(node, input)))
        paths << model.path;
    using Rule = Fit (*)(const ModelShape &, const ModelShape &);
    const Rule rule = part == kTextEncoder ? Rule(loadableTextEncoderFits)
                      : part == kVae        ? Rule(vaeFits)
                      : part == kLora       ? Rule([](const ModelShape &model, const ModelShape &lora) { return loraFits(model, lora); })
                                            : nullptr;
    // Whether `shape` fits `wanted`, and why not when it cannot be loaded; nothing when its
    // header tells nothing.
    const auto judge = [rule](const std::optional<ModelShape> &wanted,
                              const std::optional<ModelShape> &shape) -> std::optional<Judged> {
        if (shape && !shape->unloadable.isEmpty())
            return Judged{false, tr("Can't load: %1").arg(shape->unloadable)};
        const Fit fit = !rule || !wanted || !shape ? Fit::Unknown : rule(*wanted, *shape);
        return fit == Fit::Unknown ? std::nullopt : std::optional<Judged>(Judged{fit == Fit::Fits, {}});
    };

    // Headers read before: at once, so a menu opened again does not reorder under the pointer.
    std::optional<ModelShape> wanted;
    bool allKnown = diffusion.isEmpty() || m_shapes->known(diffusion, &wanted);
    QHash<QString, Judged> known;
    for (const QString &path : std::as_const(paths)) {
        std::optional<ModelShape> shape;
        if (!allKnown || !m_shapes->known(path, &shape)) {
            allKnown = false;
            break;
        }
        if (const std::optional<Judged> judged = judge(wanted, shape))
            known.insert(path, *judged);
    }
    if (allKnown) {
        fits.byPath = known;
        return;
    }
    // Else read on the pool; a later request, or the window closing, makes this one moot.
    QPointer<WidgetEdit> self(this);
    ModelShapes *shapes = m_shapes;
    std::shared_ptr<std::atomic_bool> stop = m_stopHashing;
    const int asked = fits.asked;
    m_shapePool.start([self, shapes, stop, judge, key, asked, diffusion, paths] {
        const std::optional<ModelShape> wanted = diffusion.isEmpty() ? std::nullopt : shapes->of(diffusion);
        QHash<QString, Judged> read;
        for (const QString &path : paths) {
            if (stop->load())
                return;
            if (const std::optional<Judged> judged = judge(wanted, shapes->of(path)))
                read.insert(path, *judged);
        }
        QMetaObject::invokeMethod(self, [self, key, asked, read] {
            if (!self || self->m_fits.value(key).asked != asked)
                return;
            self->m_fits[key].byPath = read;
            ++self->m_modelsRevision;
            emit self->modelsChanged();
        });
    });
}

QString WidgetEdit::fitsModel(const QString &node, const QString &input) const {
    return m_fits.value(fitsKey(node, input)).model;
}

QVariantList WidgetEdit::models(const QString &node, const QString &input) const {
    const QString key = modelsKey(node, input);
    if (key.isEmpty())
        return {};
    const QHash<QString, Judged> fits = m_fits.value(fitsKey(node, input)).byPath;
    QVariantList list;
    for (const Model &model : m_models.value(key) + m_diskModels.value(key)) {
        QVariantMap entry{{"name", model.name},
                          {"title", model.title.isEmpty() ? model.name : model.title},
                          {"handle", model.handle.toVariantMap()},
                          {"status", model.status},
                          {"provider", model.provider},
                          {"note", model.note}};
        if (model.onDisk()) {
            entry.insert("path", model.path);
            entry.insert("folder", model.folder);
            entry.insert("size", model.size);
            if (const auto fit = fits.constFind(model.path); fit != fits.constEnd()) {
                entry.insert("fits", fit->fits); // absent when the file's header tells nothing
                if (!fit->why.isEmpty())
                    entry.insert("fitNote", fit->why);
            }
        }
        list.append(entry);
    }
    return list;
}

bool WidgetEdit::modelsLoading(const QString &node, const QString &input) const {
    const QString key = modelsKey(node, input);
    return !key.isEmpty() && m_loading.contains(key);
}

QString WidgetEdit::modelsEmptyText(const QString &node, const QString &input) const {
    const QStringList wanted = capabilities(node, input);
    const bool image = std::any_of(wanted.begin(), wanted.end(), [](const QString &capability) {
        return capability.startsWith(QLatin1String("image."));
    });
    if (image)
        return tr("No image model available: start one in omarchy-local-ai or add a provider");
    if (wanted.contains(QStringLiteral("text.generate")))
        return tr("No text model available: start one in omarchy-local-ai or add a provider");
    // A model file of this kind goes in its folder in Fugue's models folder (8.9).
    const Target t = target(node, input);
    const QString folder = t.spec ? library().folders(t.spec->type.type).value(0) : QString();
    if (folder.isEmpty())
        return tr("No model available: start one in omarchy-local-ai or add a provider");
    return tr("No model found: put one in %1, or add its folder to Extra model folders in Settings").arg(folder);
}

QString WidgetEdit::setModel(const QString &node, const QString &input, const QVariantMap &handle) {
    if (target(node, input).widget != QLatin1String("model"))
        return tr("%1 cannot be edited here.").arg(input);
    return setValue(node, input, handle);
}

QString WidgetEdit::setModelName(const QString &node, const QString &input, const QString &name) {
    if (target(node, input).widget != QLatin1String("model"))
        return tr("%1 cannot be edited here.").arg(input);
    return setText(node, input, name);
}

// --- files on disk -------------------------------------------------------------

ModelLibrary WidgetEdit::library() const {
    if (m_modelRoots)
        return ModelLibrary(*m_modelRoots, m_registry);
    return ModelLibrary::current(m_registry); // with what each model folder is set to hold (#86)
}

FileHashes &WidgetEdit::hashes() {
    if (!m_hashes)
        m_hashes = std::make_shared<FileHashes>(FileHashes::defaultPath());
    return *m_hashes;
}

QString WidgetEdit::setDiskModel(const QString &node, const QString &input, const QString &path) {
    const Target t = target(node, input);
    if (t.widget != QLatin1String("model"))
        return tr("%1 cannot be edited here.").arg(input);
    if (m_canvas->scopeDocument().graph.nodes.value(node).inputs.value(input).kind() == InputValue::Kind::Link)
        return tr("%1 is linked; take the link out to pick a model.").arg(label(*t.spec));
    // Only a file the library lists for the input's type: its name is what finds it again.
    const PortType &type = t.spec->type.type;
    // The files the picker listed, else the folders walked now.
    const QString key = modelsKey(node, input);
    QList<Model> files = m_diskModels.value(key);
    if (files.isEmpty())
        files = listDiskModels(library(), type, hashes());
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const auto file = std::find_if(files.begin(), files.end(), [&](const Model &each) {
        return !canonical.isEmpty() && QFileInfo(each.path).canonicalFilePath() == canonical;
    });
    if (file == files.end())
        return tr("%1 is not a model file of this kind in the model folders.").arg(QFileInfo(path).fileName());
    if (const QString known = hashes().known(file->path); !known.isEmpty())
        return setValue(node, input, diskHandle(type.toString(), file->name, known).toVariantMap());
    dropPick(node, input);
    Pick pick;
    pick.scope = m_canvas->scopePath();
    pick.node = node;
    pick.input = input;
    pick.path = file->path;
    pick.resource = type.toString();
    pick.name = file->name;
    m_picks.append(pick);
    startHashing(file->path);
    ++m_hashRevision;
    emit hashingChanged();
    return {};
}

void WidgetEdit::startHashing(const QString &path) {
    if (m_hashing.contains(path))
        return;
    m_hashing.insert(path);
    QPointer<WidgetEdit> self(this);
    hashes(); // made here, on the window's thread
    std::shared_ptr<FileHashes> store = m_hashes;
    std::shared_ptr<std::atomic_bool> stop = m_stopHashing;
    m_hashPool.start([self, store, stop, path] {
        int shown = -1; // percent last told, so the window is told at most a hundred times
        const QString hash = store->hash(path, [&](qint64 done, qint64 total) {
            const int percent = total > 0 ? int(done * 100 / total) : 100;
            if (percent != shown) {
                shown = percent;
                QMetaObject::invokeMethod(self, [self, path, percent] {
                    if (!self)
                        return;
                    bool moved = false;
                    for (Pick &pick : self->m_picks)
                        if (pick.path == path) {
                            pick.progress = percent / 100.0;
                            moved = true;
                        }
                    if (moved) {
                        ++self->m_hashRevision;
                        emit self->hashingChanged();
                    }
                });
            }
            return !stop->load();
        });
        if (stop->load())
            return;
        if (!hash.isEmpty())
            store->save();
        QMetaObject::invokeMethod(self, [self, path, hash] {
            if (self)
                self->hashed(path, hash);
        });
    });
}

void WidgetEdit::hashed(const QString &path, const QString &hash) {
    m_hashing.remove(path);
    for (Pick &pick : m_picks) {
        if (pick.path != path)
            continue;
        if (hash.isEmpty())
            pick.error = tr("%1 could not be read.").arg(QFileInfo(path).fileName());
        else
            pick.hash = hash;
    }
    storePicks();
    ++m_hashRevision;
    emit hashingChanged();
}

void WidgetEdit::storePicks() {
    const QString scope = m_canvas->scopePath();
    QList<Pick> ready;
    for (qsizetype i = m_picks.size() - 1; i >= 0; --i)
        if (m_picks.at(i).scope == scope && !m_picks.at(i).hash.isEmpty())
            ready.prepend(m_picks.takeAt(i));
    // Written only while the node still has the input: it may be gone by now.
    for (const Pick &pick : std::as_const(ready))
        if (target(pick.node, pick.input).widget == QLatin1String("model"))
            setValue(pick.node, pick.input, diskHandle(pick.resource, pick.name, pick.hash).toVariantMap());
}

const WidgetEdit::Pick *WidgetEdit::pickFor(const QString &node, const QString &input) const {
    const QString scope = m_canvas->scopePath();
    for (const Pick &pick : m_picks)
        if (pick.scope == scope && pick.node == node && pick.input == input)
            return &pick;
    return nullptr;
}

bool WidgetEdit::dropPick(const QString &node, const QString &input) {
    const QString scope = m_canvas->scopePath();
    return m_picks.removeIf([&](const Pick &pick) {
        return pick.scope == scope && pick.node == node && pick.input == input;
    }) > 0;
}

QVariantMap WidgetEdit::hashState(const QString &node, const QString &input) const {
    const Pick *pick = pickFor(node, input);
    if (!pick)
        return {};
    if (!pick->error.isEmpty())
        return {{"error", pick->error}};
    return {{"progress", pick->progress}, {"name", pick->name}};
}
