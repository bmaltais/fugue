#include "workflow/workflows.h"

#include "workflow/tab_session.h"
#include "canvas/widget_edit.h"

#include "cli/process.h"
#include "engine/atomic_file.h"
#include "engine/canvas_package.h"
#include "engine/compile_for_run.h"
#include "engine/compiler.h"
#include "engine/embed.h"
#include "engine/file_inputs.h"
#include "engine/file_roots.h"
#include "engine/group_library.h"
#include "engine/ids.h"
#include "engine/file_hashes.h"
#include "engine/json.h"
#include "engine/model_catalog.h"
#include "engine/model_download.h"
#include "engine/lazy_reach.h"
#include "engine/model_library.h"
#include "engine/model_recommend.h"
#include "engine/model_speed.h"
#include "engine/model_use.h"
#include "engine/model_places.h"
#include "engine/template_search.h"
#include "engine/fugue_home.h"
#include "engine/paths.h"
#include "engine/settings.h"
#include "engine/app_binding.h"
#include "engine/share_package.h"
#include "workflow/content_hash.h"
#include "workflow/workflow_io.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QGuiApplication>
#include <QClipboard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <functional>

namespace fugue {

namespace {

QString appVersion() {
    const QString version = QCoreApplication::applicationVersion();
    return version.isEmpty() ? QStringLiteral("0.1.0") : version;
}

QString newTabId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

// Every node type the document uses that is not a group and not installed.
QStringList missingTypes(const Document &document, const Registry &registry) {
    QSet<QString> missing;
    const auto scan = [&](const Nodes &nodes) {
        for (const Node &node : nodes)
            if (!document.groups.contains(node.type) && !registry.nodeType(node.type))
                missing.insert(node.type);
    };
    scan(document.graph.nodes);
    for (const GroupDefinition &group : document.groups)
        scan(group.nodes);
    QStringList list(missing.begin(), missing.end());
    list.sort();
    return list;
}

// Writes the workflow file atomically. Without `replace`, false and nothing written when a
// file is already at `path`.
Result<bool> writeWorkflow(const Document &document, const QString &path, bool replace) {
    if (replace) {
        const Errors errors = document.saveFile(path);
        return hasErrors(errors) ? Result<bool>(errors) : Result<bool>(true);
    }
    const QFileInfo info(path);
    PartialFile partial(info.absolutePath(), info.fileName());
    if (const Errors errors = partial.write(writeJson(document.toJson()), info.fileName()); hasErrors(errors))
        return errors;
    return partial.commit(path, Overwrite::Never);
}

// The button for the models of `missing` that have a source: "Download 3 models, 19.4 GB", the
// size when every one records it. Empty for none.
QString offerText(const QList<MissingModel> &missing) {
    int count = 0;
    qint64 bytes = 0;
    bool sized = true;
    for (const MissingModel &model : missing) {
        if (model.source.isEmpty())
            continue;
        ++count;
        sized = sized && model.size >= 0;
        bytes += std::max<qint64>(0, model.size);
    }
    if (count == 0)
        return {};
    return sized ? Workflows::tr("Download %n models, %1", nullptr, count).arg(ModelDownloads::sizeText(bytes))
                 : Workflows::tr("Download %n models", nullptr, count);
}

// The bytes of the models of `missing` that have a source, as the card's button says them;
// empty unless every one records its size.
QString downloadSize(const QList<MissingModel> &missing) {
    qint64 bytes = 0;
    int count = 0;
    for (const MissingModel &model : missing) {
        if (model.source.isEmpty())
            continue;
        if (model.size < 0)
            return {};
        bytes += model.size;
        ++count;
    }
    return count ? ModelDownloads::sizeText(bytes) : QString();
}

// The missing models that no file of the person's stands in for: what a download fetches, and what a card's Download counts.
QList<MissingModel> uncoveredModels(const QList<MissingModel> &absent, const ModelLibrary &library, const RecommendContext &context) {
    QList<MissingModel> out;
    for (const MissingModel &model : absent)
        if (!findOnDisk(model.handle, library, context, ModelShapes::shared()))
            out.append(model);
    return out;
}

// The rows of a template's details view: each model it uses, with its type ("diffusion model"), size and license
// (read from the catalog by file name or hash when it lists the file), and where it is: "disk", "served" (by a
// paired host) or "missing".
QVariantList modelRows(const QJsonArray &models, const QList<ModelPlaces> &placed, const ModelCatalog &catalog) {
    QVariantList rows;
    for (qsizetype i = 0; i < models.size(); ++i) {
        const QJsonObject handle = models.at(i).toObject();
        const QString name = handle.value(QStringLiteral("name")).toString();
        const QString hash = handle.value(QStringLiteral("hash")).toString().section(QLatin1Char(':'), -1);
        const CatalogModel *found = nullptr;
        for (const CatalogModel &entry : catalog.all())
            if (entry.file == name || (!hash.isEmpty() && entry.sha256 == hash)) {
                found = &entry;
                break;
            }
        const ModelPlaces place = placed.value(i);
        QString kind = handle.value(QStringLiteral("resource")).toString().section(QLatin1Char('/'), -1).replace(QLatin1Char('-'), QLatin1Char(' '));
        if (!kind.isEmpty())
            kind[0] = kind.at(0).toUpper();
        const qint64 size = handle.contains(QStringLiteral("size")) ? qint64(handle.value(QStringLiteral("size")).toDouble())
                                                                    : found ? found->size : -1;
        rows.append(QVariantMap{{"name", name},
                                {"kind", kind},
                                {"size", size > 0 ? ModelDownloads::sizeText(size) : QString()},
                                {"license", found ? licenseLine(*found) : QString()},
                                {"status", place.local ? QStringLiteral("disk") : !place.hosts.isEmpty() ? QStringLiteral("served")
                                                                                                         : QStringLiteral("missing")}});
    }
    return rows;
}

// A model folder as the banner's choice names it.
QString rootTitle(const QString &root, const ModelLibrary &library) {
    return ReadinessSheet::rootTitle(root, library.roots().value(0));
}

// Each message with its hint, for the banner.
QString withHints(const Errors &errors) {
    QStringList lines;
    for (const Error &error : errors)
        lines << (error.hint.isEmpty() ? error.message : error.message + QLatin1Char(' ') + error.hint);
    return lines.join(QLatin1Char(' '));
}

QString firstMessage(const Errors &errors) {
    for (const Error &error : errors)
        if (error.severity == Error::Severity::Error)
            return error.message;
    return errors.isEmpty() ? QString() : errors.first().message;
}

} // namespace

Workflows::Workflows(CanvasModel *canvas, const Registry *registry, CommandRegistry *commands,
                     WorkflowLocations locations, QObject *parent)
    : QObject(parent), m_canvas(canvas), m_registry(registry), m_commands(commands),
      m_templateRoots(locations.templateRoots), m_recovery(locations.recoveryDir), m_trust(locations.trustPath),
      m_registerProviders(locations.registerProviders ? locations.registerProviders : cli::registerProviders),
      m_tabColorsPath(locations.tabColorsPath), m_sessionPath(locations.sessionPath) {
    if (QFile file(m_tabColorsPath); file.open(QIODevice::ReadOnly)) {
        const QJsonObject stored = QJsonDocument::fromJson(file.readAll()).object();
        for (auto it = stored.begin(); it != stored.end(); ++it)
            if (const int color = it.value().toInt(-1); color >= 0 && color < kTabColors)
                m_tabColors.insert(it.key(), color);
    }
    connect(m_canvas, &CanvasModel::edited, this, &Workflows::refreshUnsaved);
    connect(m_canvas, &CanvasModel::pasted, this, [this](const Document &document) {
        if (m_current >= 0)
            analyze(document, m_tabs[size_t(m_current)].title);
    });
    m_images = new ImageInputs(m_canvas, m_registry, this);
    m_downloads = new ModelDownloads(this);
    m_graphDestination = std::make_unique<GraphDestination>(*m_canvas, [this] { return m_current < 0 ? QString() : m_tabs[size_t(m_current)].id; });
    ChoiceHost host;
    host.facts = [this] { return choiceFacts(); };
    host.autoOn = [this] { return m_registry && autoUseOn(); };
    host.modelsKey = [this] { return QJsonDocument(modelsToHave(shownDocument(), *m_registry)).toJson(QJsonDocument::Compact); };
    host.covered = [this] { return onDiskNow(); };
    host.hash = [this](const QString &path, std::function<void(int)> progress, std::function<void(const QString &)> done) {
        m_work.hash(this, path, std::move(progress), std::move(done));
    };
    host.say = [this](const QString &text) { setNotice(text); };
    host.sayChoice = [this](const SwapNotice &swaps) { notifySwaps(swaps); };
    host.saying = [this] { return m_notice; };
    host.sayInSheet = [this](const QString &text) { m_readiness->say(text); };
    host.changed = [this](bool force) {
        m_onDiskKey.clear();
        refreshMissing(force);
    };
    host.endReading = [this] {
        setNotice({});
        setNotice(m_missingNotice);
    };
    host.offerSheet = [this] { offerReadiness(); };
    host.later = [this](std::function<void()> what) { after(std::move(what)); };
    m_choice = std::make_unique<ModelChoice>(std::move(host), *m_graphDestination, this);
    connect(m_choice.get(), &ModelChoice::applied, this, &Workflows::usesApplied);
    connect(m_choice.get(), &ModelChoice::finished, this, &Workflows::usesFinished);
    connect(m_choice.get(), &ModelChoice::readingChanged, this, &Workflows::readingChanged);
    connect(m_choice.get(), &ModelChoice::hashingChanged, this, &Workflows::hashingUsesChanged);
    connect(m_choice.get(), &ModelChoice::recordChanged, this, &Workflows::noticeChanged);
    m_readiness = new ReadinessSheet(m_registry, locations.trustPath,
                                     [this] {
                                         return m_current < 0 ? ReadinessRequest()
                                                              : requestFor(shownDocument(), *m_registry, m_tabs[size_t(m_current)].carried,
                                                                        m_tabs[size_t(m_current)].path.isEmpty() ? QString() : QFileInfo(m_tabs[size_t(m_current)].path).absolutePath());
                                     },
                                     this);
    // What was installed, trusted or downloaded changes what the banner lacks (the registry reload does the same for packs).
    connect(m_readiness, &ReadinessSheet::finished, this, [this] { refreshMissing(false); });
    // Use what you have (#605): a row of the sheet offers the file on disk beside its download.
    m_readiness->setUseSource([this](const QString &hash) { return onDiskOffer(hash); });
    connect(m_readiness, &ReadinessSheet::useRequested, this, [this](const QStringList &hashes, int token) { useOnDisk(hashes, token); });
    connect(m_readiness, &ReadinessSheet::useCancelled, this, [this](int token) { cancelUses(token); });
    connect(this, &Workflows::usesApplied, m_readiness, &ReadinessSheet::refresh);
    connect(this, &Workflows::readingChanged, m_readiness, [this] { m_readiness->setReading(m_choice->reading()); });
    connect(this, &Workflows::usesFinished, m_readiness, &ReadinessSheet::usesFinished);
    // A swap's banner belongs to the tab it is about (docs/ui-ergonomics.md 2026-11-27): another tab shown puts it aside, with its
    // Undo, and shows that tab's own line; back on its tab it comes back, unless another line took its place meanwhile.
    connect(this, &Workflows::currentChanged, this, [this] {
        const QString shown = m_current < 0 ? QString() : m_tabs[size_t(m_current)].id;
        if (!m_noticeLines.isEmpty() && m_noticeTab != shown) {
            if (!m_noticeTab.isEmpty()) // empty: its tab was closed, so nothing is kept
                m_tabSwaps.insert(m_noticeTab, SwapNotice{m_noticeTitle, m_noticeLines, m_notice});
            showMissing();
        }
        if (const auto it = m_tabSwaps.constFind(shown); it != m_tabSwaps.constEnd() && (m_notice.isEmpty() || m_notice == m_missingNotice)) {
            const SwapNotice swaps = *it;
            m_tabSwaps.erase(it);
            setNotice(swaps.text, swaps.title, swaps.lines);
            m_noticeTab = shown;
        }
        if (m_choice->hasRecord())
            emit noticeChanged(); // the banner's Undo belongs to the tab it was opened in
    });
    connect(this, &Workflows::tabClosed, this, [this](const QString &id) {
        m_tabSwaps.remove(id);
        // The shown tab closing: its banner is not kept for it (tabClosed comes before currentChanged would store it).
        if (id == m_noticeTab)
            m_noticeTab.clear();
    });
    connect(m_canvas, &CanvasModel::edited, this, [this] {
        if (m_choice->hasRecord())
            emit noticeChanged(); // an edit ends the banner's Undo
    });
    // What waited for a modal Get ready sheet to close (a template opened with the person's files).
    connect(m_readiness, &ReadinessSheet::changed, this, [this] {
        while (!m_readiness->isOpen() && !m_afterSheet.isEmpty())
            m_afterSheet.takeFirst()();
    });
    // One click: the sheet is for the tab that was shown when it opened; once everything is ready it has closed itself and that
    // tab is the one shown, ready to run, whichever tab the person went to meanwhile.
    connect(m_readiness, &ReadinessSheet::shown, this, [this] { m_readyTabId = m_current < 0 ? QString() : m_tabs[size_t(m_current)].id; });
    connect(m_readiness, &ReadinessSheet::succeeded, this, [this] {
        for (int i = 0; i < count(); ++i)
            if (m_tabs[size_t(i)].id == m_readyTabId) {
                setCurrent(i);
                break;
            }
        refreshMissing(false);
    });
    m_canvas->widgets()->setDownloads(m_downloads); // the model picker's catalog downloads share the line (#134)
    // The banner says what arrived, or what went wrong; what is still missing comes back.
    connect(m_downloads, &ModelDownloads::finished, this, [this](int arrived, const Errors &all) {
        refreshMissing(false);
        // What could not be planned, then what failed; a cancel is the person's own, not news.
        Errors errors = m_refused;
        m_refused.clear();
        for (const Error &error : all)
            if (error.code != QLatin1String("cancelled"))
                errors << error;
        if (!errors.isEmpty())
            setNotice(withHints(errors));
        else if (arrived > 0)
            setNotice(m_missingNotice.isEmpty() ? tr("Downloaded %n models.", nullptr, arrived) : m_missingNotice);
        emit noticeChanged(); // the offer, with the same line
    });
    connect(m_images, &ImageInputs::message, this, [this](const QString &text) { setNotice(text); });
    m_timer.setInterval(locations.recoveryIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &Workflows::writeRecoveryCopies);
    m_timer.start();
    m_sessionTimer.setSingleShot(true);
    m_sessionTimer.setInterval(locations.sessionDelayMs);
    connect(&m_sessionTimer, &QTimer::timeout, this, &Workflows::flushSession);
    connect(this, &Workflows::tabsChanged, this, &Workflows::scheduleSession);
    connect(this, &Workflows::currentChanged, this, &Workflows::scheduleSession);
    if (m_commands)
        connect(m_commands, &CommandRegistry::closeWorkflowRequested, this, &Workflows::closeCurrent);
    for (const RecoveryEntry &entry : m_recovery.list())
        m_recoverable.append(QVariantMap{{"id", entry.id},
                                         {"title", entry.title},
                                         {"path", entry.originalPath},
                                         {"modified", entry.modified}});
    newWorkflow();
}

// --- state for the window ---

QVariantList Workflows::tabs() const {
    QVariantList list;
    for (int i = 0; i < count(); ++i) {
        const Tab &tab = m_tabs[size_t(i)];
        list.append(QVariantMap{{"id", tab.id},
                                {"title", tab.title},
                                {"path", tab.path},
                                {"template", tab.templateFolder}, // where an unnamed tab of a template came from (#398)
                                {"package", tab.packagePath},     // the package an unnamed tab was opened from (#394)
                                {"unsaved", unsaved(i)},
                                {"current", i == m_current},
                                {"color", tab.color}});
    }
    return list;
}

bool Workflows::unsaved(int index) const {
    return index >= 0 && index < count() && m_tabs[size_t(index)].unsaved;
}

bool Workflows::unsaved() const {
    for (int i = 0; i < count(); ++i)
        if (unsaved(i))
            return true;
    return false;
}

QString Workflows::pathOf(int index) const {
    return index >= 0 && index < count() ? m_tabs[size_t(index)].path : QString();
}

QString Workflows::windowTitle() const {
    const QString project = m_projectName.isEmpty() ? QString() : QStringLiteral(" — ") + m_projectName;
    if (m_current < 0)
        return QStringLiteral("Fugue");
    return QStringLiteral("%1%2%3 — Fugue").arg(unsaved(m_current) ? QStringLiteral("• ") : QString(),
                                                m_tabs[size_t(m_current)].title, project);
}

void Workflows::setProjectName(const QString &name) {
    if (m_projectName == name)
        return;
    m_projectName = name;
    emit tabsChanged();
}

QUrl Workflows::suggestedFolder() const {
    if (!fugueHome().isEmpty())
        return QUrl::fromLocalFile(nearestExistingFolder(documentsDir()));
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString fugue = documents + "/Fugue";
    return QUrl::fromLocalFile(QFileInfo(fugue).isDir() ? fugue : (documents.isEmpty() ? QDir::homePath() : documents));
}

void Workflows::setNotice(const QString &text, const QString &title, const QVariantList &lines) {
    if (m_notice == text && m_noticeTitle == title && m_noticeLines == lines)
        return;
    m_notice = text;
    m_noticeTitle = text.isEmpty() ? QString() : title;
    m_noticeLines = text.isEmpty() ? QVariantList() : lines;
    if (m_noticeLines.isEmpty())
        m_noticeTab.clear();
    emit noticeChanged();
}

void Workflows::showMissing() {
    if (m_current < 0)
        return setNotice({});
    m_missingNotice = missingNotice(shownDocument(), m_tabs[size_t(m_current)].title, &m_offer);
    setNotice(m_missingNotice);
}

void Workflows::dismissNotice() { setNotice({}); }

QString Workflows::toPath(const QString &pathOrUrl) {
    if (pathOrUrl.startsWith(QStringLiteral("file:")))
        return QUrl(pathOrUrl).toLocalFile();
    return expandHome(pathOrUrl);
}

QString Workflows::untitledName() const {
    const QString base = tr("Untitled");
    for (int n = 1;; ++n) {
        const QString name = n == 1 ? base : QStringLiteral("%1 %2").arg(base).arg(n);
        bool taken = false;
        for (const Tab &tab : m_tabs)
            taken |= tab.path.isEmpty() && tab.title == name;
        if (!taken)
            return name;
    }
}

// --- tabs ---

Document Workflows::documentOf(int index) const {
    return index == m_current ? m_canvas->document() : m_tabs[size_t(index)].document;
}

Document Workflows::shownDocument() const {
    if (m_current < 0)
        return {};
    const Document document = documentOf(m_current);
    return withUses(document, m_choice->inForce(m_tabs[size_t(m_current)].id));
}

const QList<OnDiskUse> &Workflows::onDiskNow() const {
    if (m_current < 0 || !m_registry) {
        m_onDisk.clear();
        return m_onDisk;
    }
    const QJsonArray wanted = modelsToHave(shownDocument(), *m_registry);
    const QByteArray key = m_tabs[size_t(m_current)].id.toUtf8() + QJsonDocument(wanted).toJson(QJsonDocument::Compact);
    if (key == m_onDiskKey && m_onDiskAge.isValid() && m_onDiskAge.elapsed() < 2000)
        return m_onDisk;
    FileHashes hashes(FileHashes::defaultPath());
    const ModelLibrary library = ModelLibrary::current(m_registry);
    const ModelSpeeds speeds;
    m_onDisk = onDiskUses(library.missing(wanted, hashes), library, RecommendContext{localComputeDevice(), 0, &speeds}, ModelShapes::shared());
    m_onDiskKey = key;
    m_onDiskAge.restart();
    return m_onDisk;
}

QVariantMap Workflows::onDiskOffer(const QString &hash) const {
    for (const OnDiskUse &use : onDiskNow())
        if (use.wanted.value(QStringLiteral("hash")).toString() == hash)
            return {{"same", use.sameModelText()},
                    {"file", use.match.name},
                    {"path", use.match.path},
                    {"tier", use.match.tier},
                    {"size", double(use.match.size)},
                    {"otherSource", use.match.otherSource},
                    {"recommended", use.match.recommended}};
    return {};
}

void Workflows::useOnDisk(const QStringList &hashes, int token) { m_choice->ask(hashes, token); }

void Workflows::cancelUses(int token) { m_choice->cancel(token); }

void Workflows::usesChanged() {
    m_onDiskKey.clear();
    refreshMissing(false);
}

bool Workflows::autoUseOn() const { return currentSettings().useOnDisk == QLatin1String("use"); }

void Workflows::setAppDestination(ChoiceDestination *app) { m_choice->setApp(app); }

ChoiceFacts Workflows::choiceFacts() const {
    ChoiceFacts facts;
    if (m_current >= 0)
        facts.shownTab = m_tabs[size_t(m_current)].id;
    for (const Tab &tab : m_tabs)
        facts.openTabs << tab.id;
    facts.sheetOpen = m_readiness->isOpen();
    facts.sheetReview = facts.sheetOpen && m_readiness->phase() == QLatin1String("review");
    return facts;
}

void Workflows::beginOpen(const QString &key) { m_choice->openAsked(key); }

QList<OnDiskUse> Workflows::coveringUses(const Template &chosen, bool partial) const {
    if (!partial && !chosen.neededPacks.isEmpty())
        return {}; // the Get ready sheet has more to ask than the files
    FileHashes hashes(FileHashes::defaultPath());
    ModelLibrary library = ModelLibrary::current(m_registry);
    const QList<MissingModel> absent = library.missing(chosen.models, hashes);
    if (absent.isEmpty())
        return {}; // nothing missing: an ordinary open
    const ModelSpeeds speeds;
    const QList<OnDiskUse> uses = onDiskUses(absent, library, RecommendContext{localComputeDevice(), 0, &speeds}, ModelShapes::shared());
    return partial || uses.size() == absent.size() ? uses : QList<OnDiskUse>();
}

bool Workflows::openTemplateWithWhatYouHave(const QString &id) {
    const std::optional<Template> chosen = TemplateLibrary(m_templateRoots, m_registry).find(id);
    if (!chosen)
        return false;
    FileHashes hashes(FileHashes::defaultPath());
    const ModelLibrary library = ModelLibrary::current(m_registry);
    const ModelSpeeds speeds;
    const QList<OnDiskUse> uses = onDiskUses(library.missing(chosen->models, hashes), library, RecommendContext{localComputeDevice(), 0, &speeds},
                                             ModelShapes::shared());
    if (uses.isEmpty())
        return false;
    beginOpen(QStringLiteral("open:") + chosen->id);
    return openWithUses(*chosen, uses, {});
}

bool Workflows::openWithUses(const Template &chosen, const QList<OnDiskUse> &uses, std::function<void(bool)> opened) {
    return m_choice->openWith(
        QStringLiteral("open:") + chosen.id, uses,
        [this, chosen](const Uses &resolved) {
            m_openUses = resolved;
            const bool done = openTemplate(chosen);
            m_openUses.clear();
            return done;
        },
        chosen.workflow, std::move(opened));
}

void Workflows::after(std::function<void()> what) {
    if (m_readiness->isOpen())
        m_afterSheet.append(std::move(what));
    else
        what();
}

bool Workflows::noticeUndoable() const { return m_choice->undoable(); }

void Workflows::undoNotice() { m_choice->undo(); }

bool Workflows::openTemplateForDownload(const QString &id) {
    const std::optional<Template> chosen = TemplateLibrary(m_templateRoots, m_registry).find(id);
    if (!chosen)
        return false;
    // The Get ready sheet opens by itself when something is missing; every model with a file of its own is set to Download. The person
    // asked to download: "Use my file" leaves this tab alone (they can still choose their file on the sheet).
    bool opened = false;
    {
        const ModelChoice::Opening guard(*m_choice);
        opened = openTemplate(*chosen);
    }
    if (!opened)
        return false;
    if (m_current >= 0) {
        QStringList hashes;
        for (const OnDiskUse &use : onDiskNow())
            hashes << use.wanted.value(QStringLiteral("hash")).toString();
        m_choice->declined(m_tabs[size_t(m_current)].id, hashes); // the choice covers both views
    }
    if (m_readiness->isOpen())
        m_readiness->chooseAll(false);
    return true;
}

void Workflows::snapshotCurrent() {
    if (m_current < 0)
        return;
    refreshUnsaved();
    m_tabs[size_t(m_current)].document = m_canvas->document();
}

void Workflows::refreshUnsaved() {
    if (m_current < 0)
        return;
    Tab &tab = m_tabs[size_t(m_current)];
    const bool now = contentHash(m_canvas->document()) != tab.savedHash;
    if (now == tab.unsaved)
        return;
    tab.unsaved = now;
    refreshRegistry();
    emit tabsChanged();
}

void Workflows::refreshRegistry() {
    if (!m_commands)
        return;
    m_commands->setUnsavedWork(unsaved());
    // Ctrl+W on a lone, empty, untitled workflow has nothing to close: it quits.
    m_commands->setWorkflowOpen(!nothingOpen());
}

bool Workflows::isPristine(int index) const {
    if (index < 0 || index >= count())
        return false;
    const Tab &tab = m_tabs[size_t(index)];
    if (!tab.path.isEmpty() || tab.unsaved)
        return false;
    const Document doc = documentOf(index);
    QStringList layoutKeys = doc.layout.keys();
    layoutKeys.removeAll(QStringLiteral("view"));
    return doc.graph.nodes.isEmpty() && doc.groups.isEmpty() && layoutKeys.isEmpty();
}

int Workflows::addTab(const Document &document, const QString &path, const QString &title, bool forceUnsaved,
                      bool replacePristine, const QString &id) {
    const int from = m_current;
    int at = count();
    if (replacePristine && isPristine(m_current)) {
        at = m_current;
        const QString gone = m_tabs[size_t(at)].id;
        m_tabs.erase(m_tabs.begin() + at);
        m_current = -1;
        emit tabClosed(gone);
    } else {
        snapshotCurrent();
        // The tab being left keeps its undo stack; the new one starts with an empty one.
        if (m_current >= 0)
            m_tabs[size_t(m_current)].undo = m_canvas->takeUndoStack();
    }
    Tab tab;
    tab.id = id.isEmpty() ? newTabId() : id;
    tab.path = path;
    tab.title = title.isEmpty() ? untitledName() : title;
    tab.document = document;
    // A saved workflow gets its remembered color back; any other tab the first color no open tab has.
    tab.color = m_tabColors.value(path, -1);
    if (tab.color < 0)
        tab.color = freeTabColor();
    m_tabs.insert(m_tabs.begin() + at, std::move(tab));
    m_current = at;
    m_canvas->setDocument(document);
    // The baseline is what the canvas makes of the document (it places nodes the file has no
    // position for), so opening a file never starts out unsaved. A recovered tab has none.
    Tab &added = m_tabs[size_t(at)];
    added.savedHash = forceUnsaved ? QString() : contentHash(m_canvas->document());
    added.unsaved = forceUnsaved;
    refreshRegistry();
    emit tabsChanged();
    emit currentChanged(from, at);
    return at;
}

// The first color no open tab has; when every color is taken, the one the fewest tabs have.
int Workflows::freeTabColor() const {
    int used[kTabColors] = {};
    for (const Tab &tab : m_tabs)
        if (tab.color >= 0 && tab.color < kTabColors)
            ++used[tab.color];
    int best = 0;
    for (int i = 1; i < kTabColors; ++i)
        if (used[i] < used[best])
            best = i;
    return best;
}

void Workflows::rememberTabColor(const Tab &tab) {
    if (tab.path.isEmpty())
        return;
    m_tabColors.insert(tab.path, tab.color);
    QJsonObject stored;
    for (auto it = m_tabColors.begin(); it != m_tabColors.end(); ++it)
        stored.insert(it.key(), it.value());
    QDir().mkpath(QFileInfo(m_tabColorsPath).path());
    QSaveFile file(m_tabColorsPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(stored).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void Workflows::setTabColor(int index, int color) {
    if (index < 0 || index >= count() || color < 0 || color >= kTabColors || m_tabs[size_t(index)].color == color)
        return;
    m_tabs[size_t(index)].color = color;
    rememberTabColor(m_tabs[size_t(index)]);
    emit tabsChanged();
}

void Workflows::setCurrent(int index) {
    if (index == m_current || index < 0 || index >= count())
        return;
    const int from = m_current;
    snapshotCurrent();
    m_tabs[size_t(m_current)].undo = m_canvas->takeUndoStack();
    m_current = index;
    m_canvas->setDocument(m_tabs[size_t(index)].document);
    m_canvas->setUndoStack(std::move(m_tabs[size_t(index)].undo));
    refreshRegistry();
    emit tabsChanged();
    emit currentChanged(from, index);
    m_choice->check(ModelChoice::Check::TabShown); // the tab shown now (restored with the session, or reached from another)
}

void Workflows::removeTab(int index) {
    if (index < 0 || index >= count())
        return;
    const QString id = m_tabs[size_t(index)].id;
    m_recovery.remove(id);
    m_choice->tabClosed(id);
    if (count() == 1) {
        // The window always has a workflow: closing the last one leaves a fresh one.
        m_tabs.clear();
        m_current = -1;
        emit tabClosed(id);
        addTab(Document(), {}, {}, false, false);
        return;
    }
    const int from = m_current;
    const bool wasCurrent = index == m_current;
    if (!wasCurrent)
        snapshotCurrent();
    m_tabs.erase(m_tabs.begin() + index);
    emit tabClosed(id);
    if (wasCurrent) {
        m_current = qMin(index, count() - 1);
        m_canvas->setDocument(m_tabs[size_t(m_current)].document);
        m_canvas->setUndoStack(std::move(m_tabs[size_t(m_current)].undo));
    } else if (index < m_current) {
        --m_current;
    }
    refreshRegistry();
    emit tabsChanged();
    emit currentChanged(from, m_current);
}

void Workflows::newWorkflow() { addTab(Document(), {}, {}, false, false); }

// --- open ---

bool Workflows::openPath(const QString &pathOrUrl) {
    beginOpen(QStringLiteral("path:") + pathOrUrl);
    const QString path = toPath(pathOrUrl);
    const QFileInfo info(path);
    if (info.isFile()) {
        for (int i = 0; i < count(); ++i) {
            const QString &open = m_tabs[size_t(i)].path;
            if (!open.isEmpty() && QFileInfo(open).canonicalFilePath() == info.canonicalFilePath()) {
                setCurrent(i);
                return true;
            }
        }
    }
    if (info.isFile() && isSharePackage(path))
        return canvas::isCanvasPackage(path) ? openCanvasPackage(path) : openPackage(path);
    const Result<ImportedWorkflow> imported = readWorkflowFile(path);
    if (!imported.ok()) {
        setNotice(tr("Could not open %1: %2").arg(info.fileName().isEmpty() ? path : info.fileName(),
                                                    firstMessage(imported.errors())));
        return false;
    }
    const Document &document = imported->document;
    // An output's workflow is a new, unnamed one: saving asks where, never touches the image.
    const bool fromImage = imported->kind == SourceKind::Image;
    addTab(document, fromImage ? QString() : info.absoluteFilePath(),
           fromImage ? info.completeBaseName() : info.fileName(), false, true);
    analyze(document, m_tabs[size_t(m_current)].title);
    offerReadiness();
    return true;
}

// A package that carries a canvas (#249): it is imported into this project's canvases as a new canvas, never over one.
bool Workflows::openCanvasPackage(const QString &path) {
    const QString shown = QFileInfo(path).fileName();
    canvas::CanvasStore store = canvas::CanvasStore::forSession();
    const Result<canvas::CanvasImported> got = canvas::importCanvasPackage(path, store);
    if (!got) {
        setNotice(tr("Could not open %1: %2").arg(shown, firstMessage(got.errors())));
        return false;
    }
    QString line = got->part == canvas::SharedPart::History
                       ? tr("Imported the canvas %1 from %2, with its history (%3 revisions). Open it from the Canvas view.").arg(got->info.name, shown).arg(got->revisions)
                       : tr("Imported the canvas %1 from %2. Open it from the Canvas view.").arg(got->info.name, shown);
    if (got->renamedId)
        line += QLatin1Char(' ') + tr("You already had a canvas with its id, so it is a new canvas and yours is untouched.");
    setNotice(line);
    return true;
}

bool Workflows::openPackage(const QString &path) {
    const QString shown = QFileInfo(path).fileName();
    // Unpacked only for as long as it opens: its samples go into the inputs folder (openTemplate).
    // Under the cache, not /tmp, which is memory on Arch.
    QDir().mkpath(cacheDir());
    // An app bundle (#394) carries packs, which the Get ready sheet reads from here: its folder lives as long as the tab.
    const auto unpacked = std::make_shared<QTemporaryDir>(cacheDir() + QStringLiteral("/package-XXXXXX"));
    if (!unpacked->isValid()) {
        setNotice(tr("Could not open %1: %2").arg(shown, unpacked->errorString()));
        return false;
    }
    const Result<QString> folder = unpackSharePackage(path, unpacked->path());
    if (!folder) {
        setNotice(tr("Could not open %1: %2").arg(shown, firstMessage(folder.errors())));
        return false;
    }
    const TemplateLibrary library({{QFileInfo(folder.value()).absolutePath(), false}}, m_registry);
    const std::optional<Template> chosen = library.find(QFileInfo(folder.value()).fileName());
    if (!chosen) {
        const Errors problems = library.problems();
        setNotice(tr("Could not open %1: %2")
                      .arg(shown, problems.isEmpty() ? tr("it was made by a newer Fugue; update Fugue to open it.")
                                                     : firstMessage(problems)));
        return false;
    }
    m_pendingPackage = {QFileInfo(path).absoluteFilePath(), chosen->carriedPacks.isEmpty() ? nullptr : unpacked};
    const bool opened = openTemplate(*chosen, true);
    m_pendingPackage = {};
    return opened;
}

QString Workflows::shareSuffix() const {
    if (m_current < 0)
        return QString::fromLatin1(workflowPackageSuffix);
    const Result<std::optional<app::App>> block = app::appOf(documentOf(m_current));
    return QString::fromLatin1(block && *block ? appPackageSuffix : workflowPackageSuffix);
}

bool Workflows::exportShare(const QString &pathOrUrl, const QString &preview) {
    if (m_current < 0)
        return false;
    QString path = toPath(pathOrUrl);
    // An app (a workflow with an `app` block) is shared as a bundle that holds what it needs but the models (#394).
    const bool app = shareSuffix() == QLatin1String(appPackageSuffix);
    const QString suffix = QFileInfo(path).suffix();
    if (suffix.compare(QLatin1String(workflowPackageSuffix), Qt::CaseInsensitive) != 0
        && suffix.compare(QLatin1String(appPackageSuffix), Qt::CaseInsensitive) != 0)
        path += QLatin1Char('.') + shareSuffix();
    const Tab &tab = m_tabs[size_t(m_current)];
    ShareRequest request;
    request.app = app;
    request.carried = tab.carried;
    request.title = tab.title.endsWith(QStringLiteral(".fugue")) ? tab.title.chopped(6) : tab.title;
    request.workflowPath = tab.path;
    request.preview = preview;
    request.appVersion = appVersion();
    QStringList included;
    const Errors errors = writeSharePackage(documentOf(m_current), *m_registry, request, path, &included);
    if (hasErrors(errors)) {
        setNotice(tr("Could not export %1: %2").arg(request.title, withHints(errors)));
        return false;
    }
    QString line = app ? tr("Exported the app %1 for sharing to %2. It carries its packs and sample files; models are not in it: who opens it downloads them after one approval.")
                             .arg(request.title, path)
                       : tr("Exported %1 for sharing to %2. Models are not in it: who opens it can download them.")
                             .arg(request.title, path);
    if (!included.isEmpty())
        line += QLatin1Char(' ') + tr("Images in it: %1.").arg(included.join(QStringLiteral(", ")));
    if (!errors.isEmpty())
        line += QLatin1Char(' ') + withHints(errors);
    setNotice(line);
    return true;
}

bool Workflows::exportCompiled(const QString &pathOrUrl) {
    if (m_current < 0)
        return false;
    QString path = toPath(pathOrUrl);
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".json");
    const QString title = m_tabs[size_t(m_current)].title;
    // As a run compiles it (compileForRun): its providers and trust apply, so this is the graph a run
    // would run. The providers go into a copy, not into the registry the window shows.
    Registry registry = *m_registry;
    const Compilation compilation = compileForRun(documentOf(m_current), registry, {},
                                                  runCompileSetup(m_registerProviders, m_trust, m_activeCanvas));
    if (!compilation) {
        setNotice(tr("%1 does not compile: %2").arg(title, withHints(compilation.errors())));
        return false;
    }
    const Errors errors = createFile(path, writeJson(compilation->toJson()), QFileInfo(path).fileName());
    if (hasErrors(errors)) {
        setNotice(tr("Could not export %1: %2").arg(title, withHints(errors)));
        return false;
    }
    setNotice(tr("Exported the compiled graph of %1 to %2.").arg(title, path));
    return true;
}

void Workflows::openUrls(const QVariantList &urls) {
    for (const QVariant &url : urls) {
        const QString text = url.toString();
        if (url.canConvert<QUrl>() && !text.startsWith(QStringLiteral("file:")) && url.toUrl().isLocalFile()) {
            openPath(url.toUrl().toLocalFile());
        } else if (text.startsWith(QStringLiteral("file:")) || text.startsWith('/') || text.startsWith('~')) {
            openPath(text);
        } else {
            setNotice(tr("Only files can be opened, not %1.").arg(text));
        }
    }
}

void Workflows::dropUrls(const QVariantList &urls, qreal x, qreal y) {
    const QString onNode = m_canvas->nodeAt(x, y);
    const bool onLoadImage = !m_images->imageInput(onNode).isEmpty();
    QStringList images, workflows;
    for (const QVariant &url : urls) {
        const QString text = url.toString();
        const bool isFile = text.startsWith(QStringLiteral("file:")) || text.startsWith('/') || text.startsWith('~');
        const QString path = isFile ? toPath(text) : QString();
        if (path.isEmpty()) {
            setNotice(tr("Only files can be opened, not %1.").arg(text));
            continue;
        }
        // Only a regular file is opened at all: a pipe would wait for a writer forever.
        if (const QFileInfo info(path); info.exists() && !info.isFile() && !info.isDir()) {
            setNotice(tr("Could not open %1: it is not a file.").arg(QFileInfo(path).fileName()));
            continue;
        }
        QFile file(path);
        const bool image = file.open(QIODevice::ReadOnly) && sniffImage(file.read(16));
        file.close();
        // A picture with a workflow in it opens as that workflow, except onto a Load Image.
        if (image && (onLoadImage || !readEmbeddedWorkflow(path).ok()))
            images.append(path);
        else
            workflows.append(path);
    }
    if (!images.isEmpty())
        m_images->dropFiles(images, QPointF(x, y), onNode);
    for (const QString &path : workflows)
        openPath(path);
}

bool Workflows::openText(const QString &text) {
    const Result<Document> document = readWorkflowText(text);
    if (!document.ok()) {
        setNotice(tr("The text is not a Fugue workflow: %1").arg(firstMessage(document.errors())));
        return false;
    }
    addTab(document.value(), {}, {}, false, true);
    analyze(document.value(), m_tabs[size_t(m_current)].title);
    return true;
}

bool Workflows::openTemplate(const Template &chosen, bool fromPackage) {
    Result<Document> document = Document::loadFile(chosen.workflow);
    Errors errors = document ? Errors() : document.errors();
    if (document) {
        makeInputsAbsolute(document.value(), *m_registry, chosen.workflow);
        // Opened with the files the person has in place of the models it names that are not on disk (#605): this tab's copy only.
        if (!m_openUses.isEmpty())
            document.value() = withUses(document.value(), m_openUses);
        // Every file a template ships is taken in, not only its pictures: a song, a clip or a LUT left in the template's
        // folder is named by a full path that an AppImage mounts somewhere else at each start, so a saved copy would lose it.
        // Template inputs are small (fugue-workflows' bin/check caps them) and stored once by hash.
        errors = takeInputsFrom(document.value(), *m_registry, chosen.folder, true);
    }
    if (document && fromPackage && errors.isEmpty()) {
        const QString folder = canonicalPath(chosen.folder);
        forEachFileLiteral(document.value(), *m_registry, [&](const QString &, const QString &, InputValue &value) {
            const QString path = canonicalPath(value.literal().toString());
            if (!path.isEmpty() && isInside(path, folder))
                value = InputValue::literal(QFileInfo(path).fileName());
        });
        // A node of a pack not installed here: which inputs are files is not known, so any text
        // naming one of the package's images is taken in too, to be there once the pack is.
        const FileRoots roots = FileRoots::forInputs();
        const auto takeUnknown = [&](Nodes &nodes) {
            for (Node &node : nodes) {
                if (m_registry->nodeType(node.type))
                    continue;
                for (InputValue &value : node.inputs) {
                    if (value.kind() != InputValue::Kind::Literal || !value.literal().isString()
                        || !value.literal().toString().startsWith(QStringLiteral("inputs/")))
                        continue;
                    const QString path = canonicalPath(folder + QLatin1Char('/') + value.literal().toString());
                    if (path.isEmpty() || !isInside(path, folder) || !QFileInfo(path).isFile())
                        continue;
                    if (const Result<QString> stored = storeImageFile(path, inputsDir(), roots))
                        value = InputValue::literal(stored.value());
                }
            }
        };
        takeUnknown(document->graph.nodes);
        for (GroupDefinition &group : document->groups)
            takeUnknown(group.nodes);
    }
    if (!errors.isEmpty()) {
        setNotice(tr("Could not open the template %1: %2").arg(chosen.title, firstMessage(errors)));
        return false;
    }
    addTab(document.value(), {}, chosen.title, true, true);
    // Its carried packs stay in the template's folder, so a package (unpacked only while it
    // opens) offers none; the workflow's requires.packs source is its offer then (#91). An app bundle's folder lives as long
    // as the tab (m_pendingPackage), so it offers the packs it carries (#394).
    if (fromPackage) {
        Tab &tab = m_tabs[size_t(m_current)];
        tab.packagePath = m_pendingPackage.path;
        tab.unpacked = m_pendingPackage.unpacked;
        if (tab.unpacked)
            tab.carried = chosen.carriedPacks;
        emit tabsChanged();
    } else {
        m_tabs[size_t(m_current)].carried = chosen.carriedPacks;
        m_tabs[size_t(m_current)].templateFolder = chosen.folder;
        emit tabsChanged(); // addTab's came before the folder was set, and the App view's launcher reads it (#398)
    }
    analyze(document.value(), chosen.title);
    offerReadiness();
    return true;
}

QVariantList Workflows::templates() const {
    FileHashes hashes(FileHashes::defaultPath());
    const ModelSpeeds speeds;
    const ModelLibrary models = ModelLibrary::current(m_registry);
    const ModelCatalog catalog = ModelCatalog::load();
    const bool useCovered = autoUseOn(); // "Use my file": a template partly covered opens with the files it has
    QVariantList out;
    for (const Template &t : TemplateLibrary(m_templateRoots, m_registry).list()) {
        QVariantList missing;
        ModelLibrary library = models;
        downloadable(t.models, library); // the folders they would go in
        const QList<MissingModel> absent = library.missing(t.models, hashes);
        // The same model is on disk in another file (#370, #605): per missing model, the file; `covered` are the hashes it would replace.
        QVariantList onDisk;
        QSet<QString> covered;
        QList<MissingModel> uncovered, coveredModels; // what a download would fetch, and what the person's files stand in for
        const RecommendContext context{localComputeDevice(), 0, &speeds};
        for (const MissingModel &model : absent) {
            const std::optional<OnDiskMatch> match = findOnDisk(model.handle, library, context, ModelShapes::shared());
            missing.append(QVariantMap{{"name", model.handle.value(QStringLiteral("name")).toString()},
                                       {"source", model.source},
                                       {"folder", model.folder},
                                       {"covered", bool(match)},
                                       {"suggest", match ? match->name : QString()}});
            if (!match)
                continue;
            coveredModels.append(model);
            const OnDiskUse use{model.handle, *match};
            covered.insert(model.handle.value(QStringLiteral("hash")).toString());
            onDisk.append(QVariantMap{{"name", model.handle.value(QStringLiteral("name")).toString()},
                                      {"hash", model.handle.value(QStringLiteral("hash")).toString()},
                                      {"same", use.sameModelText()},
                                      {"file", match->name},
                                      {"tier", match->tier},
                                      {"otherSource", match->otherSource}});
        }
        uncovered = uncoveredModels(absent, library, context);
        // Where each model is: on disk (not among `absent`), or served (#164).
        QList<ModelPlaces> placed;
        for (const QJsonValue &value : t.models) {
            const QJsonObject handle = value.toObject();
            const bool absentHere = std::any_of(absent.cbegin(), absent.cend(), [&](const MissingModel &m) {
                return m.handle.value(QStringLiteral("name")) == handle.value(QStringLiteral("name"))
                       && m.handle.value(QStringLiteral("resource")) == handle.value(QStringLiteral("resource"))
                       && !covered.contains(m.handle.value(QStringLiteral("hash")).toString()); // a file of the same model is here
            });
            placed.append(placesOf(handle, !absentHere, m_served));
        }
        TemplatePlaces places = summarizePlaces(placed);
        // The models a provider serves (a name, no hash) are not files to fetch: they add where they run when an
        // off-machine instance offers them (the badge), and never change `ready`.
        for (const QJsonValue value : t.providerModels)
            for (const QString &cloud : placesOf(value.toObject(), false, m_served).clouds)
                if (!places.clouds.contains(cloud))
                    places.clouds << cloud;
        QStringList status;
        const qsizetype stillMissing = missing.size() - covered.size();
        if (stillMissing > 0) {
            status << tr("Models missing: %1 of %2").arg(stillMissing).arg(t.models.size());
            if (!covered.isEmpty())
                status << tr("you have %n of them", nullptr, int(covered.size()));
        } else if (!covered.isEmpty() && places.ready) {
            // Ready to run with what you have (#605): the tier of the file when there is one, else the plain words.
            const QString tier = onDisk.size() == 1 ? onDisk.at(0).toMap().value("tier").toString() : QString();
            status << (tier.isEmpty() ? tr("Ready · uses a file you have") : tr("Ready · uses your %1 file").arg(tier));
        }
        // A pack the template carries is installed from it, after a question (#118).
        QStringList carried, elsewhere;
        for (const QString &pack : t.neededPacks) {
            const bool here = std::any_of(t.carriedPacks.cbegin(), t.carriedPacks.cend(),
                                          [&](const CarriedPack &c) { return c.id == pack; });
            (here ? carried : elsewhere) << pack;
        }
        if (!elsewhere.isEmpty())
            status << tr("Packs needed: %1").arg(elsewhere.join(QStringLiteral(", ")));
        if (!carried.isEmpty())
            status << tr("Carries the pack to install: %1").arg(carried.join(QStringLiteral(", ")));
        QVariantList packs;
        for (const QString &pack : t.neededPacks) {
            const auto carriedHere = std::find_if(t.carriedPacks.cbegin(), t.carriedPacks.cend(), [&](const CarriedPack &c) { return c.id == pack; });
            packs.append(QVariantMap{{"id", pack},
                                     {"title", packTitle(pack, carriedHere != t.carriedPacks.cend() ? carriedHere->name : QString())},
                                     {"carried", carriedHere != t.carriedPacks.cend()},
                                     {"installed", false}});
        }
        for (const CarriedPack &c : t.carriedPacks)
            if (!t.neededPacks.contains(c.id))
                packs.append(QVariantMap{{"id", c.id}, {"title", packTitle(c.id, c.name)}, {"carried", true}, {"installed", true}});
        QStringList inputs; // the sample pictures of its inputs folder, as file URLs
        const QDir inputsDir(t.folder + QStringLiteral("/inputs"));
        for (const QFileInfo &file : inputsDir.entryInfoList({QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.webp")}, QDir::Files, QDir::Name))
            inputs << QUrl::fromLocalFile(file.absoluteFilePath()).toString();
        out.append(QVariantMap{{"id", t.id},
                               {"modelList", modelRows(t.models, placed, catalog)},
                               {"packs", packs},
                               {"inputs", inputs},
                               {"title", t.app ? t.app->title : t.title}, // an app's card says what the app is called (#387)
                               {"description", t.app ? t.app->summary : t.description},
                               {"app", t.app.has_value()},
                               {"appKind", t.app ? t.app->kind : QString()},
                               {"category", t.category},
                               {"categoryTitle", t.categoryTitle},
                               {"tags", t.tags},
                               {"source", t.source.toVariantMap()},
                               {"families", t.families},
                               {"preview", t.preview.isEmpty() ? QString() : QUrl::fromLocalFile(t.preview).toString()},
                               {"video", t.video.isEmpty() ? QString() : QUrl::fromLocalFile(t.video).toString()},
                               {"own", t.own},
                               {"inherited", t.inherited},
                               {"missingModels", missing},
                               {"onDisk", onDisk},
                               {"usesOnDisk", !covered.isEmpty() && places.ready},
                               {"ready", places.ready},
                               {"local", places.local},
                               {"hosts", places.hosts},
                               {"offlineHosts", places.offlineHosts},
                               {"clouds", places.clouds},
                               {"download", offerText(uncovered)},
                               {"downloadSize", downloadSize(uncovered)},
                               {"ownDownloadSize", downloadSize(coveredModels)}, // what the template's own models would fetch, for the details' Download
                               {"opensWithFiles", !absent.isEmpty() && uncovered.isEmpty() && t.neededPacks.isEmpty()},
                               // Open puts the person's files in (coveringUses): all of them with "Use my file", only a full cover with "Ask me".
                               {"opensUsingFiles", !onDisk.isEmpty() && (useCovered || (uncovered.isEmpty() && t.neededPacks.isEmpty()))},
                               {"neededPacks", t.neededPacks},
                               {"status", status.join(QStringLiteral(" · "))}});
    }
    return out;
}

bool Workflows::templateMatches(const QString &query, const QVariantMap &templ) const {
    return templateMatchesSearch(query, templ);
}

QVariantMap Workflows::missingModels(const QJsonArray &models) const {
    FileHashes hashes(FileHashes::defaultPath());
    ModelLibrary library = ModelLibrary::current(m_registry);
    downloadable(models, library); // the folders they would go in
    const QList<MissingModel> absent = library.missing(models, hashes);
    return {{"count", int(absent.size())}, {"total", int(models.size())}, {"download", offerText(absent)}, {"size", downloadSize(absent)}};
}

void Workflows::loadHostModels() {
    const ModelSource source = m_source ? m_source : ModelSource(listOfferedModels);
    QPointer<Workflows> self(this);
    source(QString(), {}, [self](QList<ListedModel> models) {
        if (!self)
            return;
        self->m_served = std::move(models);
        emit self->templatesChanged();
    });
}

void Workflows::refreshTemplateRoots() {
    m_templateRoots = TemplateLibrary::standardRoots();
    emit templatesChanged();
}

QList<Template> Workflows::templateList() const {
    return TemplateLibrary(m_templateRoots, m_registry).list();
}

bool Workflows::openCopyOf(const QString &file, const QString &title) {
    const Result<Document> document = Document::loadFile(file);
    if (!document) {
        setNotice(tr("Could not open %1: %2").arg(title, firstMessage(document.errors())));
        return false;
    }
    addTab(document.value(), {}, title, true, true);
    analyze(document.value(), title);
    return true;
}

bool Workflows::openTemplateById(const QString &id) { return openTemplateByIdThen(id, {}); }

bool Workflows::openTemplateAsIs(const QString &id) {
    const std::optional<Template> chosen = TemplateLibrary(m_templateRoots, m_registry).find(id);
    if (!chosen) {
        setNotice(tr("There is no template %1.").arg(id));
        return false;
    }
    return openTemplate(*chosen);
}

bool Workflows::openTemplateByIdThen(const QString &id, std::function<void(bool)> opened) {
    const std::optional<Template> chosen = TemplateLibrary(m_templateRoots, m_registry).find(id);
    if (!chosen) {
        setNotice(tr("There is no template %1.").arg(id));
        return false;
    }
    return openTemplateWithFiles(*chosen, std::move(opened));
}

bool Workflows::openTemplateWithFiles(const Template &chosen, std::function<void(bool)> opened) {
    beginOpen(QStringLiteral("open:") + chosen.id);
    // Ready only through the files the person has: it opens with them, no sheet (UI design 2026-11-26).
    // "Use my file" (the setting, #618): whatever is covered is used, and Get ready opens for the rest. "Ask me": only when all of it is.
    if (const QList<OnDiskUse> uses = coveringUses(chosen, autoUseOn()); !uses.isEmpty())
        return openWithUses(chosen, uses, std::move(opened));
    const bool done = openTemplate(chosen);
    if (opened)
        opened(done); // at once: the tab is there
    return done;
}

QString Workflows::duplicateTemplate(const QString &id, const QString &title) {
    const TemplateLibrary library(m_templateRoots, m_registry);
    const std::optional<Template> chosen = library.find(id);
    if (!chosen) {
        setNotice(tr("There is no template %1.").arg(id));
        return {};
    }
    QString copied;
    if (const Errors failed = library.duplicate(*chosen, title, &copied); !failed.isEmpty()) {
        setNotice(failed.first().message);
        return {};
    }
    emit templatesChanged();
    return copied;
}

bool Workflows::renameTemplate(const QString &id, const QString &title) {
    const TemplateLibrary library(m_templateRoots, m_registry);
    const std::optional<Template> chosen = library.find(id);
    if (!chosen) {
        setNotice(tr("There is no template %1.").arg(id));
        return false;
    }
    if (const Errors failed = library.rename(*chosen, title); !failed.isEmpty()) {
        setNotice(failed.first().message);
        return false;
    }
    emit templatesChanged();
    return true;
}

bool Workflows::openClipboard() {
    return openText(QGuiApplication::clipboard()->text());
}

void Workflows::openStartup(const QStringList &paths) {
    for (const QString &path : paths)
        openPath(path);
}

// --- the last session's tabs ---

void Workflows::setSessionEnabled(bool enabled) {
    if (enabled == m_sessionEnabled)
        return;
    m_sessionEnabled = enabled;
    if (!enabled) {
        m_sessionTimer.stop();
        QFile::remove(m_sessionPath);
    } else {
        scheduleSession();
    }
}

void Workflows::scheduleSession() {
    if (m_sessionEnabled && m_sessionLive)
        m_sessionTimer.start();
}

void Workflows::flushSession() {
    m_sessionTimer.stop();
    if (!m_sessionEnabled || !m_sessionLive)
        return;
    TabSession session;
    for (int i = 0; i < count(); ++i)
        if (!m_tabs[size_t(i)].path.isEmpty())
            session.paths << m_tabs[size_t(i)].path;
    if (m_current >= 0)
        session.active = m_tabs[size_t(m_current)].path;
    const Errors errors = saveTabSession(m_sessionPath, session);
    if (!errors.isEmpty() && !m_sessionWarned) {
        m_sessionWarned = true;
        qWarning("Fugue: the open tabs are not kept: %s", qPrintable(errors.first().message));
    }
}

void Workflows::restoreSession() {
    m_sessionLive = true; // a setting turned on later keeps the tabs from then
    if (!m_sessionEnabled)
        return;
    const TabSession last = loadTabSession(m_sessionPath);
    TabRestore plan = planRestore(last);
    QStringList opened;
    for (const QString &path : plan.paths) {
        if (openPath(path))
            opened << path;
        else
            plan.skipped << path;
    }
    for (int i = 0; i < count(); ++i)
        if (m_tabs[size_t(i)].path == plan.active && opened.contains(plan.active))
            setCurrent(i);
    flushSession();
    if (!plan.skipped.isEmpty()) {
        QStringList names;
        for (const QString &path : plan.skipped)
            names << QFileInfo(path).fileName();
        setNotice(tr("%n tabs of the last session were not reopened, their files are gone or unreadable: %1",
                     nullptr, int(plan.skipped.size()))
                      .arg(names.join(QStringLiteral(", "))));
    }
}

QVariantMap Workflows::carriedPackOffer(const Document &document) const {
    if (m_current < 0)
        return {};
    const QStringList needed = packsNeeded(document, *m_registry);
    for (const CarriedPack &pack : m_tabs[size_t(m_current)].carried)
        if (needed.contains(pack.id))
            return QVariantMap{{"id", pack.id},
                               {"source", pack.folder},
                               {"ref", QString()},
                               {"version", pack.version},
                               {"text", tr("Install %1 %2…").arg(pack.id, pack.version)}};
    return {};
}

QVariantMap Workflows::packNeedOf(const Document &document, const QString &pack) const {
    QString name;
    QVariantMap offer;
    if (m_current >= 0)
        for (const CarriedPack &carried : m_tabs[size_t(m_current)].carried)
            if (carried.id == pack) {
                name = carried.name;
                offer = QVariantMap{{"source", carried.folder}, {"ref", QString()}};
                break;
            }
    const QJsonObject entry = document.requirements.value(QStringLiteral("packs")).toObject().value(pack).toObject();
    if (name.isEmpty())
        name = entry.value(QStringLiteral("name")).toString();
    if (offer.isEmpty() && !entry.value(QStringLiteral("source")).toString().isEmpty() && m_registry->packVersion(pack).isEmpty())
        offer = QVariantMap{{"source", entry.value(QStringLiteral("source")).toString()},
                            {"ref", entry.value(QStringLiteral("ref")).toString()}};
    const QString title = packTitle(pack, name);
    return QVariantMap{{"id", pack},
                       {"name", title},
                       {"text", tr("Needs the %1 pack").arg(title)},
                       {"installable", !offer.isEmpty()},
                       {"source", offer.value("source")},
                       {"ref", offer.value("ref")}};
}

QVariantMap Workflows::packNeed(const QString &pack) const {
    return m_current < 0 ? QVariantMap() : packNeedOf(documentOf(m_current), pack);
}

bool Workflows::installPackById(const QString &pack) {
    const QVariantMap need = packNeed(pack);
    if (!need.value("installable").toBool())
        return false;
    emit packInstallRequested(need.value("source").toString(), need.value("ref").toString());
    return true;
}

QString Workflows::missingNotice(const Document &document, const QString &title, DownloadOffer *offer) const {
    QStringList lines;
    const QVariantMap carried = carriedPackOffer(document);
    if (!carried.isEmpty())
        lines << tr("%1 carries the pack %2 %3, which its nodes need. Get ready… installs and trusts it with everything else, after one question.")
                     .arg(title, carried.value("id").toString(), carried.value("version").toString());
    const QStringList missing = missingTypes(document, *m_registry);
    // #266: each missing pack once, by display name; a node type with no pack of its own stays named.
    QStringList missingPacks; // ids, in the order the types come
    QStringList names;
    for (const QString &type : missing) {
        const QString pack = packOfType(type);
        if (pack.isEmpty())
            names << type;
        else if (!missingPacks.contains(pack)) {
            missingPacks << pack;
            names << packNeedOf(document, pack).value("name").toString();
        }
    }
    if (!missing.isEmpty())
        lines << tr("%1 uses nodes that are not installed (%2). They open as placeholders and keep their inputs.")
                     .arg(title, names.join(QStringLiteral(", ")));
    // Spec 4.3: the models it names that are not here, without reading a model file.
    FileHashes hashes(FileHashes::defaultPath());
    ModelLibrary library = ModelLibrary::current(m_registry);
    QList<PortType> types;
    const QJsonArray wanted = modelsToHave(document, *m_registry); // not a model only a Switch's lazy side holds (#557)
    for (const MissingModel &model : downloadable(wanted, library))
        if (const std::optional<PortType> type = PortType::parse(model.handle.value(QStringLiteral("resource")).toString()))
            types << *type;
    const QList<MissingModel> absent = library.missing(wanted, hashes); // with the fitted folders
    QStringList models;
    for (const MissingModel &model : absent) {
        const QString name = model.handle.value(QStringLiteral("name")).toString();
        models << (model.source.isEmpty() ? tr("%1, into %2").arg(name, model.folder)
                                          : tr("%1 from %2, into %3").arg(name, model.source, model.folder));
    }
    if (!models.isEmpty()) {
        QString line = tr("%1 needs models that are not in the model folders: %2.").arg(title, models.join(QStringLiteral("; ")));
        // The same weights may be here in another file: Get ready… can use it with no download (#605).
        const ModelSpeeds speeds;
        const int have = int(onDiskUses(absent, library, RecommendContext{localComputeDevice(), 0, &speeds}, ModelShapes::shared()).size());
        if (have > 0)
            line += QLatin1Char(' ') + tr("You already have %n of them in another file; Get ready… can use it with no download.", nullptr, have);
        lines << line;
    }
    if (offer) {
        // Each missing pack the workflow says where to get (4.3); the carried one alone when it carries any.
        offer->packs.clear();
        if (!carried.isEmpty())
            offer->packs << carried;
        else
            for (const QString &pack : missingPacks) {
                const QVariantMap need = packNeedOf(document, pack);
                if (need.value("installable").toBool())
                    offer->packs << QVariantMap{{"id", pack},
                                                {"source", need.value("source")},
                                                {"ref", need.value("ref")},
                                                {"text", tr("Install %1…").arg(need.value("name").toString())}};
            }
        offer->text = offerText(absent);
        offer->models = wanted;
        offer->root = library.downloadRoot();
        offer->roots.clear();
        if (!offer->text.isEmpty())
            for (const QString &root : library.downloadRoots(types))
                offer->roots.append(QVariantMap{{"path", root}, {"title", rootTitle(root, library)}});
    }
    return lines.join(QLatin1Char(' '));
}

void Workflows::modelFoldersChanged(const QString &notice) {
    refreshMissing(false);
    if (!notice.isEmpty())
        setNotice(notice);
    emit templatesChanged(); // templates() reads the model folders afresh, so a card whose model is now on disk reads as ready
}

void Workflows::refreshMissing(bool force) {
    if (m_current < 0)
        return;
    const QString missing = missingNotice(shownDocument(), m_tabs[size_t(m_current)].title, &m_offer);
    const bool shown = m_notice == m_missingNotice;
    m_missingNotice = missing;
    if (force || shown)
        setNotice(missing);
    emit noticeChanged(); // the offer may differ with the same line
    m_choice->check(ModelChoice::Check::ModelsChanged); // a model that is missing and has a file on disk (a folder added, a download ended): "Use my file" uses it
}

QVariantList Workflows::packOffers() const {
    return !m_notice.isEmpty() && m_notice == m_missingNotice ? m_offer.packs : QVariantList();
}

QVariantMap Workflows::packOffer() const {
    const QVariantList offers = packOffers();
    return offers.isEmpty() ? QVariantMap() : offers.first().toMap();
}

void Workflows::installPack() {
    const QVariantMap pack = packOffer();
    if (!pack.isEmpty())
        emit packInstallRequested(pack.value("source").toString(), pack.value("ref").toString());
}

QString Workflows::downloadOffer() const {
    return !m_notice.isEmpty() && m_notice == m_missingNotice && !m_downloads->busy() ? m_offer.text : QString();
}

QList<MissingModel> Workflows::downloadable(const QJsonArray &models, ModelLibrary &library) const {
    FileHashes hashes(FileHashes::defaultPath());
    QList<PortType> types;
    for (const MissingModel &model : library.missing(models, hashes))
        if (const std::optional<PortType> type = PortType::parse(model.handle.value(QStringLiteral("resource")).toString());
            type && !model.source.isEmpty())
            types << *type;
    library.fitDownloadRoot(types);
    QList<MissingModel> out;
    for (const MissingModel &model : library.missing(models, hashes))
        if (!model.source.isEmpty())
            out << model;
    return out;
}

void Workflows::startDownloads(const QJsonArray &models, const QString &what) {
    ModelLibrary library = ModelLibrary::current(m_registry);
    QList<DownloadTarget> targets;
    Errors refused;
    for (const MissingModel &model : downloadable(models, library)) {
        const Result<DownloadTarget> target = planDownload(library, model);
        if (target)
            targets << target.value();
        else
            refused << target.errors();
    }
    if (!refused.isEmpty())
        setNotice(withHints(refused));
    // Said again when they end, beside what failed then; at once when nothing is to come.
    if (targets.isEmpty())
        refused.clear();
    m_refused += refused;
    m_downloads->add(targets, what);
    emit noticeChanged(); // the offer hides while they download
}

void Workflows::downloadMissing() {
    if (!downloadOffer().isEmpty())
        startDownloads(m_offer.models, QStringLiteral("workflow"));
}

bool Workflows::downloadTemplate(const QString &id) {
    const std::optional<Template> chosen = TemplateLibrary(m_templateRoots, m_registry).find(id);
    if (!chosen)
        return false;
    // A template that needs a pack has models whose type may come from it, and a direct download refuses those:
    // the Get ready sheet (the open's own) plans the pack first, then the models.
    if (!chosen->neededPacks.isEmpty())
        return openTemplate(*chosen);
    // Only what the card's button counts: a model the person has a file for (Use my file) is not fetched.
    FileHashes hashes(FileHashes::defaultPath());
    ModelLibrary library = ModelLibrary::current(m_registry);
    downloadable(chosen->models, library);
    const ModelSpeeds speeds;
    QSet<QString> keep;
    for (const MissingModel &model : uncoveredModels(library.missing(chosen->models, hashes), library, RecommendContext{localComputeDevice(), 0, &speeds}))
        keep.insert(model.handle.value(QStringLiteral("hash")).toString());
    QJsonArray wanted;
    for (const QJsonValue &entry : chosen->models)
        if (keep.contains(entry.toObject().value(QStringLiteral("hash")).toString()) || entry.toObject().value(QStringLiteral("hash")).toString().isEmpty())
            wanted.append(entry);
    startDownloads(wanted, QStringLiteral("template:") + id);
    return false;
}

void Workflows::setDownloadRoot(const QString &root) {
    const ModelLibrary library = ModelLibrary::current(m_registry);
    if (!library.roots().contains(root))
        return;
    // Fugue's own folder is the default: no key.
    const Errors errors = changeSetting(settingsPath(), QStringLiteral("download_folder"),
                                        root == library.roots().value(0) ? QJsonValue(QJsonValue::Undefined)
                                                                         : QJsonValue(root));
    if (!errors.isEmpty()) {
        setNotice(withHints(errors));
        return;
    }
    refreshMissing(false); // the line names the folders they go in
}

void Workflows::offerReadiness() {
    // "Use my file" (#618): a model with a file of its own on disk is used first, and the sheet opens for what is really missing when the
    // file has been read (the read runs off the window's thread; the module offers again when it ends).
    if (m_choice->check(ModelChoice::Check::Offer))
        return;
    if (m_choice->offerAfterRead()) // a read of this tab is already going (not started here): the sheet it waited for opens when it ends
        return;
    // Opened but not ready: the Get ready sheet opens by itself (one yes covers the packs, trust, environments and models).
    // Nothing happens before that yes; Cancel leaves the banner's Get ready… button to open it again. The line may be another (the swap's
    // own), so what is left is read from the offer itself, not from the banner.
    if (!m_offer.packs.isEmpty() || (!m_offer.text.isEmpty() && !m_downloads->busy()))
        m_readiness->show();
}

void Workflows::analyze(const Document &document, const QString &title) {
    m_missingNotice = missingNotice(document, title, &m_offer);
    setNotice(m_missingNotice);
    emit noticeChanged(); // the offer may differ with the same line
    if (addPendingTrust(m_trust.untrusted(document, *m_registry)))
        emit pendingTrustChanged();
}

bool Workflows::addPendingTrust(const QList<PackUse> &uses) {
    bool changed = false;
    for (const PackUse &use : uses) {
        bool known = false;
        for (const QVariant &entry : std::as_const(m_pendingTrust))
            known |= entry.toMap().value("id") == use.id && entry.toMap().value("version") == use.version;
        if (known)
            continue;
        // The secrets the pack reads (6.4, #402): names only, from the node types the workflow uses.
        QStringList secrets;
        for (const QString &type : use.types)
            if (const NodeType *node = m_registry->nodeType(type))
                for (const QString &name : node->packSecrets)
                    if (!secrets.contains(name))
                        secrets << name;
        m_pendingTrust.append(QVariantMap{
            {"id", use.id}, {"version", use.version}, {"source", use.source}, {"types", use.types}, {"secrets", secrets}});
        changed = true;
    }
    return changed;
}

// --- save and close ---

bool Workflows::saveTab(int index, const QString &path, bool replace) {
    const QString taken = tr("Could not save %1: a file of that name already exists; choose another name.")
                              .arg(QFileInfo(path).fileName());
    if (!replace && QFileInfo::exists(path)) {
        setNotice(taken);
        return false;
    }
    // Into another folder, the relative paths the tab and its undo steps hold would name other
    // files: they become the full paths they name now, as no step, before the inputs move.
    const QString from = pathOf(index);
    if (index == m_current && !from.isEmpty()
        && canonicalPath(QFileInfo(from).absolutePath()) != canonicalPath(QFileInfo(path).absolutePath()))
        m_canvas->rewriteDocuments([this, from](Document &document) { makeInputsAbsolute(document, *m_registry, from); });
    // Fugue's own documents folder may not exist yet (a project made before it was created, a folder removed
    // since): make it. Any other missing folder is the user's typo and stays an error.
    if (const QString folder = QFileInfo(path).absolutePath(); folder == QDir::cleanPath(documentsDir()))
        QDir().mkpath(folder);
    Document document = documentOf(index);
    // A saved workflow stays portable: the images it takes from Fugue's inputs folder are copied
    // into an `inputs` folder beside it, and its nodes point there (Appendix A, always, no question).
    const Document before = document;
    if (const Errors copied = relocateInputs(document, *m_registry, pathOf(index), path);
        hasErrors(copied)) {
        setNotice(tr("Could not save %1: %2").arg(QFileInfo(path).fileName(), firstMessage(copied)));
        return false;
    }
    const bool moved = contentHash(document) != contentHash(before);
    const Document relocated = document;
    document.refreshRequirements(appVersion(), m_registry);
    const Result<bool> written = writeWorkflow(document, path, replace);
    if (written && !*written) {
        setNotice(taken);
        return false;
    }
    if (const Errors errors = written.errors(); hasErrors(errors)) {
        setNotice(tr("Could not save %1: %2").arg(QFileInfo(path).fileName(), firstMessage(errors)));
        return false;
    }
    Tab &tab = m_tabs[size_t(index)];
    tab.path = path;
    tab.title = QFileInfo(path).fileName();
    rememberTabColor(tab); // a saved tab keeps its color under the new name
    // After the path: the repointed inputs are relative to the new place (Appendix A).
    if (index == m_current && moved)
        m_canvas->commit(relocated, tr("Move images"));
    tab.savedHash = contentHash(document);
    tab.unsaved = false;
    tab.recoveryHash.clear();
    if (index != m_current)
        tab.document = document;
    m_recovery.remove(tab.id);
    setNotice({});
    refreshRegistry();
    emit tabsChanged();
    return true;
}

bool Workflows::save() {
    if (m_current < 0)
        return false;
    if (m_tabs[size_t(m_current)].path.isEmpty()) {
        emit saveAsRequested();
        return false;
    }
    return saveTab(m_current, m_tabs[size_t(m_current)].path);
}

bool Workflows::saveAs(const QString &pathOrUrl) {
    if (m_current < 0)
        return false;
    QString path = toPath(pathOrUrl);
    if (path.isEmpty())
        return false;
    const bool appended = QFileInfo(path).suffix().isEmpty();
    if (appended)
        path += QStringLiteral(".fugue");
    const QString id = m_tabs[size_t(m_current)].id;
    if (!saveTab(m_current, path, !appended))
        return false;
    if (m_closeAfterSave == id) {
        m_closeAfterSave.clear();
        removeTab(m_current);
    }
    return true;
}

void Workflows::closeTab(int index) {
    if (index < 0 || index >= count())
        return;
    if (unsaved(index))
        emit closeConfirmationRequested(index, m_tabs[size_t(index)].title);
    else
        removeTab(index);
}

bool Workflows::renameTab(int index, const QString &name) {
    QString wanted = name.trimmed();
    if (index < 0 || index >= count() || wanted.isEmpty())
        return false;
    if (wanted.contains(QLatin1Char('/')) || wanted == QLatin1String(".") || wanted == QLatin1String("..")) {
        setNotice(tr("A workflow's name cannot hold a slash."));
        return false;
    }
    Tab &tab = m_tabs[size_t(index)];
    if (tab.path.isEmpty()) {
        tab.title = wanted;
        emit tabsChanged();
        return true;
    }
    if (QFileInfo(wanted).suffix().isEmpty())
        wanted += QStringLiteral(".fugue");
    const QFileInfo old(tab.path);
    if (wanted == old.fileName())
        return true;
    const QString path = old.dir().filePath(wanted);
    if (QFileInfo::exists(path)) {
        setNotice(tr("%1 already exists in that folder.").arg(wanted));
        return false;
    }
    if (!QFile::rename(tab.path, path)) {
        setNotice(tr("Could not rename %1 to %2.").arg(old.fileName(), wanted));
        return false;
    }
    tab.path = path;
    tab.title = wanted;
    rememberTabColor(tab);
    setNotice({});
    emit tabsChanged();
    return true;
}

void Workflows::duplicateTab(int index) {
    if (index < 0 || index >= count())
        return;
    const QString base = m_tabs[size_t(index)].title.endsWith(QStringLiteral(".fugue"))
                             ? m_tabs[size_t(index)].title.chopped(6)
                             : m_tabs[size_t(index)].title;
    const Document copy = documentOf(index);
    addTab(copy, {}, tr("%1 copy").arg(base), true, false);
}

void Workflows::closeOtherTabs(int index) {
    if (index < 0 || index >= count())
        return;
    const QString keep = m_tabs[size_t(index)].id;
    refreshUnsaved();
    int kept = 0;
    for (int i = count() - 1; i >= 0; --i) {
        if (m_tabs[size_t(i)].id == keep)
            continue;
        if (m_tabs[size_t(i)].unsaved)
            ++kept;
        else
            removeTab(i);
    }
    for (int i = 0; i < count(); ++i)
        if (m_tabs[size_t(i)].id == keep && i != m_current)
            setCurrent(i);
    if (kept > 0)
        setNotice(tr("Kept %n tabs with unsaved changes.", nullptr, kept));
}

void Workflows::closeAllTabs() {
    refreshUnsaved();
    int unsavedTabs = 0;
    for (const Tab &tab : m_tabs)
        unsavedTabs += tab.unsaved ? 1 : 0;
    if (unsavedTabs > 0)
        emit closeAllConfirmationRequested(unsavedTabs);
    else
        discardAndCloseAll();
}

// As the last tab's close does: removeTab leaves one fresh, empty workflow, and drops each tab's recovery copy.
void Workflows::discardAndCloseAll() {
    while (count() > 1)
        removeTab(count() - 1);
    removeTab(0);
}

void Workflows::discardAndClose(int index) { removeTab(index); }

void Workflows::saveAndClose(int index) {
    if (index < 0 || index >= count())
        return;
    if (m_tabs[size_t(index)].path.isEmpty()) {
        setCurrent(index);
        m_closeAfterSave = m_tabs[size_t(index)].id;
        emit saveAsRequested();
        return;
    }
    if (saveTab(index, m_tabs[size_t(index)].path))
        removeTab(index);
}

// --- recovery ---

void Workflows::recover() {
    const QVariantList entries = m_recoverable;
    m_recoverable.clear();
    emit recoverableChanged();
    QStringList failed;
    QString aside;
    for (const QVariant &value : entries) {
        const QVariantMap entry = value.toMap();
        const QString id = entry.value("id").toString();
        const Result<Document> document = m_recovery.read(id);
        if (!document.ok()) {
            // Never deleted: it may be the only copy of the work, readable by a later version.
            failed << entry.value("title").toString();
            if (const QString folder = m_recovery.setAside(id); !folder.isEmpty())
                aside = folder;
            continue;
        }
        // The copy keeps its id, so the next 30-second write replaces it in place.
        addTab(document.value(), entry.value("path").toString(), entry.value("title").toString(), true, true, id);
        analyze(document.value(), m_tabs[size_t(m_current)].title);
    }
    if (!failed.isEmpty())
        setNotice(aside.isEmpty()
                      ? tr("These recovery copies could not be read: %1").arg(failed.join(", "))
                      : tr("These recovery copies could not be read and were kept in %1: %2").arg(aside, failed.join(", ")));
}

void Workflows::discardRecovery() {
    for (const QVariant &value : m_recoverable)
        m_recovery.remove(value.toMap().value("id").toString());
    m_recoverable.clear();
    emit recoverableChanged();
}

void Workflows::dropRecoveryCopies() {
    m_timer.stop();
    for (Tab &tab : m_tabs) {
        m_recovery.remove(tab.id);
        tab.recoveryHash.clear();
    }
}

void Workflows::writeRecoveryCopies() {
    refreshUnsaved();
    for (int i = 0; i < count(); ++i) {
        Tab &tab = m_tabs[size_t(i)];
        if (!tab.unsaved) {
            if (!tab.recoveryHash.isEmpty()) {
                m_recovery.remove(tab.id);
                tab.recoveryHash.clear();
            }
            continue;
        }
        const Document document = documentOf(i);
        const QString hash = contentHash(document);
        if (hash == tab.recoveryHash)
            continue;
        const Errors errors = m_recovery.write(tab.id, document, tab.path, tab.title);
        if (hasErrors(errors))
            qWarning("Fugue: recovery copy of %s not written: %s", qPrintable(tab.title),
                     qPrintable(firstMessage(errors)));
        else
            tab.recoveryHash = hash;
    }
}

// --- trust ---

void Workflows::allowPendingTrust() {
    for (const QVariant &value : m_pendingTrust) {
        const QVariantMap use = value.toMap();
        const Errors errors = m_trust.trust(use.value("id").toString(), use.value("version").toString());
        if (hasErrors(errors))
            setNotice(tr("Could not remember your choice: %1").arg(firstMessage(errors)));
    }
    dismissPendingTrust();
}

void Workflows::dismissPendingTrust() {
    for (const QVariant &entry : std::as_const(m_pendingTrust))
        m_trustDismissed.insert(entry.toMap().value("id").toString() + QLatin1Char(' ')
                                + entry.toMap().value("version").toString());
    m_pendingTrust.clear();
    emit pendingTrustChanged();
}

bool Workflows::isTrusted(const QString &packId, const QString &version) const {
    return m_trust.isTrusted(packId, version);
}

void Workflows::refreshNodeTypes() {
    if (m_current < 0)
        return;
    const Document shown = shownDocument();
    // The banner follows the shown workflow's missing nodes (a pack removed adds some, one
    // installed takes them away), unless it shows another line; a line dismissed comes back
    // only when what is missing changed.
    const QString missing = missingNotice(shown, m_tabs[size_t(m_current)].title, &m_offer);
    if (missing != m_missingNotice && (m_notice.isEmpty() || m_notice == m_missingNotice))
        setNotice(missing);
    m_missingNotice = missing;
    emit noticeChanged(); // the offer
    // A model folder added or the setting changed (the registry and settings reload end here): what is on disk may cover a missing model now.
    m_onDiskKey.clear();
    m_choice->modelsMayHaveChanged();
    m_choice->check(ModelChoice::Check::ModelsChanged);

    // The question follows what is installed and trusted now: a pack trusted meanwhile (the
    // Packs page, the CLI) is not asked about, nor one at a version no longer installed; a pack
    // that came, or a new version of one, is asked about (6.6, 6.7), unless the person said
    // "not now" to that version already.
    QVariantList still;
    for (const QVariant &entry : std::as_const(m_pendingTrust)) {
        const QString id = entry.toMap().value("id").toString(), version = entry.toMap().value("version").toString();
        const QString installed = m_registry->packVersion(id);
        if (!m_trust.isTrusted(id, version) && (installed.isEmpty() || m_registry->hasPackVersion(id, version)))
            still << entry;
    }
    bool changed = still.size() != m_pendingTrust.size();
    m_pendingTrust = still;
    QList<PackUse> asked;
    for (const PackUse &use : m_trust.untrusted(shown, *m_registry))
        if (!m_trustDismissed.contains(use.id + QLatin1Char(' ') + use.version))
            asked << use;
    changed |= addPendingTrust(asked);
    if (changed)
        emit pendingTrustChanged();
}

// --- commands ---

void registerFileCommands(CommandRegistry &registry, Workflows &workflows) {
    registry.setHandler("file.new", [&workflows] { workflows.newWorkflow(); });
    registry.setHandler("file.open", [&workflows] { emit workflows.openDialogRequested(); });
    registry.setHandler("file.save", [&workflows] { workflows.save(); });
    // The Templates dialog is the window's (Main.qml), like Settings.
    const auto browseTemplates = [&registry] { emit registry.windowRequested(QStringLiteral("file.templates")); };
    if (!registry.contains("file.templates")) {
        CommandSpec spec;
        spec.id = QStringLiteral("file.templates");
        spec.description = QT_TRANSLATE_NOOP("Command", "Browse templates");
        spec.versus = QStringLiteral("same");
        const QString problem = registry.add(spec, browseTemplates);
        if (!problem.isEmpty())
            qWarning("Fugue: %s", qPrintable(problem));
    } else {
        registry.setHandler("file.templates", browseTemplates);
    }
    registry.setHandler("file.save-as", [&workflows] { emit workflows.saveAsRequested(); });
    // The tab menu's commands act on the current tab (a right click on a tab makes it current).
    const auto onCurrent = [&registry](const char *id, const char *description, const char *label,
                                       std::function<void()> run) {
        const QString key = QString::fromLatin1(id);
        if (!registry.contains(key)) {
            CommandSpec spec;
            spec.id = key;
            spec.description = QString::fromUtf8(description);
            spec.label = QString::fromUtf8(label);
            spec.context = QStringLiteral("app");
            spec.versus = QStringLiteral("added");
            if (const QString problem = registry.add(spec, std::move(run)); !problem.isEmpty())
                qWarning("Fugue: %s", qPrintable(problem));
        } else {
            registry.setHandler(key, std::move(run));
        }
    };
    onCurrent("file.rename", QT_TRANSLATE_NOOP("Command", "Rename the workflow"),
              QT_TRANSLATE_NOOP("Command", "Rename…"), [&workflows] { emit workflows.tabRenameRequested(workflows.current()); });
    onCurrent("file.duplicate", QT_TRANSLATE_NOOP("Command", "Duplicate the workflow into a new tab"),
              QT_TRANSLATE_NOOP("Command", "Duplicate"), [&workflows] { workflows.duplicateTab(workflows.current()); });
    onCurrent("file.close-others", QT_TRANSLATE_NOOP("Command", "Close the other workflows"),
              QT_TRANSLATE_NOOP("Command", "Close other tabs"), [&workflows] { workflows.closeOtherTabs(workflows.current()); });
    onCurrent("tab.close-all", QT_TRANSLATE_NOOP("Command", "Close every tab (asks once if some have unsaved changes)"),
              QT_TRANSLATE_NOOP("Command", "Close all tabs…"), [&workflows] { workflows.closeAllTabs(); });
    onCurrent("file.tab-color", QT_TRANSLATE_NOOP("Command", "Pick the tab's color"),
              QT_TRANSLATE_NOOP("Command", "Tab color…"), [&workflows] { emit workflows.tabColorRequested(workflows.current()); });
    registry.setEnabledWhen("file.close-others", [&workflows] { return workflows.count() > 1; });
    registry.setHandler("file.export-share", [&workflows] { emit workflows.exportShareRequested(); });
    registry.setHandler("file.export-compiled", [&workflows] { emit workflows.exportCompiledRequested(); });
    if (!registry.contains("file.open-clipboard")) {
        CommandSpec spec;
        spec.id = QStringLiteral("file.open-clipboard");
        spec.description = QT_TRANSLATE_NOOP("Command", "Open the workflow on the clipboard");
        spec.versus = QStringLiteral("added");
        const QString problem = registry.add(spec, [&workflows] { workflows.openClipboard(); });
        if (!problem.isEmpty())
            qWarning("Fugue: %s", qPrintable(problem));
    } else {
        registry.setHandler("file.open-clipboard", [&workflows] { workflows.openClipboard(); });
    }
}

} // namespace fugue
