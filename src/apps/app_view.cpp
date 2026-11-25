#include "apps/app_view.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QRandomGenerator>

#include <cmath>

#include "apps/app_models.h"
#include "engine/error.h"
#include "engine/file_roots.h"
#include "engine/input_binding.h"
#include "engine/paths.h"
#include "engine/port_type.h"
#include "engine/registry.h"
#include "workflow/image_inputs.h"
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

constexpr double maxSeed = 9007199254740991.0; // 2^53 - 1 (spec 2.1): what a number can hold exactly

// What the workflow has in the input's first target: a literal, or a model's handle; Undefined for none or for a random seed.
QJsonValue currentOf(const Document &document, const InputBinding &input) {
    if (input.targets.isEmpty())
        return QJsonValue::Undefined;
    const PortRef &target = input.targets.first();
    const auto node = document.graph.nodes.constFind(target.node);
    if (node == document.graph.nodes.constEnd())
        return QJsonValue::Undefined;
    const InputValue value = node->inputs.value(target.port);
    if (value.isEmpty())
        return QJsonValue::Undefined;
    if (value.kind() == InputValue::Kind::Resource)
        return value.toJson();
    return value.kind() == InputValue::Kind::Literal ? value.literal() : QJsonValue(QJsonValue::Undefined);
}

// The first target is a seed left to the engine ({"auto": "random"}).
bool isRandom(const Document &document, const InputBinding &input) {
    if (input.targets.isEmpty())
        return false;
    const PortRef &target = input.targets.first();
    const auto node = document.graph.nodes.constFind(target.node);
    return node != document.graph.nodes.constEnd() && node->inputs.value(target.port).kind() == InputValue::Kind::Auto;
}

// The resource type and capabilities of the node that takes a model input (a group's exposed input: its inner node's).
struct ModelTarget {
    QString type;
    QStringList capabilities;
};
ModelTarget modelTargetOf(const Document &document, const InputBinding &input, const Registry *registry) {
    if (input.targets.isEmpty() || !registry)
        return {};
    PortRef target = input.targets.first();
    auto node = document.graph.nodes.constFind(target.node);
    if (node == document.graph.nodes.constEnd())
        return {};
    QString nodeType = node->type;
    int major = node->version.major;
    if (!registry->nodeType(nodeType)) {
        const GroupDefinition *group = document.group(nodeType);
        if (!group)
            return {};
        for (const GroupInput &exposed : group->inputs)
            if (exposed.name == target.port && !exposed.targets.isEmpty()) {
                const PortRef inner = exposed.targets.first();
                const Node &innerNode = group->nodes.value(inner.node);
                nodeType = innerNode.type;
                major = innerNode.version.major;
                target = inner;
            }
    }
    const NodeType *installed = registry->nodeTypeFor(nodeType, major);
    const InputSpec *spec = installed ? installed->input(target.port) : nullptr;
    if (!spec)
        return {};
    ModelTarget result{spec->type.type.toString(), {}};
    for (const QJsonValue &capability : installed->capabilities.toArray())
        result.capabilities << capability.toString();
    return result;
}

// A fresh seed in the input's range.
double newSeed(const InputBinding &input) {
    const double low = std::max(0.0, input.min.value_or(0)), high = std::min(maxSeed, input.max.value_or(maxSeed));
    return high <= low ? low : low + double(QRandomGenerator::global()->bounded(quint64(high - low) + 1));
}

QString kindOf(const InputBinding &input, const QJsonValue &current, const InputSpec *spec) {
    if (input.role == QLatin1String("image") || input.role == QLatin1String("mask") || input.role == QLatin1String("model")
        || input.role == QLatin1String("seed"))
        return input.role;
    if (input.choices && !input.choices->isEmpty())
        return QStringLiteral("choice");
    if (current.isBool() || (spec && spec->type.type.id == QLatin1String("bool") && !spec->type.list))
        return QStringLiteral("toggle");
    if (input.role == QLatin1String("strength") || input.min || input.max || current.isDouble()
        || (spec && spec->type.type.id == QLatin1String("number") && !spec->type.list))
        return QStringLiteral("number");
    return QStringLiteral("text");
}

} // namespace

AppView::AppView(Start start, Cancel cancel, const Registry *registry, QObject *parent)
    : QObject(parent), m_start(std::move(start)), m_cancel(std::move(cancel)), m_registry(registry) {
    if (registry) {
        m_models = new AppModels(registry, this);
        connect(m_models, &AppModels::changed, this, &AppView::modelsChanged);
        connect(m_models, &AppModels::hashed, this, &AppView::modelHashed);
    }
}

void AppView::setDownloads(ModelDownloads *downloads) {
    if (m_models)
        m_models->setDownloads(downloads);
}

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
    else if (!parsed && !m_told.contains(tab)) {
        m_told.insert(tab);
        const Error &why = parsed.error();
        emit notice(why.code == QLatin1String("unsupported_format_version")
                        ? tr("This workflow's app was made by a newer Fugue, so it opens as a graph.")
                        : tr("This workflow's app block cannot be used, so it opens as a graph: %1").arg(why.message));
    }
    buildFields();
    emit appChanged();
    emit runChanged();
    emit progressChanged();
}

void AppView::forget(const QString &tab) {
    const auto it = m_state.constFind(tab);
    m_told.remove(tab);
    if (it == m_state.constEnd())
        return;
    if (!it->run.isEmpty() && m_cancel)
        m_cancel(it->run);
    m_state.erase(it);
    m_waiting.removeIf([&tab](const auto &waiting) { return waiting.value().tab == tab; });
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

QVariantMap AppView::fieldFor(const InputBinding &given) const {
    // What the block leaves unsaid comes from the target's spec, through a group instance to its inner input (spec 5.4).
    const InputSpec *spec = m_registry && !given.targets.isEmpty() ? targetInputSpec(m_document, *m_registry, given.targets.first()) : nullptr;
    const InputBinding input = completedBy(given, spec);
    const QJsonValue current = currentOf(m_document, input);
    const QString kind = kindOf(input, current, spec);
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
    if (kind == QLatin1String("text"))
        field.insert("multiline", input.role == QLatin1String("prompt") || input.role == QLatin1String("negative_prompt"));
    if (kind == QLatin1String("seed"))
        field.insert("mode", state.seedModes.value(input.key(), isRandom(m_document, input) ? QStringLiteral("random")
                                                                                              : QStringLiteral("fixed")));
    if (kind == QLatin1String("image"))
        field.insert("samples", samplesOf());
    if (kind == QLatin1String("model"))
        field.insert("resource", modelTargetOf(m_document, input, m_registry).type);
    QJsonValue value = state.values.contains(input.key()) ? state.values.value(input.key())
                       : !input.defaultValue.isUndefined() ? input.defaultValue
                                                           : current;
    // The workflow holds no value (a group instance left at its inner input's default): show the spec's default.
    if (value.isUndefined() && spec && !spec->defaultValue.isUndefined()
        && (kind == QLatin1String("number") || kind == QLatin1String("text") || kind == QLatin1String("toggle")))
        value = spec->defaultValue;
    if (kind == QLatin1String("choice")) {
        QVariantList choices;
        if (input.choices) {
            for (const QString &word : *input.choices)
                choices << QVariantMap{{"value", word}, {"text", word}};
            // A choice always has one in force: with no default and no value, the first, kept as the form's value so the
            // run binds what the form shows.
            // The target's own default is the one in force when the workflow holds none.
            const QJsonValue first = spec && input.choices->contains(spec->defaultValue.toString()) ? spec->defaultValue
                                                                                                    : QJsonValue(input.choices->value(0));
            if (!choices.isEmpty() && (!value.isString() || !input.choices->contains(value.toString()))) {
                value = first;
                state.values.insert(input.key(), value);
            }
        }
        field.insert("choices", choices);
    } else if (kind == QLatin1String("toggle")) {
        value = QJsonValue(value.toBool());
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
    if (kind == QLatin1String("number") || kind == QLatin1String("seed")) {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || std::isnan(number) || value.toString().trimmed().isEmpty())
            state.values.remove(key);
        else {
            state.values.insert(key, number);
            if (kind == QLatin1String("seed") && seedMode(key) == QLatin1String("random"))
                state.seedModes.insert(key, QStringLiteral("fixed")); // a number typed is the number to use
        }
    } else {
        state.values.insert(key, QJsonValue::fromVariant(value));
    }
}

QMap<QString, QJsonValue> AppView::values() const { return m_state.value(m_tab).values; }

QVariant AppView::valueOf(const QString &key) const {
    const TabState state = m_state.value(m_tab);
    if (state.values.contains(key))
        return state.values.value(key).toVariant();
    for (const QVariant &field : m_fields)
        if (field.toMap().value("key").toString() == key)
            return field.toMap().value("value");
    return {};
}

const InputBinding *AppView::inputOf(const QString &key) const {
    if (m_app)
        for (const InputBinding &input : std::as_const(m_app->inputs))
            if (input.key() == key)
                return &input;
    return nullptr;
}

QString AppView::kindOfField(const QString &key) const {
    for (const QVariant &field : m_fields)
        if (field.toMap().value("key").toString() == key)
            return field.toMap().value("kind").toString();
    return {};
}

// --- seeds ---------------------------------------------------------------------

QString AppView::seedMode(const QString &key) const {
    if (kindOfField(key) != QLatin1String("seed"))
        return {};
    const QString given = m_state.value(m_tab).seedModes.value(key);
    return !given.isEmpty() ? given : isRandom(m_document, *inputOf(key)) ? QStringLiteral("random") : QStringLiteral("fixed");
}

void AppView::setSeedMode(const QString &key, const QString &mode) {
    if (kindOfField(key) != QLatin1String("seed") || !QStringList{"fixed", "random", "increment"}.contains(mode))
        return;
    TabState &state = m_state[m_tab];
    state.seedModes.insert(key, mode);
    // A number that stays has to be one: the field shows it, the run binds it.
    if (mode != QLatin1String("random") && !valueOf(key).isValid())
        state.values.insert(key, newSeed(*inputOf(key)));
    emit valueChanged(key);
}

void AppView::rollSeed(const QString &key) {
    if (kindOfField(key) != QLatin1String("seed"))
        return;
    m_state[m_tab].values.insert(key, newSeed(*inputOf(key)));
    emit valueChanged(key);
}

// --- images --------------------------------------------------------------------

QString AppView::resolvedPath(const QString &path) const {
    if (path.isEmpty())
        return {};
    const QString expanded = expandHome(path);
    const QFileInfo info(expanded);
    if (!info.isRelative())
        return expanded;
    return m_file.isEmpty() ? QString() : QFileInfo(m_file).absoluteDir().filePath(expanded);
}

QUrl AppView::fileUrl(const QString &path) const {
    const QString resolved = resolvedPath(path);
    return resolved.isEmpty() ? QUrl() : QUrl::fromLocalFile(resolved);
}

// The pictures the template ships in the inputs folder beside the workflow file (spec 11.10): a form offers them to try.
QVariantList AppView::samplesOf() const {
    QVariantList list;
    if (m_file.isEmpty())
        return list;
    static const QStringList suffixes{"png", "jpg", "jpeg", "webp", "bmp", "gif"};
    const QDir folder(inputsDirBeside(m_file));
    for (const QFileInfo &file : folder.entryInfoList(QDir::Files | QDir::Readable, QDir::Name))
        if (suffixes.contains(file.suffix().toLower()) && list.size() < 12)
            list << QVariantMap{{"path", file.absoluteFilePath()}, {"url", QUrl::fromLocalFile(file.absoluteFilePath())},
                                {"name", file.completeBaseName()}};
    return list;
}

QString AppView::storeImages(const QStringList &paths, const QString &key) {
    if (kindOfField(key) != QLatin1String("image") || paths.isEmpty())
        return {};
    const Result<QString> stored = storeImageFile(paths.first(), inputsDir(), FileRoots::forInputs());
    if (!stored)
        return QObject::tr("Could not take the image: %1").arg(stored.error().message);
    m_state[m_tab].values.insert(key, stored.value());
    emit valueChanged(key);
    return {};
}

QString AppView::setImageFiles(const QString &key, const QList<QUrl> &files) {
    QStringList paths;
    for (const QUrl &url : files)
        if (url.isLocalFile())
            paths << url.toLocalFile();
    return paths.isEmpty() ? QObject::tr("Drop a picture file.") : storeImages(paths, key);
}

QString AppView::setImageFromMime(const QString &key, const QMimeData &data) {
    if (kindOfField(key) != QLatin1String("image"))
        return {};
    // Encoded bytes first, then a file manager's copy (the files themselves, which decoded pixels would lose), then the pixels.
    Result<QByteArray> bytes = clipboardImageBytes(data, false);
    if (!bytes) {
        if (const QStringList files = clipboardFiles(data); !files.isEmpty())
            return storeImages(files, key);
        bytes = clipboardImageBytes(data);
    }
    if (!bytes)
        return QObject::tr("Could not paste the image: %1").arg(bytes.error().message);
    const Result<QString> stored = storeImage(bytes.value(), inputsDir(), FileRoots::forInputs());
    if (!stored)
        return QObject::tr("Could not paste the image: %1").arg(stored.error().message);
    m_state[m_tab].values.insert(key, stored.value());
    emit valueChanged(key);
    return {};
}

bool AppView::canPasteImage() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    return clipboard && clipboard->mimeData() && clipboardHasImage(*clipboard->mimeData());
}

QString AppView::pasteImage(const QString &key) {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard || !clipboard->mimeData() || !clipboardHasImage(*clipboard->mimeData()))
        return QObject::tr("The clipboard has no picture.");
    return setImageFromMime(key, *clipboard->mimeData());
}

void AppView::useImage(const QString &key, const QString &path) {
    if (kindOfField(key) != QLatin1String("image"))
        return;
    // Only what the form offered: the inputs folder's files and the template's samples.
    const QString canonical = QFileInfo(path).canonicalFilePath();
    QStringList roots{QFileInfo(inputsDir()).canonicalFilePath()};
    if (!m_file.isEmpty())
        roots << QFileInfo(inputsDirBeside(m_file)).canonicalFilePath();
    const bool offered = !canonical.isEmpty() && std::any_of(roots.cbegin(), roots.cend(), [&](const QString &root) {
                             return !root.isEmpty() && canonical.startsWith(root + QLatin1Char('/'));
                         });
    if (!offered || !QFileInfo(canonical).isFile())
        return;
    m_state[m_tab].values.insert(key, canonical);
    emit valueChanged(key);
}

QVariantList AppView::recentImages() const {
    QVariantList list;
    for (const RecentInput &input : recentInputs(48))
        list << QVariantMap{{"path", input.path}, {"url", QUrl::fromLocalFile(input.path)}, {"name", input.name}};
    return list;
}

QVariantMap AppView::maskPicture() const {
    for (const QVariant &field : m_fields) {
        const QVariantMap map = field.toMap();
        if (map.value("kind").toString() != QLatin1String("image"))
            continue;
        const QUrl url = fileUrl(valueOf(map.value("key").toString()).toString());
        if (!url.isValid())
            return {};
        QImageReader reader(url.toLocalFile());
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (!size.isValid())
            return {};
        return {{"url", url}, {"width", size.width()}, {"height", size.height()}};
    }
    return {};
}

// --- models --------------------------------------------------------------------

void AppView::requestModels(const QString &key) {
    const InputBinding *input = inputOf(key);
    if (!m_models || !input || kindOfField(key) != QLatin1String("model"))
        return;
    const ModelTarget target = modelTargetOf(m_document, *input, m_registry);
    if (!target.type.isEmpty())
        m_models->request(target.type, target.capabilities);
}

QVariantList AppView::models(const QString &key) const {
    const InputBinding *input = inputOf(key);
    if (!m_models || !input)
        return {};
    const ModelTarget target = modelTargetOf(m_document, *input, m_registry);
    return m_models->entries(target.type, target.capabilities);
}

QString AppView::modelsEmptyText(const QString &key) const {
    const InputBinding *input = inputOf(key);
    if (!m_models || !input)
        return {};
    const ModelTarget target = modelTargetOf(m_document, *input, m_registry);
    return m_models->emptyText(target.type, target.capabilities);
}

bool AppView::modelsLoading(const QString &key) const {
    const InputBinding *input = inputOf(key);
    if (!m_models || !input)
        return false;
    const ModelTarget target = modelTargetOf(m_document, *input, m_registry);
    return m_models->loading(target.type, target.capabilities);
}

void AppView::pickModel(const QString &key, const QVariantMap &handle) {
    if (kindOfField(key) != QLatin1String("model"))
        return;
    m_state[m_tab].values.insert(key, QJsonObject::fromVariantMap(handle));
    emit valueChanged(key);
}

void AppView::pickModelFile(const QString &key, const QString &path) {
    const InputBinding *input = inputOf(key);
    if (!m_models || !input || kindOfField(key) != QLatin1String("model"))
        return;
    const QString type = modelTargetOf(m_document, *input, m_registry).type;
    const int ticket = waitFor(key);
    emit modelsChanged();
    m_models->pick(ticket, type, path);
}

int AppView::waitFor(const QString &key) {
    const int ticket = ++m_tickets;
    m_waiting.insert(ticket, Waiting{m_tab, key});
    return ticket;
}

QStringList AppView::waitingModels() const {
    QStringList keys;
    for (const Waiting &waiting : m_waiting)
        if (waiting.tab == m_tab && !keys.contains(waiting.key))
            keys << waiting.key;
    return keys;
}

QVariantMap AppView::catalogInfo(const QString &key, const QString &id) const {
    const InputBinding *input = inputOf(key);
    return m_models && input ? m_models->catalogInfo(modelTargetOf(m_document, *input, m_registry).type, id) : QVariantMap();
}

QString AppView::downloadModel(const QString &key, const QString &id) {
    const InputBinding *input = inputOf(key);
    if (!m_models || !input || kindOfField(key) != QLatin1String("model"))
        return {};
    const QString type = modelTargetOf(m_document, *input, m_registry).type;
    const int ticket = waitFor(key);
    const QString why = m_models->download(ticket, type, id);
    if (!why.isEmpty())
        m_waiting.remove(ticket);
    emit modelsChanged();
    return why;
}

void AppView::modelHashed(int ticket, const QVariantMap &handle) {
    const auto it = m_waiting.constFind(ticket);
    if (it == m_waiting.constEnd())
        return; // its tab was closed
    const Waiting waiting = *it;
    m_waiting.erase(it);
    if (!handle.isEmpty()) {
        if (waiting.tab == m_tab)
            pickModel(waiting.key, handle);
        else if (const auto state = m_state.find(waiting.tab); state != m_state.end())
            state->values.insert(waiting.key, QJsonObject::fromVariantMap(handle)); // shown when that tab is
    }
    emit modelsChanged();
}

QString AppView::run() {
    if (!canRun())
        return {};
    TabState &state = m_state[m_tab];
    // A seed left to chance is the engine's own (as a node's is); the others are what the form shows.
    QMap<QString, QJsonValue> given = state.values;
    QStringList incrementing;
    for (const QVariant &field : std::as_const(m_fields)) {
        const QVariantMap map = field.toMap();
        const QString key = map.value("key").toString();
        if (map.value("kind").toString() != QLatin1String("seed"))
            continue;
        const QString mode = seedMode(key);
        if (mode == QLatin1String("random"))
            given.insert(key, QJsonObject{{"auto", "random"}});
        else if (mode == QLatin1String("increment"))
            incrementing << key;
    }
    const Result<Document> bound = app::bindApp(m_document, *m_app, given);
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
    // The next run's seed is the next number: the run used the one shown (as Increment on a node).
    for (const QString &key : std::as_const(incrementing)) {
        const InputBinding *input = inputOf(key);
        const QVariant now = valueOf(key);
        if (!input || !now.isValid())
            continue;
        double next = now.toDouble() + 1;
        if (input->max && next > *input->max)
            next = input->min.value_or(0);
        state.values.insert(key, next);
        emit valueChanged(key);
    }
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
