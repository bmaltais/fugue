#include "canvas/widget_edit.h"

#include "canvas/canvas_model.h"
#include "canvas/input_links.h"
#include "engine/providers.h"
#include "engine/registry.h"

#include <QJsonArray>
#include <QPointer>
#include <QRandomGenerator>
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

// Asks each enabled provider instance to describe itself and lists the models it offers of
// the resource type. The providers live until every reply is in.
WidgetEdit::ModelSource providerModels() {
    return [](const QString &type, std::function<void(QList<WidgetEdit::Model>)> done) {
        const Result<QList<ProviderInstance>> instances = loadProviderInstances(providersPath());
        if (!instances.ok())
            return done({});
        struct Pending {
            int left = 0;
            QList<WidgetEdit::Model> models;
            std::function<void(QList<WidgetEdit::Model>)> done;
            std::vector<std::shared_ptr<Provider>> providers;
        };
        auto pending = std::make_shared<Pending>();
        pending->done = std::move(done);
        QList<std::pair<std::shared_ptr<Provider>, QString>> asked;
        for (const ProviderInstance &instance : instances.value()) {
            if (!instance.enabled)
                continue;
            Result<std::shared_ptr<Provider>> made = createProvider(instance);
            if (!made.ok())
                continue;
            pending->providers.push_back(made.value());
            asked.append({made.value(), instance.name});
        }
        pending->left = int(asked.size());
        if (asked.isEmpty())
            return pending->done({});
        for (const auto &[provider, name] : asked) {
            provider->describe([pending, type, name = name](const Result<Description> &description) {
                if (description.ok()) {
                    for (const CapabilityOffer &offer : description->capabilities) {
                        for (const OfferedModel &model : offer.models) {
                            if (model.handle.value("resource").toString() != type)
                                continue;
                            const bool listed = std::any_of(
                                pending->models.begin(), pending->models.end(), [&](const WidgetEdit::Model &m) {
                                    return m.provider == name && sameModel(m.handle, model.handle);
                                });
                            if (!listed)
                                pending->models.append({model.handle.value("name").toString(), model.handle,
                                                        modelStatusName(model.status), name});
                        }
                    }
                }
                if (--pending->left > 0)
                    return;
                pending->done(pending->models);
                // Not from inside a provider's own reply.
                QTimer::singleShot(0, [pending] { pending->providers.clear(); });
            });
        }
    };
}

} // namespace

WidgetEdit::WidgetEdit(const Registry *registry, CanvasModel *canvas, QObject *parent)
    : QObject(parent), m_registry(registry), m_canvas(canvas) {
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
        // A comma is the decimal separator in many languages; there is no grouping here.
        if (number.contains(',') && !number.contains('.'))
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

void WidgetEdit::write(const QString &node, const QString &input, const QJsonValue &value, const QString &text) {
    Document document = m_canvas->document();
    if (!document.graph.nodes.contains(node))
        return;
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
    Document document = m_canvas->document();
    Node &n = document.graph.nodes[node];
    if (n.inputs.value(input).kind() == InputValue::Kind::Link)
        return;
    if (mode == QLatin1String("random"))
        n.inputs.insert(input, InputValue::autoRandom());
    else if (n.inputs.value(input).kind() == InputValue::Kind::Auto)
        n.inputs.insert(input, InputValue::literal(randomSeed(*t.spec)));
    setIncrementing(document, node, input, mode == QLatin1String("increment"));
    m_canvas->commit(document, stepText(node, input));
}

void WidgetEdit::advanceSeeds() {
    Document document = m_canvas->document();
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

void WidgetEdit::requestModels(const QString &node, const QString &input) {
    const Target t = target(node, input);
    if (t.widget != QLatin1String("model"))
        return;
    const QString type = t.spec->type.type.toString();
    if (!m_source)
        m_source = providerModels();
    m_loading.insert(type);
    ++m_modelsRevision;
    emit modelsChanged();
    QPointer<WidgetEdit> self(this);
    m_source(type, [self, type](QList<Model> models) {
        if (!self)
            return;
        self->m_models.insert(type, std::move(models));
        self->m_loading.remove(type);
        ++self->m_modelsRevision;
        emit self->modelsChanged();
    });
}

QVariantList WidgetEdit::models(const QString &node, const QString &input) const {
    const Target t = target(node, input);
    if (t.widget != QLatin1String("model"))
        return {};
    QVariantList list;
    for (const Model &model : m_models.value(t.spec->type.type.toString()))
        list.append(QVariantMap{{"name", model.name},
                                {"handle", model.handle.toVariantMap()},
                                {"status", model.status},
                                {"provider", model.provider}});
    return list;
}

bool WidgetEdit::modelsLoading(const QString &node, const QString &input) const {
    const Target t = target(node, input);
    return t.widget == QLatin1String("model") && m_loading.contains(t.spec->type.type.toString());
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
