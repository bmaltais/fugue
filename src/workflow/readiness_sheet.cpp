#include "workflow/readiness_sheet.h"

#include <QDir>
#include <QJsonArray>
#include <QMetaObject>
#include <QPointer>
#include <QUrl>

#include "engine/file_hashes.h"
#include "engine/model_catalog.h"
#include "engine/model_library.h"
#include "engine/pack_loader.h"
#include "engine/paths.h"
#include "engine/registry.h"
#include "engine/settings.h"
#include "workflow/model_downloads.h"

namespace fugue {

namespace {

QString stateName(RowState state) {
    switch (state) {
    case RowState::Waiting: return QStringLiteral("waiting");
    case RowState::Running: return QStringLiteral("running");
    case RowState::Done: return QStringLiteral("done");
    case RowState::Failed: return QStringLiteral("failed");
    case RowState::Skipped: return QStringLiteral("skipped");
    }
    return {};
}

QString packSummaryLine(const ReadinessPack &row, const QString &from) {
    const PackSummary &p = row.pack;
    QStringList parts;
    if (!from.isEmpty() && row.installs())
        parts << ReadinessSheet::tr("from %1").arg(from);
    parts << ReadinessSheet::tr("declares %1").arg(p.permissions.isEmpty() ? ReadinessSheet::tr("nothing") : p.permissions.join(QStringLiteral(", ")));
    if (!p.secrets.isEmpty())
        parts << ReadinessSheet::tr("reads secrets %1").arg(p.secrets.join(QStringLiteral(", ")));
    if (row.hasEnvironment())
        parts << ReadinessSheet::tr("runs %1").arg(displayCommand(p.setupCommand));
    return parts.join(QStringLiteral(" · "));
}

// How often a download's progress changes the view at most.
constexpr int kProgressMs = 200;

QString joinParts(const QStringList &parts) {
    if (parts.size() < 2)
        return parts.join(QString());
    return parts.mid(0, parts.size() - 1).join(QStringLiteral(", ")) + ReadinessSheet::tr(" and ") + parts.last();
}

} // namespace

ReadinessSheet::ReadinessSheet(const Registry *registry, QString trustPath, Request request, QObject *parent)
    : QObject(parent), m_registry(registry), m_trustPath(std::move(trustPath)), m_request(std::move(request)) {
    m_publishTimer.setSingleShot(true);
    m_publishTimer.setInterval(kProgressMs);
    connect(&m_publishTimer, &QTimer::timeout, this, &ReadinessSheet::publish);
    m_ownRoot = ModelLibrary::current(m_registry).roots().value(0);
    publish(); // QML reads the view before anything is shown
}

ReadinessSheet::~ReadinessSheet() {
    if (m_thread) {
        m_cancel = true;
        m_thread->wait();
        delete m_thread;
    }
}

QString ReadinessSheet::rootTitle(const QString &root, const QString &own) {
    if (root == own)
        return tr("Fugue's models folder");
    const QString home = QDir::homePath();
    return root.startsWith(home + QLatin1Char('/')) ? QLatin1Char('~') + root.mid(home.size()) : root;
}

ModelLibrary ReadinessSheet::library() const {
    Settings settings = currentSettings();
    if (!m_picked.isEmpty() && !ModelLibrary::roots(settings).contains(m_picked)) {
        settings.modelPaths << m_picked;
        settings.modelFolderKinds.insert(m_picked, ModelLibrary::kLayout);
    }
    ModelLibrary library(ModelLibrary::roots(settings), m_registry, ModelLibrary::rootKinds(settings));
    QStringList inherited;
    for (const SearchPath &root : ModelLibrary::taggedRoots(settings))
        if (root.inherited)
            inherited << root.path;
    library.setInheritedRoots(inherited);
    if (!settings.downloadFolder.isEmpty())
        library.setDownloadRoot(QDir::cleanPath(expandHome(settings.downloadFolder)));
    if (!m_destination.isEmpty())
        library.setDownloadRoot(m_destination);
    return library;
}

ReadinessPlan ReadinessSheet::buildPlan() const {
    FileHashes hashes(FileHashes::defaultPath());
    TrustStore trust(m_trustPath);
    const ModelCatalog catalog = ModelCatalog::load();
    ReadinessEnv env{library()};
    env.registry = m_registry;
    env.hashes = &hashes;
    env.trust = &trust;
    env.packsDir = packsDir();
    env.catalog = &catalog;
    return buildReadinessPlan(m_request(), env);
}

ReadinessPlan ReadinessSheet::currentPlan() const { return buildPlan(); }

void ReadinessSheet::rebuild() {
    m_ownRoot = library().roots().value(0);
    m_plan = buildPlan();
    m_states = QList<RowState>(m_plan.rowCount(), RowState::Waiting);
    m_details = QStringList(m_plan.rowCount());
    m_fractions = QList<double>(m_plan.rowCount(), 0.0);
}

void ReadinessSheet::useOnDisk(const QString &hash) {
    if (m_phase == QLatin1String("review") && m_open)
        emit useRequested(hash);
}

void ReadinessSheet::refresh() {
    if (!m_open || m_thread || m_phase != QLatin1String("review"))
        return;
    rebuild();
    publish();
}

void ReadinessSheet::show() {
    if (m_thread)
        return;
    m_destination.clear();
    m_picked.clear();
    m_question.clear();
    rebuild();
    m_phase = QStringLiteral("review");
    m_message.clear();
    m_retry.clear();
    m_ok = false;
    m_open = true;
    publish();
    emit shown();
}

void ReadinessSheet::close() {
    if (m_thread)
        return;
    m_open = false;
    publish();
}

void ReadinessSheet::cancel() {
    if (m_thread)
        m_cancel = true;
    else
        close();
}

void ReadinessSheet::changeDestination(const QString &root) {
    if (m_thread || m_phase != QLatin1String("review") || !m_plan.destinations.contains(root))
        return;
    m_destination = root; // written at the yes
    rebuild();
    publish();
}

void ReadinessSheet::pickDestination(const QString &folder) {
    if (m_thread || m_phase != QLatin1String("review"))
        return;
    const QUrl url(folder);
    const QString typed = url.isLocalFile() ? url.toLocalFile() : folder;
    const QString path = QFileInfo(typed).canonicalFilePath(); // links and ".." resolved before it is compared or kept
    if (path.isEmpty() || !QFileInfo(path).isDir())
        return;
    m_picked.clear();
    for (const QString &root : ModelLibrary::roots(currentSettings()))
        if (QFileInfo(root).canonicalFilePath() == path)
            m_destination = root;
    if (m_destination.isEmpty() || QFileInfo(m_destination).canonicalFilePath() != path) {
        m_picked = path;
        m_destination = path;
    }
    rebuild();
    publish();
}

void ReadinessSheet::start() {
    if (m_thread || m_phase != QLatin1String("review") || m_plan.empty() || m_plan.lowSpace())
        return;
    // The yes also makes the folder choice the settings': a picked folder joins the model folders (ComfyUI layout) and the\n    // folder becomes download_folder. Nothing was written before this.
    if (!savePlacement())
        return;
    m_cancel = false;
    m_phase = QStringLiteral("running");
    publish();
    ReadinessPlan plan = m_plan;
    // The run plans the pack's own model types against a copy of the node types, made here: the window's registry is filled
    // again on the window's thread (RegistryReloader) as soon as the pack lands in the packs folder, while the run would be
    // reading it on its own. Kept until the next start(), which is refused while this run is on.
    m_runTypes = m_registry ? std::make_unique<const Registry>(*m_registry) : nullptr;
    plan.registry = m_runTypes.get();
    const QString trustPath = m_trustPath;
    QPointer<ReadinessSheet> self(this);
    std::atomic<bool> *cancel = &m_cancel; // the destructor waits for the thread
    // The packs' git and setup commands, and the downloads' event loop, block: a thread of their own.
    m_thread = QThread::create([self, plan, trustPath, cancel] {
        TrustStore trust(trustPath);
        FileHashes hashes(FileHashes::defaultPath());
        ReadinessRun run(packsDir(), &trust, &hashes);
        const auto tell = [self](std::function<void(ReadinessSheet &)> what) {
            if (self)
                QMetaObject::invokeMethod(self.data(), [self, what] { if (self) what(*self); }, Qt::QueuedConnection);
        };
        run.onRow = [tell](int row, RowState state, const QString &detail) {
            tell([=](ReadinessSheet &sheet) { sheet.setRow(row, state, detail); });
        };
        run.onProgress = [tell](int row, qint64 got, qint64 all) {
            tell([=](ReadinessSheet &sheet) {
                if (row >= 0 && row < sheet.m_fractions.size()) {
                    sheet.m_fractions[row] = all > 0 ? double(got) / double(all) : -1;
                    sheet.publishSoon();
                }
            });
        };
        run.cancelled = [cancel] { return cancel->load(); };
        run.allowMore = [self](const PackSummary &pack, const QStringList &permissions) {
            return self && self->askMore(pack, permissions);
        };
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        tell([outcome](ReadinessSheet &sheet) { sheet.ended(outcome); });
    });
    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_thread = nullptr;
    });
    m_thread->start();
}

bool ReadinessSheet::savePlacement() {
    Errors errors;
    const Settings settings = currentSettings();
    if (!m_picked.isEmpty() && !ModelLibrary::roots(settings).contains(m_picked)) {
        QStringList paths = settings.modelPaths;
        paths << m_picked;
        errors += changeSetting(settingsPath(), QStringLiteral("model_paths"), QJsonArray::fromStringList(paths));
        QJsonObject kinds;
        for (auto it = settings.modelFolderKinds.cbegin(); it != settings.modelFolderKinds.cend(); ++it)
            kinds.insert(it.key(), it.value());
        kinds.insert(m_picked, ModelLibrary::kLayout);
        errors += changeSetting(settingsPath(), QStringLiteral("model_folder_kinds"), kinds);
    }
    if (!m_destination.isEmpty() && !hasErrors(errors)) {
        // Fugue's own folder is the default: no key.
        const bool own = m_destination == ModelLibrary::roots(settings).value(0);
        errors += changeSetting(settingsPath(), QStringLiteral("download_folder"), own ? QJsonValue(QJsonValue::Undefined) : QJsonValue(m_destination));
    }
    if (hasErrors(errors)) {
        m_message = tr("Could not remember the folder: %1").arg(errors.first().message);
        publish();
        return false;
    }
    m_message.clear();
    return true;
}

// From the running thread: asks, on the sheet, whether a pack fetched after the yes may have the access it declares, and waits.
bool ReadinessSheet::askMore(const PackSummary &pack, const QStringList &permissions) {
    {
        QMutexLocker lock(&m_mutex);
        m_answer = -1;
    }
    const QVariantMap question{{"pack", pack.name.isEmpty() ? pack.id : pack.name},
                               {"text", tr("%1 also wants %2 access. Allow it, or skip this pack?").arg(pack.name.isEmpty() ? pack.id : pack.name, permissions.join(QStringLiteral(", ")))}};
    QMetaObject::invokeMethod(this, [this, question] { m_question = question; publish(); }, Qt::QueuedConnection);
    QMutexLocker lock(&m_mutex);
    while (m_answer < 0 && !m_cancel.load())
        m_asked.wait(&m_mutex, 100);
    const bool allowed = m_answer == 1;
    lock.unlock();
    QMetaObject::invokeMethod(this, [this] { m_question.clear(); publish(); }, Qt::QueuedConnection);
    return allowed;
}

void ReadinessSheet::answer(bool allow) {
    QMutexLocker lock(&m_mutex);
    m_answer = allow ? 1 : 0;
    m_asked.wakeAll();
}

void ReadinessSheet::setRow(int row, RowState state, const QString &detail) {
    if (row < 0 || row >= m_states.size())
        return;
    m_states[row] = state;
    m_details[row] = detail;
    if (state == RowState::Done)
        m_fractions[row] = 1;
    publish();
}

void ReadinessSheet::ended(const ReadinessOutcome &outcome) {
    m_phase = QStringLiteral("finished");
    m_ok = outcome.ok && m_plan.problems.isEmpty();
    m_retry = outcome.retry;
    if (outcome.ok && !m_plan.problems.isEmpty())
        m_message = tr("Done, but not everything could be made ready: the lines above say what is left.");
    else if (outcome.ok)
        m_message = tr("Everything is ready.");
    else if (!outcome.cancelled && outcome.failedRow < 0)
        m_message = tr("Some rows were skipped, so it is not everything: they are listed below.");
    else if (outcome.cancelled)
        m_message = tr("Stopped. Nothing half-done is left: what is not done yet is listed below.");
    else {
        QStringList lines;
        for (const Error &error : outcome.errors)
            lines << (error.hint.isEmpty() ? error.message : error.message + QLatin1Char(' ') + error.hint);
        m_message = tr("It stopped at the first failure: %1").arg(lines.join(QLatin1Char(' ')));
    }
    // One click, then done: when everything is ready the sheet goes away (no "done" sheet to close); a failure stays open.
    if (m_ok)
        m_open = false;
    publish();
    if (outcome.packsChanged)
        emit packsModified();
    emit finished(outcome.ok);
    if (m_ok)
        emit succeeded();
}

bool ReadinessSheet::waitIdle(int msec) {
    if (!m_thread)
        return true;
    QThread *thread = m_thread;
    return thread->wait(msec);
}

void ReadinessSheet::publishSoon() {
    if (!m_publishTimer.isActive())
        m_publishTimer.start();
}

void ReadinessSheet::publish() {
    m_publishTimer.stop(); // what it would show is in this view
    QVariantMap view;
    QStringList parts;
    if (const int n = m_plan.installCount())
        parts << tr("install and trust %n packs", nullptr, n);
    if (const int n = m_plan.trustOnlyCount())
        parts << tr("trust %n packs", nullptr, n);
    if (const int n = m_plan.environmentCount())
        parts << tr("set up %n environments", nullptr, n);
    if (!m_plan.models.isEmpty()) {
        const int n = int(m_plan.models.size());
        parts << (m_plan.downloadBytes > 0
                      ? tr("download %n models, %1", nullptr, n)
                            .arg((m_plan.unknownSizes ? tr("at least ") : QString()) + ModelDownloads::sizeText(m_plan.downloadBytes))
                      : tr("download %n models", nullptr, n));
    }
    view["headline"] = parts.isEmpty() ? (m_plan.problems.isEmpty() ? tr("There is nothing to get ready.") : tr("Nothing here can be made ready from this machine."))
                                       : tr("Fugue will %1.").arg(joinParts(parts));
    QVariantList packs;
    for (int i = 0; i < m_plan.packs.size(); ++i) {
        const ReadinessPack &row = m_plan.packs.at(i);
        const PackSummary &p = row.pack;
        QString from = p.source.location;
        if (!p.source.ref.isEmpty())
            from += tr(" at %1").arg(p.source.ref);
        packs << QVariantMap{{"id", p.id}, {"name", p.name.isEmpty() ? p.id : p.name}, {"version", p.version}, {"install", row.installs()}, {"pending", row.pending},
                             {"summary", row.pending ? tr("From %1 · details after the download").arg(from.isEmpty() ? tr("its recorded source") : from)
                                                     : packSummaryLine(row, from)},
                             {"source", from}, {"runtime", p.runtimeKinds.join(QStringLiteral(", "))},
                             {"permissions", p.permissions.isEmpty() ? tr("nothing (network, files, GPU)") : p.permissions.join(QStringLiteral(", "))},
                             {"secrets", p.secrets.join(QStringLiteral(", "))},
                             {"setup", row.hasEnvironment() ? displayCommand(p.setupCommand) : QString()},
                             {"setupNote", p.setupDescription},
                             {"setupSize", p.setupSize >= 0 ? ModelDownloads::sizeText(p.setupSize) : QString()},
                             {"state", stateName(m_states.value(i))}, {"detail", m_details.value(i)}};
    }
    view["packs"] = packs;
    QVariantList models;
    for (int i = 0; i < m_plan.models.size(); ++i) {
        const ReadinessModel &m = m_plan.models.at(i);
        const int row = int(m_plan.packs.size()) + i;
        const QString hash = m.entry.value(QStringLiteral("hash")).toString();
        models << QVariantMap{{"hash", hash},
                              {"use", m_useSource && !hash.isEmpty() ? m_useSource(hash) : QVariantMap()},
                              {"name", m.name}, {"size", m.size >= 0 ? ModelDownloads::sizeText(m.size) : tr("size not recorded")},
                              {"host", m.host}, {"license", m.license}, {"licenseUrl", m.licenseUrl},
                              {"folder", m.deferred ? tr("its folder, once the pack is installed") : m.folder},
                              {"typeFrom", !m.deferred ? QString() : m.typeFrom.isEmpty() ? tr("type from a pack in this list") : tr("type from %1").arg(m.typeFrom)},
                              {"state", stateName(m_states.value(row))}, {"detail", m_details.value(row)},
                              {"fraction", m_fractions.value(row)}};
    }
    view["models"] = models;
    const QString &own = m_ownRoot; // not the settings again for every progress step
    view["destination"] = m_plan.destination;
    view["destinationTitle"] = rootTitle(m_plan.destination, own);
    QVariantList destinations;
    for (const QString &root : m_plan.destinations)
        destinations << QVariantMap{{"path", root}, {"title", rootTitle(root, own)}};
    view["destinations"] = destinations;
    view["hasModels"] = !m_plan.models.isEmpty();
    view["freeText"] = m_plan.freeBytes >= 0 ? tr("%1 free").arg(ModelDownloads::sizeText(m_plan.freeBytes)) : QString();
    view["sizeText"] = m_plan.downloadBytes > 0 ? ModelDownloads::sizeText(m_plan.downloadBytes) : QString();
    view["lowSpace"] = m_plan.lowSpace();
    QStringList problems;
    for (const Error &problem : m_plan.problems)
        problems << (problem.hint.isEmpty() ? problem.message : problem.message + QLatin1Char(' ') + problem.hint);
    view["problems"] = problems;
    view["empty"] = m_plan.empty();
    view["question"] = m_question;
    view["ok"] = m_ok;
    view["message"] = m_message;
    view["retry"] = m_retry;
    m_view = view;
    emit changed();
}

} // namespace fugue
