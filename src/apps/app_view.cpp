#include "apps/app_view.h"

#include "engine/templates.h"

#include <QPointer>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

#include "apps/app_models.h"
#include "apps/app_takes.h"
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

// A seed's value is a number; a missing one, or the engine's own ({"auto": "random"}, which a block's default may be), is none.
bool hasSeedNumber(const QVariant &value) {
    bool ok = false;
    return value.isValid() && value.typeId() != QMetaType::QVariantMap && !value.toString().isEmpty()
           && (value.toDouble(&ok), ok);
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
    m_liveTimer.setSingleShot(true);
    m_liveTimer.setInterval(600);
    connect(&m_liveTimer, &QTimer::timeout, this, &AppView::liveFire);
    m_needs = new NeedsModel(this);
    // A new app, or a run that ended, may have found something missing: the panel looks again (the source says "ready" at once
    // when nothing is).
    connect(this, &AppView::appChanged, m_needs, &NeedsModel::refreshSoon);
    connect(this, &AppView::appChanged, this, &AppView::shownChanged); // what the tab shows follows the app and its mode
    connect(this, &AppView::runChanged, m_needs, [this] { if (status() == QLatin1String("failed")) m_needs->refreshSoon(); });
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

void AppView::askPlacement(bool modelsAreElsewhere) {
    if (!modelsAreElsewhere || !m_app || !m_placement) {
        m_placementTab.clear();
        setRunsOn({});
        return;
    }
    if (m_placementTab == m_tab)
        return; // asked for this tab already
    m_placementTab = m_tab;
    const QString tab = m_tab;
    const QPointer<AppView> self(this);
    m_placement([self, tab](const QString &host) {
        if (self && self->m_tab == tab && self->m_placementTab == tab)
            self->setRunsOn(host);
    });
}

void AppView::setRunsOn(const QString &host) {
    if (host == m_runsOn)
        return;
    const QString before = runsOn();
    m_runsOn = host;
    if (runsOn() != before)
        emit runsOnChanged();
}

void AppView::setOffMachine(const QString &services) {
    if (services == m_offMachine)
        return;
    const QString before = runsOn();
    m_offMachine = services;
    if (runsOn() != before)
        emit runsOnChanged();
}

void AppView::show(const QString &tab, const Document &document, const QString &file, const QString &templateFolder) {
    if (tab != m_tab) { // another tab: where it runs is asked again
        m_placementTab.clear();
        setRunsOn({});
    }
    m_tab = tab;
    m_file = file;
    m_takesKey = !file.isEmpty() ? file : templateFolder.isEmpty() ? QString() : QDir(templateFolder).filePath("workflow.fugue");
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
    // A debounce waiting for another tab is that tab's: it is owed again when the tab comes back.
    m_liveTimer.stop();
    if (m_app) {
        TabState &state = m_state[tab];
        if (state.storedFor != m_takesKey)
            loadStored(state);
        if (state.live != QLatin1String("off") && state.liveDirty)
            scheduleLive(tab);
    }
    emit appChanged();
    emit runChanged();
    emit progressChanged();
    emit liveChanged();
}

void AppView::setHistory(const QString &runsRoot, Replay replay) {
    m_runsRoot = runsRoot;
    m_replay = std::move(replay);
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
    m_replaying.removeIf([&tab](const auto &replaying) { return replaying.value() == tab; });
    if (tab == m_tab) {
        m_liveTimer.stop();
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
    // An optional picture or mask starts empty, whatever file its node holds: left empty it takes its branch out of the run (#484).
    const bool optionalPicture = input.optional && (input.role == QLatin1String("image") || input.role == QLatin1String("mask"));
    const QJsonValue current = optionalPicture ? QJsonValue(QJsonValue::Undefined) : currentOf(m_document, input);
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
    if (optionalPicture) {
        field.insert("optional", true);
        field.insert("hint", tr("Optional"));
    }
    if (kind == QLatin1String("text"))
        field.insert("multiline", input.role == QLatin1String("prompt") || input.role == QLatin1String("negative_prompt"));
    if (kind == QLatin1String("seed"))
        field.insert("mode", state.seedModes.value(input.key(), isRandom(m_document, input) ? QStringLiteral("random")
                                                                                              : QStringLiteral("fixed")));
    if (kind == QLatin1String("image")) {
        // The template's own sample first: the block's default, else what the workflow holds, when it is a file there.
        QVariantList samples = samplesOf();
        const QJsonValue own = !input.defaultValue.isUndefined() ? input.defaultValue : current;
        const QString sample = own.isString() ? QFileInfo(resolvedPath(own.toString())).canonicalFilePath() : QString();
        if (!sample.isEmpty()) {
            // The template's own sample leads the list, once.
            for (int i = 0; i < samples.size(); ++i)
                if (QFileInfo(samples.at(i).toMap().value("path").toString()).canonicalFilePath() == sample) {
                    samples.removeAt(i);
                    break;
                }
            samples.prepend(QVariantMap{{"path", sample}, {"url", QUrl::fromLocalFile(sample)},
                                        {"name", QFileInfo(sample).completeBaseName()}});
        }
        field.insert("samples", samples);
        field.insert("sample", sample);
    }
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
    if (kind == QLatin1String("seed")) {
        // A seed shows a number, or none while it is Random. Fixed and Increment always have one (the workflow's, else a new
        // one kept in the form), so what is shown is what every run uses.
        if (!hasSeedNumber(value.toVariant()))
            value = QJsonValue::Undefined;
        if (value.isUndefined() && field.value("mode").toString() != QLatin1String("random")) {
            value = newSeed(input);
            state.values.insert(input.key(), value);
        }
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
    const QMap<QString, QJsonValue> before = state.values;
    const QMap<QString, QString> modesBefore = state.seedModes;
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
    if (state.values != before || state.seedModes != modesBefore)
        inputEdited();
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
    if (mode != QLatin1String("random") && !hasSeedNumber(valueOf(key)))
        state.values.insert(key, newSeed(*inputOf(key)));
    emit valueChanged(key);
    inputEdited();
    // Repeat that waited for a seed to vary goes on now.
    if (state.live == QLatin1String("repeat") && !busy() && repeatVaries()) {
        state.liveDirty = true;
        liveFire();
    }
    emit liveChanged(); // repeatWaits
}

void AppView::rollSeed(const QString &key) {
    if (kindOfField(key) != QLatin1String("seed"))
        return;
    m_state[m_tab].values.insert(key, newSeed(*inputOf(key)));
    emit valueChanged(key);
    inputEdited();
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

QUrl AppView::preview() const {
    if (m_file.isEmpty())
        return {};
    const QString still = previewFilesIn(QFileInfo(m_file).absolutePath()).still;
    return still.isEmpty() ? QUrl() : QUrl::fromLocalFile(still);
}

QUrl AppView::previewVideo() const {
    if (m_file.isEmpty())
        return {};
    const QString video = previewFilesIn(QFileInfo(m_file).absolutePath()).video;
    return video.isEmpty() ? QUrl() : QUrl::fromLocalFile(video);
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
    inputEdited();
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

QString AppView::imageTarget() const {
    QString first;
    for (const QVariant &field : m_fields) {
        const QVariantMap map = field.toMap();
        if (map.value("kind").toString() != QLatin1String("image"))
            continue;
        if (map.value("key").toString() == m_focusedImage)
            return m_focusedImage;
        if (first.isEmpty())
            first = map.value("key").toString();
    }
    return first;
}

void AppView::setFocusedImage(const QString &key, bool focused) {
    if (focused)
        m_focusedImage = key;
    else if (m_focusedImage == key)
        m_focusedImage.clear();
}

bool AppView::pasteToApp() {
    const QString key = imageTarget();
    if (!shown() || key.isEmpty() || !canPasteImage())
        return false;
    emit imageNotice(key, pasteImage(key));
    return true;
}

QString AppView::dropImageFiles(const QList<QUrl> &files) {
    const QString key = imageTarget();
    if (key.isEmpty())
        return {};
    const QString why = setImageFiles(key, files);
    emit imageNotice(key, why);
    return why;
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
    inputEdited();
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
    inputEdited();
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
    QMap<QString, QJsonValue> drawn; // the seeds the form drew for this run
    for (const QVariant &field : std::as_const(m_fields)) {
        const QVariantMap map = field.toMap();
        const QString key = map.value("key").toString();
        if (map.value("kind").toString() != QLatin1String("seed"))
            continue;
        const QString mode = seedMode(key);
        if (mode == QLatin1String("random")) {
            // The form draws the seed (as the engine would, in the input's range) and binds it as a number: the take then
            // knows the number the picture came from, and Run again with it makes the same picture (#389). The form's own
            // mode stays Random for the next run.
            const InputBinding *input = inputOf(key);
            const double seed = input ? newSeed(*input) : 0;
            given.insert(key, seed);
            drawn.insert(key, seed);
        } else if (mode == QLatin1String("increment"))
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
    QMap<QString, QJsonValue> shownValues = state.values;
    QMap<QString, QString> shownModes = state.seedModes;
    QJsonObject valuesJson, modesJson;
    for (auto it = drawn.cbegin(); it != drawn.cend(); ++it) {
        shownValues.insert(it.key(), it.value());
        shownModes.insert(it.key(), QStringLiteral("fixed"));
    }
    for (auto it = shownValues.cbegin(); it != shownValues.cend(); ++it)
        valuesJson.insert(it.key(), it.value());
    for (auto it = shownModes.cbegin(); it != shownModes.cend(); ++it)
        modesJson.insert(it.key(), it.value());
    RunRequest request;
    request.document = bound.value();
    // The form as it was run, in the run's own folder: what makes the run a take after a restart (apps/app_takes.h).
    if (!m_takesKey.isEmpty())
        request.document.extra.insert(QLatin1String(app::takeKey), app::takeRecord(m_takesKey, valuesJson, modesJson));
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
    state.liveFrame.clear();
    state.pinned.clear(); // a new run is the one to look at
    state.cleared = false;
    state.takes.prepend({id, state.status, {}, shownValues, shownModes, inputsLine(shownValues, shownModes)});
    while (state.takes.size() > maxTakes)
        state.takes.removeLast();
    emit runChanged();
    emit progressChanged();
    return id;
}

bool AppView::canClear() const {
    const TabState state = m_state.value(m_tab);
    return m_app && state.run.isEmpty() && (state.status != QLatin1String("idle") || !state.takes.isEmpty());
}

bool AppView::canRepeat() const {
    for (const QVariant &field : std::as_const(m_fields))
        if (field.toMap().value("kind").toString() == QLatin1String("seed"))
            return true;
    return false;
}

bool AppView::repeatVaries() const {
    for (const QVariant &field : std::as_const(m_fields)) {
        const QVariantMap map = field.toMap();
        if (map.value("kind").toString() != QLatin1String("seed"))
            continue;
        const QString mode = seedMode(map.value("key").toString());
        if (mode == QLatin1String("random") || mode == QLatin1String("increment"))
            return true;
    }
    return false;
}

bool AppView::repeatWaits() const {
    return m_app && liveMode() == QLatin1String("repeat") && !busy() && !repeatVaries();
}

void AppView::clearResults() {
    if (!canClear())
        return;
    TabState &state = m_state[m_tab];
    state.status = QStringLiteral("idle");
    state.progress = -1;
    state.error.clear();
    state.details.clear();
    state.results.clear();
    state.cleared = true;
    state.pinned.clear();
    // The strip too (owner, 2026-11-25); the run folders stay. The newest run cleared is recorded, so a restart does not bring the
    // older ones back.
    QString newest;
    for (const Take &take : std::as_const(state.takes))
        newest = std::max(newest, take.run);
    state.takes.clear();
    app::setClearedThrough(m_runsRoot, m_takesKey, newest);
    emit runChanged();
    emit progressChanged();
}

void AppView::cancel() {
    // The person said stop: Live and Repeat end with it.
    if (m_state.value(m_tab).live != QLatin1String("off"))
        setLiveMode(QStringLiteral("off"));
    const QString run = m_state.value(m_tab).run;
    if (!run.isEmpty() && m_cancel)
        m_cancel(run);
}

void AppView::setAreas(bool split, bool appActive) {
    if (m_split == split && m_areaApp == appActive)
        return;
    const bool was = shown();
    m_split = split;
    m_areaApp = appActive;
    if (was != shown())
        emit shownChanged();
}

void AppView::openInGraph() {
    if (m_app && m_split) { // the graph is on screen already: bring it to the area that shows the App view
        emit areaViewRequested(false);
        return;
    }
    if (!m_app || m_state[m_tab].graph)
        return;
    m_state[m_tab].graph = true;
    emit appChanged();
}

void AppView::toggle() {
    if (m_app && m_split)
        emit areaToggleRequested();
    else if (shown())
        openInGraph();
    else
        showApp();
}

void AppView::showApp() {
    if (m_app && m_split) { // an area shows it, in the active one or the next
        emit areaViewRequested(true);
        return;
    }
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

void AppView::addResult(const QList<app::AppOutput> &outputs, QVariantList &list, const QJsonObject &event) {
    const QString node = event.value("node").toString();
    int index = -1;
    QString label;
    QStringList order;
    for (int i = 0; i < outputs.size(); ++i) {
        order << outputs.at(i).node;
        if (outputs.at(i).node == node) {
            index = i;
            label = outputs.at(i).label;
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
    const QString finished = state.run;
    state.run.clear();
    state.progress = -1;
    state.liveFrame.clear();
    state.status = status == QLatin1String("ok") ? QStringLiteral("done")
                   : status == QLatin1String("cancelled") ? QStringLiteral("cancelled")
                                                          : QStringLiteral("failed");
    if (state.status == QLatin1String("failed"))
        state.error = state.details.isEmpty() ? QObject::tr("The run did not finish.")
                                              : state.details.first().toMap().value("message").toString();
    syncTake(state);
    // A run cancelled before it made anything leaves no take: nothing of it to look at or reuse.
    if (state.status == QLatin1String("cancelled") && !state.takes.isEmpty() && state.takes.first().run == finished
        && state.takes.first().results.isEmpty()) {
        state.takes.removeFirst();
        if (state.pinned == finished)
            state.pinned.clear();
    }
    if (tab == m_tab) {
        emit runChanged();
        emit progressChanged();
    }
    // Repeat: the next run when this one made something and the next would differ (a seed varies: a tab not shown is checked when
    // it is); an edit made while it ran (Live) is owed the same way.
    if (state.live == QLatin1String("repeat") && state.status == QLatin1String("done") && (tab != m_tab || repeatVaries()))
        state.liveDirty = true;
    if (tab == m_tab)
        emit liveChanged(); // repeatWaits
    if (state.liveDirty && state.live != QLatin1String("off"))
        scheduleLive(tab);
}

void AppView::onEvent(const QString &run, const QJsonObject &event) {
    const auto replaying = m_replaying.constFind(run);
    if (replaying != m_replaying.constEnd()) {
        replayEvent(replaying.value(), run, event);
        return;
    }
    const QString tab = tabOfRun(run);
    if (tab.isEmpty())
        return;
    TabState &state = m_state[tab];
    const QString kind = event.value("event").toString();
    const bool current = tab == m_tab;
    if (kind == QLatin1String("run.started")) {
        state.status = QStringLiteral("running");
        syncTake(state);
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
        addResult(state.outputs, state.results, event);
        syncTake(state);
        if (current)
            emit runChanged();
    } else if (kind == QLatin1String("node.preview")) {
        // A step picture of a node at work (a Sampler's): the stage shows the newest while the run goes.
        const QString url = event.value("thumbnails").toObject().value("preview").toObject().value("url").toString();
        if (!url.isEmpty()) {
            state.liveFrame = url;
            if (current)
                emit progressChanged();
        }
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

namespace fugue {

// --- takes (#389) ---------------------------------------------------------------

namespace {
QString fileNameOf(const QString &path) { return QFileInfo(path).fileName(); }
} // namespace

QString AppView::inputsLine(const QMap<QString, QJsonValue> &values, const QMap<QString, QString> &modes) const {
    QStringList parts;
    for (const QVariant &entry : m_fields) {
        const QVariantMap field = entry.toMap();
        const QString key = field.value("key").toString(), kind = field.value("kind").toString(), label = field.value("label").toString();
        if (kind == QLatin1String("mask") || kind == QLatin1String("model"))
            continue;
        if (kind == QLatin1String("seed")) {
            // The number the run used: the form's (a take's own), else the one the workflow holds; a seed left to chance has none.
            const QVariant number = values.contains(key) ? values.value(key).toVariant() : field.value("value");
            parts << (modes.value(key) == QLatin1String("random") || !number.isValid() || number.isNull()
                          ? tr("seed new each run")
                          : tr("seed %1").arg(qint64(number.toDouble())));
            continue;
        }
        if (!values.contains(key))
            continue;
        const QJsonValue value = values.value(key);
        if (kind == QLatin1String("image")) {
            parts << fileNameOf(value.toString());
        } else if (kind == QLatin1String("toggle")) {
            parts << (value.toBool() ? label : tr("no %1").arg(label.toLower()));
        } else if (value.isDouble()) {
            parts << QStringLiteral("%1 %2").arg(label).arg(value.toDouble());
        } else {
            QString text = value.toString().simplified();
            if (text.size() > 40)
                text = text.left(39) + QStringLiteral("…");
            if (!text.isEmpty())
                parts << (kind == QLatin1String("text") ? QStringLiteral("“%1”").arg(text) : text);
        }
    }
    QString line = parts.join(QStringLiteral(" · "));
    if (line.size() > 160)
        line = line.left(159) + QStringLiteral("…");
    return line;
}

void AppView::syncTake(TabState &state) {
    if (state.takes.isEmpty())
        return;
    Take &take = state.takes.first();
    take.results = state.results;
    take.status = state.status;
}

int AppView::takeIndex() const {
    const QList<Take> &takes = m_state.value(m_tab).takes;
    const QString pinned = m_state.value(m_tab).pinned;
    if (m_state.value(m_tab).cleared)
        return -1;
    for (int i = 0; i < takes.size(); ++i)
        if (!pinned.isEmpty() && takes.at(i).run == pinned)
            return i;
    for (int i = 0; i < takes.size(); ++i)
        if (!takes.at(i).results.isEmpty())
            return i;
    return takes.isEmpty() ? -1 : 0;
}

QVariantList AppView::takes() const {
    QVariantList list;
    const TabState state = m_state.value(m_tab);
    const int shown = takeIndex();
    for (int i = 0; i < state.takes.size(); ++i) {
        const Take &take = state.takes.at(i);
        QString url;
        for (const QVariant &result : take.results)
            if (result.toMap().value("kind").toString() == QLatin1String("image")) {
                url = result.toMap().value("url").toString();
                break;
            }
        list << QVariantMap{{"run", take.run}, {"status", take.status},
                            {"pending", take.status == QLatin1String("waiting") || take.status == QLatin1String("running")},
                            {"url", url}, {"count", take.results.size()}, {"line", take.line}, {"shown", i == shown},
                            {"persisted", take.persisted}};
    }
    return list;
}

QVariantList AppView::shownResults() const {
    const TabState state = m_state.value(m_tab);
    const int index = takeIndex();
    return index < 0 ? QVariantList() : state.takes.at(index).results;
}

QString AppView::shownInputs() const {
    const TabState state = m_state.value(m_tab);
    const int index = takeIndex();
    return index < 0 ? QString() : state.takes.at(index).line;
}

void AppView::selectTake(int index) {
    TabState &state = m_state[m_tab];
    if (index < 0 || index >= state.takes.size())
        return;
    state.pinned = index == 0 ? QString() : state.takes.at(index).run;
    state.cleared = false;
    requestTake(state.takes[index]);
    emit runChanged();
}

void AppView::ensureTake(int index) {
    TabState &state = m_state[m_tab];
    if (index >= 0 && index < state.takes.size())
        requestTake(state.takes[index]);
}

// A take read back from History: its results are replayed from its run folder, once.
void AppView::requestTake(Take &take) {
    if (!take.persisted || take.requested || !m_replay)
        return;
    take.requested = true;
    m_replaying.insert(take.run, m_tab);
    m_replay(take.run);
}

void AppView::loadStored(TabState &state) {
    state.storedFor = m_takesKey;
    if (m_runsRoot.isEmpty() || m_takesKey.isEmpty())
        return;
    QSet<QString> have;
    for (const Take &take : std::as_const(state.takes))
        have.insert(take.run);
    for (const app::StoredTake &stored :
         app::storedTakes(m_runsRoot, m_takesKey, maxTakes, 400, app::clearedThrough(m_runsRoot, m_takesKey))) {
        if (have.contains(stored.run) || state.takes.size() >= maxTakes)
            continue;
        Take take;
        take.run = stored.run;
        take.status = stored.status == QLatin1String("ok") ? QStringLiteral("done") : QStringLiteral("failed");
        for (auto it = stored.values.begin(); it != stored.values.end(); ++it)
            take.values.insert(it.key(), it.value());
        for (auto it = stored.modes.begin(); it != stored.modes.end(); ++it)
            take.seedModes.insert(it.key(), it.value().toString());
        take.line = inputsLine(take.values, take.seedModes);
        take.persisted = true;
        state.takes.append(take);
    }
    // The stage shows the newest take that made something: its results are asked for now, the others when the strip draws them.
    for (Take &take : state.takes)
        if (take.persisted && take.status == QLatin1String("done")) {
            requestTake(take);
            break;
        }
}

void AppView::replayEvent(const QString &tab, const QString &run, const QJsonObject &event) {
    TabState &state = m_state[tab];
    const QString kind = event.value("event").toString();
    for (Take &take : state.takes) {
        if (take.run != run)
            continue;
        if (kind == QLatin1String("node.done") || kind == QLatin1String("node.cached"))
            addResult(state.outputs, take.results, event);
        break;
    }
    if (kind == QLatin1String("run.done"))
        m_replaying.remove(run);
    if (tab == m_tab && (kind == QLatin1String("run.done") || kind == QLatin1String("node.done") || kind == QLatin1String("node.cached")))
        emit runChanged();
}

// --- Live (#471) ------------------------------------------------------------------

void AppView::setLiveMode(const QString &mode) {
    if (!m_app || !QStringList{"off", "live", "repeat"}.contains(mode) || (mode == QLatin1String("repeat") && !canRepeat()))
        return;
    TabState &state = m_state[m_tab];
    if (state.live == mode)
        return;
    state.live = mode;
    state.liveDirty = false;
    m_liveTimer.stop();
    emit liveChanged();
    if (mode == QLatin1String("repeat") && !busy()) {
        state.liveDirty = true;
        liveFire(); // starts at once when nothing goes; else the end of the run chains
    }
}

void AppView::inputEdited() {
    if (!m_app)
        return;
    TabState &state = m_state[m_tab];
    if (state.live != QLatin1String("live"))
        return;
    state.liveDirty = true;
    m_liveTimer.start(); // each change restarts the wait: one run once the form has been still
}

void AppView::scheduleLive(const QString &tab) {
    // After the event that ended a run, and only for the tab shown (a tab not shown is owed its run until it is).
    if (tab != m_tab)
        return;
    QTimer::singleShot(0, this, [this, tab] {
        if (tab == m_tab)
            liveFire();
    });
}

void AppView::liveFire() {
    if (!m_app)
        return;
    TabState &state = m_state[m_tab];
    if (state.live == QLatin1String("off") || !state.liveDirty || !m_problem.isEmpty())
        return;
    if (state.status == QLatin1String("running"))
        return; // never cut a run that works: end() starts the owed one
    if (state.status == QLatin1String("waiting") && !state.run.isEmpty()) {
        // Superseded before it began: it leaves the queue and its take goes, as a cancelled run's does.
        const QString old = state.run;
        if (m_cancel)
            m_cancel(old);
        state.run.clear();
        state.status = QStringLiteral("idle");
        for (int i = 0; i < state.takes.size(); ++i)
            if (state.takes.at(i).run == old) {
                state.takes.removeAt(i);
                break;
            }
        if (state.pinned == old)
            state.pinned.clear();
    }
    state.liveDirty = false;
    run();
}

void AppView::reuseInputs(int index) {
    TabState &state = m_state[m_tab];
    if (!m_app || index < 0 || index >= state.takes.size())
        return;
    const Take &take = state.takes.at(index);
    state.values = take.values;
    state.seedModes = take.seedModes;
    for (const QVariant &field : std::as_const(m_fields))
        emit valueChanged(field.toMap().value("key").toString());
}

QString AppView::runAgain(int index) {
    reuseInputs(index);
    return run();
}

QString AppView::saveResult(const QString &fileUrl, const QUrl &destination) {
    const QString from = QUrl(fileUrl).toLocalFile();
    if (from.isEmpty() || !QFileInfo(from).isFile() || !destination.isLocalFile())
        return tr("There is no file to save.");
    const QString to = destination.toLocalFile();
    if (QFileInfo(to) == QFileInfo(from))
        return {};
    if (QFile::exists(to) && !QFile::remove(to))
        return tr("Could not replace %1.").arg(fileNameOf(to));
    return QFile::copy(from, to) ? QString() : tr("Could not save %1.").arg(fileNameOf(to));
}

QString AppView::imageKey() const {
    for (const QVariant &field : m_fields)
        if (field.toMap().value("kind").toString() == QLatin1String("image"))
            return field.toMap().value("key").toString();
    return {};
}

QString AppView::useResultAsInput(const QString &key, const QString &fileUrl) {
    const QString path = QUrl(fileUrl).toLocalFile();
    return path.isEmpty() ? tr("Only a picture can be used as an input.") : storeImages({path}, key);
}

QVariantList AppView::resultPictures() const {
    QVariantList list;
    const QList<Take> &takes = m_state.value(m_tab).takes;
    for (int i = 0; i < takes.size(); ++i)
        for (const QVariant &entry : takes.at(i).results) {
            const QVariantMap result = entry.toMap();
            const QString path = QUrl(result.value("full").toString()).toLocalFile();
            if (result.value("kind").toString() != QLatin1String("image") || path.isEmpty() || !QFileInfo::exists(path))
                continue;
            list << QVariantMap{{"path", path}, {"url", result.value("url")},
                                {"name", tr("%1 · %2").arg(fileNameOf(path)).arg(takes.size() - i)}};
        }
    return list;
}

void AppView::useSample(const QString &key) {
    for (const QVariant &entry : std::as_const(m_fields)) {
        const QVariantMap field = entry.toMap();
        if (field.value("key").toString() != key || field.value("kind").toString() != QLatin1String("image"))
            continue;
        const QString sample = field.value("sample").toString();
        if (sample.isEmpty())
            return;
        m_state[m_tab].values.insert(key, sample);
        emit valueChanged(key);
        inputEdited();
        return;
    }
}

} // namespace fugue
