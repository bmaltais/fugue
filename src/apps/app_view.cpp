#include "apps/app_view.h"

#include <QJsonArray>
#include <QUrl>

#include <cmath>

#include "engine/error.h"
#include "engine/input_binding.h"
#include "engine/registry.h"
#include "run/run_service.h"

namespace fugue {

namespace {

// "negative_prompt" -> "Negative prompt": what an input with no label is called (no role words reach the form as they are).
QString humanize(const QString &word) {
    QString text = word;
    text.replace(QLatin1Char('_'), QLatin1Char(' ')).replace(QLatin1Char('-'), QLatin1Char(' '));
    text = text.trimmed();
    return text.isEmpty() ? text : text.at(0).toUpper() + text.mid(1);
}

QJsonValue currentOf(const Document &document, const InputBinding &input) {
    if (input.targets.isEmpty())
        return QJsonValue::Undefined;
    const PortRef &target = input.targets.first();
    const auto node = document.graph.nodes.constFind(target.node);
    if (node == document.graph.nodes.constEnd())
        return QJsonValue::Undefined;
    const InputValue value = node->inputs.value(target.port);
    return !value.isEmpty() && value.kind() == InputValue::Kind::Literal ? value.literal() : QJsonValue(QJsonValue::Undefined);
}

QString kindOf(const InputBinding &input, const QJsonValue &current) {
    if (input.role == QLatin1String("image") || input.role == QLatin1String("mask"))
        return input.role;
    if (input.choices && !input.choices->isEmpty())
        return QStringLiteral("choice");
    if (current.isBool())
        return QStringLiteral("choice"); // a switch: Yes and No
    if (input.role == QLatin1String("seed") || input.role == QLatin1String("strength") || input.min || input.max
        || current.isDouble())
        return QStringLiteral("number");
    return QStringLiteral("text");
}

} // namespace

AppView::AppView(Start start, Cancel cancel, const Registry *registry, QObject *parent)
    : QObject(parent), m_start(std::move(start)), m_cancel(std::move(cancel)), m_registry(registry) {}

void AppView::show(const QString &tab, const Document &document, const QString &file) {
    m_tab = tab;
    m_file = file;
    m_document = document;
    m_app.reset();
    m_problem.clear();
    const Result<std::optional<app::App>> parsed = app::appOf(document);
    if (parsed && parsed->has_value()) {
        m_app = **parsed;
        if (m_registry) {
            app::pruneApp(*m_app, m_document, *m_registry);
            const Errors errors = app::checkApp(*m_app, m_document, *m_registry);
            if (!errors.isEmpty())
                m_problem = errors.first().message;
        }
        TabState &state = m_state[tab];
        state.outputs = m_app->outputs;
        if (!state.seen) {
            state.seen = true;
            state.graph = m_app->defaultView == QLatin1String("graph");
        }
    }
    buildFields();
    emit appChanged();
    emit runChanged();
    emit progressChanged();
}

void AppView::forget(const QString &tab) {
    const auto it = m_state.constFind(tab);
    if (it == m_state.constEnd())
        return;
    if (!it->run.isEmpty() && m_cancel)
        m_cancel(it->run);
    m_state.erase(it);
    if (tab == m_tab) {
        m_tab.clear();
        m_app.reset();
        m_fields.clear();
        emit appChanged();
        emit runChanged();
    }
}

QString AppView::status() const { return m_state.value(m_tab).status; }

bool AppView::busy() const { return !m_state.value(m_tab).run.isEmpty(); }

void AppView::buildFields() {
    m_fields.clear();
    if (!m_app)
        return;
    TabState &state = m_state[m_tab];
    // A value kept for an input the app no longer has goes.
    QStringList keys;
    for (const InputBinding &input : m_app->inputs)
        keys << input.key();
    for (auto it = state.values.begin(); it != state.values.end();)
        it = keys.contains(it.key()) ? std::next(it) : state.values.erase(it);
    for (const InputBinding &input : m_app->inputs)
        m_fields << fieldFor(input);
}

QVariantMap AppView::fieldFor(const InputBinding &input) const {
    const QJsonValue current = currentOf(m_document, input);
    const QString kind = kindOf(input, current);
    TabState &state = const_cast<AppView *>(this)->m_state[m_tab];
    QVariantMap field{{"key", input.key()},
                      {"label", input.label.isEmpty() ? humanize(input.key()) : input.label},
                      {"kind", kind},
                      {"section", input.section},
                      {"advanced", input.section == QLatin1String("Advanced")}};
    if (input.min)
        field.insert("min", *input.min);
    if (input.max)
        field.insert("max", *input.max);
    QJsonValue value = state.values.contains(input.key()) ? state.values.value(input.key())
                       : !input.defaultValue.isUndefined() ? input.defaultValue
                                                           : current;
    if (kind == QLatin1String("choice")) {
        QVariantList choices;
        if (input.choices) {
            for (const QString &word : *input.choices)
                choices << QVariantMap{{"value", word}, {"text", word}};
            // A choice always has one in force: with no default and no value, the first, kept as the form's value so the
            // run binds what the form shows.
            if (!choices.isEmpty() && (!value.isString() || !input.choices->contains(value.toString()))) {
                value = QJsonValue(input.choices->first());
                state.values.insert(input.key(), value);
            }
        } else {
            choices << QVariantMap{{"value", false}, {"text", QObject::tr("No")}}
                    << QVariantMap{{"value", true}, {"text", QObject::tr("Yes")}};
            value = QJsonValue(value.toBool());
        }
        field.insert("choices", choices);
    }
    field.insert("value", value.isUndefined() ? QVariant() : value.toVariant());
    return field;
}

void AppView::setValue(const QString &key, const QVariant &value) {
    if (!m_app)
        return;
    TabState &state = m_state[m_tab];
    QString kind;
    for (const QVariant &field : std::as_const(m_fields))
        if (field.toMap().value("key").toString() == key)
            kind = field.toMap().value("kind").toString();
    if (kind.isEmpty())
        return;
    if (kind == QLatin1String("number")) {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || std::isnan(number) || value.toString().trimmed().isEmpty())
            state.values.remove(key);
        else
            state.values.insert(key, number);
    } else {
        state.values.insert(key, QJsonValue::fromVariant(value));
    }
}

QMap<QString, QJsonValue> AppView::values() const { return m_state.value(m_tab).values; }

QString AppView::run() {
    if (!canRun())
        return {};
    TabState &state = m_state[m_tab];
    const Result<Document> bound = app::bindApp(m_document, *m_app, state.values);
    if (!bound) {
        state.status = QStringLiteral("failed");
        state.error = bound.errors().first().message;
        state.details.clear();
        addDetail(state, bound.errors().first().message, bound.errors().first().hint);
        emit runChanged();
        return {};
    }
    RunRequest request;
    request.document = bound.value();
    request.workflow = m_file;
    const QString id = m_start(request);
    state.run = id;
    state.status = QStringLiteral("waiting");
    state.progress = -1;
    state.error.clear();
    state.details.clear();
    state.results.clear();
    emit runChanged();
    emit progressChanged();
    return id;
}

void AppView::cancel() {
    const QString run = m_state.value(m_tab).run;
    if (!run.isEmpty() && m_cancel)
        m_cancel(run);
}

void AppView::openInGraph() {
    if (!m_app || m_state[m_tab].graph)
        return;
    m_state[m_tab].graph = true;
    emit appChanged();
}

void AppView::showApp() {
    if (!m_app || !m_state[m_tab].graph)
        return;
    emit documentWanted(); // edits made in the graph, and a block changed there, are in the form
    if (!m_app)
        return; // the block was taken out in the graph: it is no app now
    m_state[m_tab].graph = false;
    emit appChanged();
}

QString AppView::tabOfRun(const QString &run) const {
    for (auto it = m_state.cbegin(); it != m_state.cend(); ++it)
        if (it->run == run)
            return it.key();
    return {};
}

void AppView::addDetail(TabState &state, const QString &message, const QString &hint) {
    for (const QVariant &known : std::as_const(state.details))
        if (known.toMap().value("message").toString() == message)
            return;
    state.details << QVariantMap{{"message", message}, {"hint", hint}};
}

void AppView::addResult(TabState &state, const QJsonObject &event) {
    const QString node = event.value("node").toString();
    int index = -1;
    QString label;
    QStringList order;
    for (int i = 0; i < state.outputs.size(); ++i) {
        order << state.outputs.at(i).node;
        if (state.outputs.at(i).node == node) {
            index = i;
            label = state.outputs.at(i).label;
        }
    }
    if (index < 0)
        return;
    if (label.isEmpty())
        label = index == 0 ? QObject::tr("Result") : QObject::tr("Result %1").arg(index + 1);
    const QJsonObject thumbnails = event.value("thumbnails").toObject();
    const QJsonObject thumbnail = thumbnails.contains("image")  ? thumbnails.value("image").toObject()
                                  : thumbnails.contains("file") ? thumbnails.value("file").toObject()
                                  : thumbnails.isEmpty()        ? QJsonObject()
                                                                : thumbnails.begin().value().toObject();
    QVariantMap result{{"id", node}, {"label", label}, {"width", thumbnail.value("width").toInt()},
                       {"height", thumbnail.value("height").toInt()}, {"url", QString()}, {"full", QString()}, {"text", QString()}};
    const QString type = thumbnail.value("type").toString();
    const QString media = thumbnail.value("mediaPath").toString();
    if ((type == QLatin1String("video") || type == QLatin1String("audio")) && !media.isEmpty()) {
        const QString url = QUrl::fromLocalFile(media).toString();
        result.insert("kind", type);
        result.insert("url", url);
        result.insert("full", url);
        result.insert("text", thumbnail.value("label").toString());
    } else if (!thumbnail.value("url").toString().isEmpty()) {
        const QString source = thumbnail.value("source").toString();
        result.insert("kind", QStringLiteral("image"));
        result.insert("url", thumbnail.value("url").toString());
        result.insert("full", source.isEmpty() ? thumbnail.value("url").toString() : QUrl::fromLocalFile(source).toString());
    } else {
        result.insert("kind", QStringLiteral("text"));
        result.insert("text", thumbnail.value("label").toString());
    }
    // In the block's order, one per output.
    QVariantList &list = state.results;
    for (int i = 0; i < list.size(); ++i)
        if (list.at(i).toMap().value("id") == node) {
            list[i] = result;
            return;
        }
    int at = 0;
    while (at < list.size() && order.indexOf(list.at(at).toMap().value("id").toString()) < index)
        ++at;
    list.insert(at, result);
}

void AppView::end(const QString &tab, const QString &status) {
    TabState &state = m_state[tab];
    state.run.clear();
    state.progress = -1;
    state.status = status == QLatin1String("ok") ? QStringLiteral("done")
                   : status == QLatin1String("cancelled") ? QStringLiteral("cancelled")
                                                          : QStringLiteral("failed");
    if (state.status == QLatin1String("failed"))
        state.error = state.details.isEmpty() ? QObject::tr("The run did not finish.")
                                              : state.details.first().toMap().value("message").toString();
    if (tab == m_tab) {
        emit runChanged();
        emit progressChanged();
    }
}

void AppView::onEvent(const QString &run, const QJsonObject &event) {
    const QString tab = tabOfRun(run);
    if (tab.isEmpty())
        return;
    TabState &state = m_state[tab];
    const QString kind = event.value("event").toString();
    const bool current = tab == m_tab;
    if (kind == QLatin1String("run.started")) {
        state.status = QStringLiteral("running");
        if (current)
            emit runChanged();
    } else if (kind == QLatin1String("node.running")) {
        state.progress = -1;
        if (current)
            emit progressChanged();
    } else if (kind == QLatin1String("node.progress")) {
        state.progress = std::clamp(event.value("value").toDouble(), 0.0, 1.0);
        if (current)
            emit progressChanged();
    } else if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")) {
        // The app's own outputs are the app's results; the other nodes' pictures are the graph's.
        addResult(state, event);
        if (current)
            emit runChanged();
    } else if (kind == QLatin1String("node.error")) {
        const Error error = Error::fromJson(event.value("error").toObject());
        addDetail(state, error.message, error.hint);
        if (current)
            emit runChanged();
    } else if (kind == QLatin1String("run.done")) {
        for (const QJsonValue &value : event.value("errors").toArray()) {
            const Error error = Error::fromJson(value.toObject());
            if (error.severity != Error::Severity::Warning)
                addDetail(state, error.message, error.hint);
        }
        end(tab, event.value("status").toString());
    }
}

} // namespace fugue
