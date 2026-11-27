#include "canvas/widget_edit.h"

#include "workflow/image_inputs.h"
#include "workflow/model_downloads.h"
#include "workflow/model_picker.h"

#include "canvas/canvas_model.h"
#include "canvas/input_links.h"
#include "engine/file_hashes.h"
#include "engine/model_download.h"
#include "engine/model_family.h"
#include "engine/model_library.h"
#include "engine/model_speed.h"
#include "engine/model_use.h"
#include "engine/registry.h"
#include "engine/settings.h"
#include "typed_number.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QUndoStack>
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
    connect(&m_canvases, &CanvasChoices::changed, this, &WidgetEdit::canvasesChanged);
    const auto bump = [this] {
        ++m_graphRevision;
        emit graphChanged();
    };
    connect(canvas, &CanvasModel::documentReset, this, bump);
    connect(canvas, &CanvasModel::undoChanged, this, bump);
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

WidgetEdit::Target WidgetEdit::canvasTarget(const QString &node, const QString &input) const {
    const Target t = target(node, input);
    return t.widget == QLatin1String("canvas") ? t : Target{};
}

QVariantList WidgetEdit::canvases() { return m_canvases.rows(); }

QVariantMap WidgetEdit::canvasRow(const QString &id) { return id.isEmpty() ? QVariantMap() : m_canvases.row(id); }

QString WidgetEdit::useCanvas(const QString &node, const QString &input, const QString &id) {
    if (!canvasTarget(node, input).spec)
        return tr("%1 is not a canvas input.").arg(input);
    return setText(node, input, id);
}

QString WidgetEdit::importCanvas(const QString &node, const QString &input, const QString &path, const QString &name) {
    if (!canvasTarget(node, input).spec)
        return tr("%1 is not a canvas input.").arg(input);
    QString id;
    if (const QString why = m_canvases.makeFromFile(path, name, &id); !why.isEmpty())
        return why;
    return setText(node, input, id);
}

namespace {
const char *const kRegionNumbers[] = {"x", "y", "width", "height", "padding", "scale", "max_megapixels", "multiple"};
}

QVariantMap WidgetEdit::regionState(const QString &node) const {
    if (!canvasTarget(node, "canvas").spec)
        return {};
    for (const char *name : kRegionNumbers)
        if (!target(node, QLatin1String(name)).spec)
            return {};
    const Node &n = m_canvas->m_document.graph.nodes.value(node);
    const auto linked = [&n](const char *name) { return n.inputs.value(QLatin1String(name)).kind() == InputValue::Kind::Link; };
    const auto number = [&](const char *name) {
        QVariant stored = value(node, QLatin1String(name));
        return (stored.isValid() ? stored : spec(node, QLatin1String(name)).value("default")).toDouble();
    };
    const auto text = [&](const char *name) {
        QVariant stored = value(node, QLatin1String(name));
        return (stored.isValid() ? stored : spec(node, QLatin1String(name)).value("default")).toString();
    };
    QVariantMap state{{"ok", true},
                      {"canvas", text("canvas")},
                      {"revision", text("revision").isEmpty() ? QStringLiteral("head") : text("revision")},
                      {"storeRoot", m_canvases.storeRoot()},
                      {"canvasLinked", linked("canvas")},
                      {"maskLinked", linked("mask")},
                      {"revisionLinked", linked("revision")},
                      {"editable", !(linked("x") || linked("y") || linked("width") || linked("height"))},
                      // Region 1.2 (#230): the selection painted in a Canvas view, a hash; "" for none (an older Region has no such input).
                      {"selection", target(node, QStringLiteral("selection")).spec ? text("selection") : QString()},
                      {"selectionWritable", target(node, QStringLiteral("selection")).spec != nullptr && !linked("selection")}};
    for (const char *name : kRegionNumbers)
        state.insert(QLatin1String(name) == QLatin1String("max_megapixels") ? QStringLiteral("cap") : QLatin1String(name), number(name));
    // Region 1.3 (#242): the target size and the feather shape the model's size; an older Region has no target (0).
    state.insert(QStringLiteral("target"), target(node, QStringLiteral("target_megapixels")).spec ? number("target_megapixels") : 0.0);
    state.insert(QStringLiteral("feather"), target(node, QStringLiteral("feather")).spec ? number("feather") : 0.0);
    return state;
}

bool WidgetEdit::setRegion(const QString &node, int x, int y, int width, int height) {
    const QVariantMap now = regionState(node);
    if (now.isEmpty() || !now.value("editable").toBool())
        return false;
    const QVariantMap wanted{{"x", x}, {"y", y}, {"width", width}, {"height", height}};
    QStringList changed;
    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it)
        if (it.value().toInt() != now.value(it.key()).toInt())
            changed << it.key();
    // A rectangle drawn replaces a painted selection (as a new selection does in Photoshop): the node reads the rectangle.
    const bool dropSelection = !now.value("selection").toString().isEmpty() && now.value("selectionWritable").toBool();
    if (changed.isEmpty() && !dropSelection)
        return false;
    m_canvas->beginEdit();
    for (const QString &name : std::as_const(changed))
        setValue(node, name, wanted.value(name).toInt());
    if (dropSelection)
        setText(node, QStringLiteral("selection"), QString());
    m_canvas->endEdit(tr("Pick region"));
    return true;
}

bool WidgetEdit::setSelection(const QString &node, const QString &selection, int x, int y, int width, int height) {
    const QVariantMap now = regionState(node);
    if (now.isEmpty() || !now.value("selectionWritable").toBool())
        return false;
    const bool rect = now.value("editable").toBool();
    const QVariantMap wanted{{"x", x}, {"y", y}, {"width", width}, {"height", height}};
    QStringList changed;
    for (auto it = wanted.cbegin(); rect && it != wanted.cend(); ++it)
        if (it.value().toInt() != now.value(it.key()).toInt())
            changed << it.key();
    if (changed.isEmpty() && now.value("selection").toString() == selection)
        return false;
    m_canvas->beginEdit();
    setText(node, QStringLiteral("selection"), selection);
    for (const QString &name : std::as_const(changed))
        setValue(node, name, wanted.value(name).toInt());
    m_canvas->endEdit(tr("Use selection"));
    return true;
}

QUndoStack *WidgetEdit::undoStack() const { return m_canvas->undoStack(); }


QString WidgetEdit::nodeName(const QString &node) const { return m_canvas ? m_canvas->nodeName(node) : QString(); }

QVariantList WidgetEdit::regionNodes() const {
    QVariantList out;
    if (!m_canvas)
        return out;
    for (const QString &id : m_canvas->m_document.graph.nodes.keys()) {
        const QVariantMap state = regionState(id);
        if (state.value("ok").toBool()) {
            const QString name = nodeName(id);
            out.append(QVariantMap{{"id", id}, {"name", name.isEmpty() ? id : name}, {"canvas", state.value("canvas")}});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("name").toString().localeAwareCompare(b.toMap().value("name").toString()) < 0;
    });
    return out;
}

QString WidgetEdit::addRegionNode(const QString &canvas, const QVariantMap &rect) {
    if (!m_canvas || canvas.isEmpty())
        return {};
    const QVariantList center = m_canvas->view().value("center").toList();
    const qreal x = center.size() == 2 ? center.at(0).toReal() : 0, y = center.size() == 2 ? center.at(1).toReal() : 0;
    QUndoStack *stack = m_canvas->undoStack();
    stack->beginMacro(tr("Add a Canvas Region"));
    const QString id = m_canvas->addNode(QStringLiteral("fugue/canvas-region"), x, y);
    if (!id.isEmpty()) {
        setText(id, QStringLiteral("canvas"), canvas);
        setRegion(id, rect.value("x").toInt(), rect.value("y").toInt(), rect.value("width").toInt(), rect.value("height").toInt());
    }
    stack->endMacro();
    return id;
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
        // One rule for every typed number (typed_number.h): a point always, a comma only where the
        // language writes the decimal point as one, never a thousands separator. Numbers in a
        // workflow file are JSON numbers and never come through here.
        const QLocale locale;
        if (const std::optional<double> d = fugue::TypedNumber::read(number, locale)) {
            *value = *d;
            return {};
        }
        if (number.count(',') + number.count('.') > 1
            || (number.contains(',') && locale.decimalPoint() != QStringLiteral(",")))
            return tr("%1 has a thousands separator or a decimal separator this language does not use; type it "
                      "without grouping, as 1000 or %2.").arg(number, locale.toString(0.5));
        return tr("%1 is not a number.").arg(text.trimmed());
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
    // reads them again (m_work stops them as it goes).
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

Document WidgetEdit::holdUpstream(const QString &node, QSet<QString> *held) const {
    Document level = m_canvas->scopeDocument();
    // Every node a path of links leads from to `node`, not `node` itself.
    QSet<QString> upstream;
    QStringList open{node};
    while (!open.isEmpty()) {
        const Node &current = level.graph.nodes.value(open.takeLast());
        const auto add = [&](const InputValue &value) {
            if (value.kind() == InputValue::Kind::Link && level.graph.nodes.contains(value.from().node)
                && !upstream.contains(value.from().node)) {
                upstream.insert(value.from().node);
                open << value.from().node;
            }
        };
        for (const InputValue &value : current.inputs) {
            add(value);
            if (value.kind() == InputValue::Kind::List)
                for (const InputValue &item : value.items())
                    add(item);
        }
    }
    upstream.remove(node); // a loop cannot compile, but the end point's own seeds are never held
    for (const QString &id : upstream) {
        Node &held_node = level.graph.nodes[id];
        QStringList inputs = incrementing(level, id);
        for (auto it = held_node.inputs.cbegin(); it != held_node.inputs.cend(); ++it)
            if (it.value().kind() == InputValue::Kind::Auto && !inputs.contains(it.key()))
                inputs << it.key();
        for (const QString &input : inputs) {
            const QString mode = seedMode(id, input);
            const std::optional<double> seed = lastSeed(id, input);
            if ((mode != QLatin1String("random") && mode != QLatin1String("increment")) || !seed)
                continue;
            held_node.inputs.insert(input, InputValue::literal(*seed));
            if (held && mode == QLatin1String("increment"))
                held->insert(id + QLatin1Char('/') + input);
        }
    }
    return m_canvas->merged(level);
}

void WidgetEdit::advanceSeeds() { advanceSeedsExcept({}); }

void WidgetEdit::advanceSeedsExcept(const QSet<QString> &held) {
    Document document = m_canvas->scopeDocument();
    for (auto it = document.graph.nodes.begin(); it != document.graph.nodes.end(); ++it) {
        for (const QString &input : incrementing(document, it.key())) {
            if (held.contains(it.key() + QLatin1Char('/') + input))
                continue;
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
    if (!m_catalogSet)
        m_catalog = ModelCatalog::load(); // read each time: a file dropped in the catalog folder shows
    m_catalogMissing.insert(key, m_catalog.missing(type, library(), portType, hashes()));
    requestFits(node, input);
    requestMarks(node, input);
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
    const Rule rule = part == kTextEncoder ? (input == QLatin1String("clip_l") ? Rule(loadableClipFits) : Rule(loadableTextEncoderFits))
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
    ModelShapes *shapes = m_shapes;
    const std::shared_ptr<std::atomic_bool> stop = m_work.stopFlag();
    const int asked = fits.asked;
    m_work.read(this, [this, shapes, stop, judge, key, asked, diffusion, paths]() -> std::function<void()> {
        const std::optional<ModelShape> wanted = diffusion.isEmpty() ? std::nullopt : shapes->of(diffusion);
        QHash<QString, Judged> read;
        for (const QString &path : paths) {
            if (stop->load())
                return {};
            if (const std::optional<Judged> judged = judge(wanted, shapes->of(path)))
                read.insert(path, *judged);
        }
        return [this, key, asked, read] {
            if (m_fits.value(key).asked != asked)
                return;
            m_fits[key].byPath = read;
            ++m_modelsRevision;
            emit modelsChanged();
        };
    });
}

qint64 WidgetEdit::otherModelsMb(const QString &node, const QString &input) {
    qint64 total = 0;
    const auto &inputs = m_canvas->scopeDocument().graph.nodes.value(node).inputs;
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        if (it.key() == input || it.value().kind() != InputValue::Kind::Resource)
            continue;
        const InputSpec *spec = m_canvas->inputSpec(node, it.key());
        const QString type = spec ? spec->type.type.toString() : QString();
        if (type != kDiffusionModel && type != kTextEncoder && type != kVae)
            continue;
        const Result<QString> found = library().resolve(it.value().handle(), hashes(), ModelLibrary::Hashing::RememberedOnly);
        if (found)
            total += QFileInfo(found.value()).size() / 1000000;
    }
    return total;
}

void WidgetEdit::requestMarks(const QString &node, const QString &input) {
    const QString key = fitsKey(node, input);
    const QString type = target(node, input).spec->type.type.toString();
    Marks &marks = m_marks[key];
    marks.byPath.clear();
    marks.asked = ++m_fitsAsked;
    QList<MarkedFile> files;
    for (const Model &model : m_diskModels.value(modelsKey(node, input)))
        files.append({model.path, model.size});
    if (files.isEmpty())
        return;
    ModelShapes *shapes = m_shapes;
    const int asked = marks.asked;
    const qint64 others = otherModelsMb(node, input);
    m_work.read(this, [this, shapes, key, asked, type, files, others]() -> std::function<void()> {
        const ModelSpeeds speeds;
        const QHash<QString, ModelMark> found = markModels(type, files, RecommendContext{localComputeDevice(), others, &speeds}, *shapes);
        return [this, key, asked, found] {
            if (m_marks.value(key).asked != asked)
                return;
            m_marks[key].byPath = found;
            ++m_modelsRevision;
            emit modelsChanged();
        };
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
    const QHash<QString, ModelMark> marks = m_marks.value(fitsKey(node, input)).byPath;
    const QString type = target(node, input).spec->type.type.toString();
    QVariantList list;
    for (const Model &model : m_models.value(key) + m_diskModels.value(key)) {
        QVariantMap entry = modelRow(model, type, marks);
        if (model.onDisk())
            if (const auto fit = fits.constFind(model.path); fit != fits.constEnd()) {
                entry.insert("fits", fit->fits); // absent when the file's header tells nothing
                if (!fit->why.isEmpty())
                    entry.insert("fitNote", fit->why);
            }
        list.append(entry);
    }
    for (const CatalogModel &model : m_catalogMissing.value(key))
        list.append(catalogRow(model));
    return list;
}

bool WidgetEdit::modelsLoading(const QString &node, const QString &input) const {
    const QString key = modelsKey(node, input);
    return !key.isEmpty() && m_loading.contains(key);
}

QString WidgetEdit::modelsEmptyText(const QString &node, const QString &input) const {
    const Target t = target(node, input);
    return fugue::modelsEmptyText(capabilities(node, input), t.spec ? &t.spec->type.type : nullptr, library());
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

FileHashes &WidgetEdit::hashes() { return m_work.hashes(); }

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

QVariantList WidgetEdit::onDiskOffers(const QString &node) {
    // Asked on every hover of a warning badge: the answer is kept for a moment (until a model input of the node changes, or 2 s).
    const Document level = m_canvas->scopeDocument();
    QJsonObject inputs;
    const auto here = level.graph.nodes.constFind(node);
    if (here != level.graph.nodes.constEnd())
        for (auto input = here->inputs.cbegin(); input != here->inputs.cend(); ++input)
            if (input->kind() == InputValue::Kind::Resource)
                inputs.insert(input.key(), input->handle());
    const QByteArray key = m_canvas->scopePath().toUtf8() + '\0' + node.toUtf8() + '\0' + QJsonDocument(inputs).toJson(QJsonDocument::Compact);
    if (key == m_offersKey && m_offersAge.isValid() && m_offersAge.elapsed() < 2000)
        return m_offers;
    QVariantList offers;
    const auto found = level.graph.nodes.constFind(node);
    if (found == level.graph.nodes.constEnd())
        return offers;
    const ModelLibrary models = library();
    const ModelSpeeds speeds;
    for (auto input = found->inputs.cbegin(); input != found->inputs.cend(); ++input) {
        if (input->kind() != InputValue::Kind::Resource || target(node, input.key()).widget != QLatin1String("model"))
            continue;
        const QJsonObject handle = input->handle();
        if (handle.value(QStringLiteral("hash")).toString().isEmpty() ||
            models.resolve(handle, hashes(), ModelLibrary::Hashing::RememberedOnly))
            continue; // here, or a model a provider serves
        const std::optional<OnDiskMatch> match = findOnDisk(handle, models, RecommendContext{localComputeDevice(), 0, &speeds}, *m_shapes);
        if (!match)
            continue;
        const OnDiskUse use{handle, *match};
        offers.append(QVariantMap{{"input", input.key()},
                                  {"file", match->name},
                                  {"same", use.sameModelText()},
                                  {"path", match->path},
                                  {"tier", match->tier},
                                  {"otherSource", match->otherSource}});
    }
    m_offersKey = key;
    m_offers = offers;
    m_offersAge.restart();
    return offers;
}

void WidgetEdit::setDownloads(ModelDownloads *downloads) {
    m_downloads = downloads;
    if (downloads)
        connect(downloads, &ModelDownloads::finished, this, [this] { downloadsFinished(); });
}

QVariantMap WidgetEdit::catalogInfo(const QString &node, const QString &input, const QString &id) const {
    const Target t = target(node, input);
    const CatalogModel *model = t.widget == QLatin1String("model") ? m_catalog.find(id) : nullptr;
    if (!model || model->resource != t.spec->type.type.toString())
        return {};
    return catalogDetails(*model, library(), t.spec->type.type);
}

QString WidgetEdit::downloadCatalogModel(const QString &node, const QString &input, const QString &id) {
    const Target t = target(node, input);
    if (t.widget != QLatin1String("model"))
        return tr("%1 cannot be edited here.").arg(input);
    if (m_canvas->scopeDocument().graph.nodes.value(node).inputs.value(input).kind() == InputValue::Kind::Link)
        return tr("%1 is linked; take the link out to pick a model.").arg(label(*t.spec));
    const CatalogModel *model = m_catalog.find(id);
    if (!model || model->resource != t.spec->type.type.toString())
        return tr("This model is not in the catalog for this input.");
    if (!m_downloads)
        return tr("Downloads are not available here.");
    const Result<DownloadTarget> planned = planCatalogDownload(library(), *model, t.spec->type.type);
    if (!planned)
        return planned.error().message;
    m_downloading.append(Download{m_canvas->scopePath(), node, input, planned->path, model->resource, model->file,
                                  QStringLiteral("sha256:") + model->sha256});
    m_downloads->add({planned.value()}, QStringLiteral("pick"));
    return {};
}

void WidgetEdit::downloadsFinished() {
    // What came: the file is in place with its hash remembered (a download keeps nothing else).
    // The input gets it as a picked file does, at once when its level is shown, else when it is.
    for (const Download &download : std::as_const(m_downloading)) {
        if (!QFileInfo::exists(download.path) || hashes().known(download.path) != download.hash)
            continue;
        Pick pick;
        pick.scope = download.scope;
        pick.node = download.node;
        pick.input = download.input;
        pick.path = download.path;
        pick.resource = download.resource;
        pick.name = download.name;
        pick.hash = download.hash;
        m_picks.append(pick);
    }
    m_downloading.clear();
    storePicks();
}

void WidgetEdit::startHashing(const QString &path) {
    if (m_hashing.contains(path))
        return;
    m_hashing.insert(path);
    m_work.hash(
        this, path,
        [this, path](int percent) {
            bool moved = false;
            for (Pick &pick : m_picks)
                if (pick.path == path) {
                    pick.progress = percent / 100.0;
                    moved = true;
                }
            if (moved) {
                ++m_hashRevision;
                emit hashingChanged();
            }
        },
        [this, path](const QString &hash) { hashed(path, hash); });
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
