#include "workflow/readiness_sheet.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QJsonArray>
#include <QMetaObject>
#include <QPointer>
#include <QUrl>

#include <algorithm>

#include "engine/sandbox.h"
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

// An optional model's row (#594): the sheet lists it apart, with "Not now | Download <size>" as its choice.
QVariantMap optionalView(const ReadinessModel &m, bool on, const QString &state, double fraction, const QString &detail) {
    return QVariantMap{{"hash", m.entry.value(QStringLiteral("hash")).toString()},
                       {"name", m.name}, {"size", m.size >= 0 ? ModelDownloads::sizeText(m.size) : ReadinessSheet::tr("size not recorded")},
                       {"sizeKnown", m.size >= 0}, {"host", m.host}, {"license", m.license}, {"folder", m.deferred ? ReadinessSheet::tr("its folder, once the pack is installed") : m.folder},
                       {"on", on}, {"state", state}, {"fraction", fraction}, {"detail", detail}};
}

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

ReadinessPlan ReadinessSheet::buildPlan() const { return buildPlan(true); }

// `withChoices`: the optional models the person turned on in the open sheet count as wanted. The Needs panel reads the plan with
// none of them: a choice made in a sheet that was closed without its yes must not change what the panel says it needs.
ReadinessPlan ReadinessSheet::buildPlan(bool withChoices) const {
    FileHashes hashes(FileHashes::defaultPath());
    TrustStore trust(m_trustPath);
    const ModelCatalog catalog = ModelCatalog::load();
    ReadinessEnv env{library()};
    env.registry = m_registry;
    env.hashes = &hashes;
    env.trust = &trust;
    env.packsDir = packsDir();
    env.catalog = &catalog;
    ReadinessRequest request = m_request();
    if (withChoices)
        request.optionalChosen = m_optionalOn;
    return buildReadinessPlan(request, env);
}

ReadinessPlan ReadinessSheet::currentPlan() const { return buildPlan(false); }

// The models that have a `.part` kept by an earlier stop: looked at when rows change, not on every progress tick.
void ReadinessSheet::refreshPartial() {
    m_partial.clear();
    for (const ReadinessModel &m : std::as_const(m_plan.models))
        if (const QFileInfo part(m.target.path + QStringLiteral(".part")); !m.deferred && part.isFile() && !part.isSymLink() && part.size() > 0)
            m_partial.insert(m.target.path, part.size());
}

void ReadinessSheet::deletePartial(const QString &hash) {
    for (int i = 0; i < m_plan.models.size(); ++i) {
        const ReadinessModel &m = m_plan.models.at(i);
        if (m.deferred || m.entry.value(QStringLiteral("hash")).toString() != hash)
            continue;
        const int row = int(m_plan.packs.size()) + i;
        if (m_phase == QLatin1String("running") && m_states.value(row) == RowState::Running)
            return; // that download runs
        const QString path = m.target.path + QStringLiteral(".part");
        // Another Fugue may be downloading it: the part's lock is its writer's.
        QLockFile lock(path + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            return;
        if (const QFileInfo info(path); info.isFile() || info.isSymLink())
            QFile::remove(path); // a link goes itself, never what it points to
        lock.unlock();
        refreshPartial();
        publish();
        return;
    }
}

void ReadinessSheet::rebuild() {
    m_ownRoot = library().roots().value(0);
    m_plan = buildPlan();
    refreshPartial();
    m_states = QList<RowState>(m_plan.rowCount(), RowState::Waiting);
    m_details = QStringList(m_plan.rowCount());
    m_fractions = QList<double>(m_plan.rowCount(), 0.0);
    // The files of the same weights: each model that has one starts on "Use my file" (the sheet is the explicit yes; nothing happens before it).
    m_offers.clear();
    for (const ReadinessModel &m : m_plan.models) {
        if (m.optional)
            continue; // the person asked for the download: no second question
        const QString hash = m.entry.value(QStringLiteral("hash")).toString();
        const QVariantMap offer = m_useSource && !hash.isEmpty() ? m_useSource(hash) : QVariantMap();
        if (offer.isEmpty())
            continue;
        m_offers.insert(hash, offer);
        if (!m_seenOffers.contains(hash)) {
            m_seenOffers.insert(hash);
            m_useDisk.insert(hash);
        }
    }
    m_useDisk.removeIf([this](const QString &hash) { return !m_offers.contains(hash); });
}

bool ReadinessSheet::usesFile(const ReadinessModel &model) const {
    return m_useDisk.contains(model.entry.value(QStringLiteral("hash")).toString());
}

qint64 ReadinessSheet::downloadBytesNow(int *count, bool *unknown) const {
    qint64 bytes = 0;
    int n = 0;
    bool noSize = false;
    for (const ReadinessModel &m : m_plan.models) {
        if (usesFile(m))
            continue;
        ++n;
        if (m.size >= 0)
            bytes += m.size;
        else
            noSize = true;
    }
    if (count)
        *count = n;
    if (unknown)
        *unknown = noSize;
    return bytes;
}

bool ReadinessSheet::lowSpaceNow() const {
    return m_plan.freeBytes >= 0 && downloadBytesNow(nullptr, nullptr) > m_plan.freeBytes;
}

void ReadinessSheet::setChoice(const QString &hash, bool useDisk) {
    if (m_phase != QLatin1String("review") || !m_open || m_usesPending || !m_offers.contains(hash))
        return;
    if (m_useDisk.contains(hash) == useDisk && !m_rowErrors.contains(hash) && m_message.isEmpty())
        return;
    if (useDisk)
        m_useDisk.insert(hash);
    else
        m_useDisk.remove(hash);
    m_rowErrors.remove(hash);
    m_message.clear(); // a failure said for the choice before this one
    publish();
}

void ReadinessSheet::setOptional(const QString &hash, bool on) {
    if (m_phase != QLatin1String("review") || !m_open || m_usesPending || hash.isEmpty() || m_optionalOn.contains(hash) == on)
        return;
    if (on)
        m_optionalOn.insert(hash);
    else
        m_optionalOn.remove(hash);
    m_message.clear();
    rebuild();
    publish();
}

void ReadinessSheet::chooseAll(bool useDisk) {
    if (m_phase != QLatin1String("review") || !m_open || m_usesPending)
        return;
    m_useDisk = useDisk ? QSet<QString>(m_offers.keyBegin(), m_offers.keyEnd()) : QSet<QString>();
    m_rowErrors.clear();
    m_message.clear();
    publish();
}

void ReadinessSheet::setReading(const QVariantMap &reading) {
    const bool rowsMoved = reading.keys() != m_reading.keys(); // a row starts or stops reading: shown at once; a percent only soon
    m_reading = reading;
    if (!m_open)
        return;
    if (rowsMoved)
        publish();
    else
        publishSoon();
}

void ReadinessSheet::usesFinished(const QStringList &applied, const QStringList &failed, int token) {
    Q_UNUSED(applied)
    if (!m_usesPending || token == 0 || token != m_usesToken)
        return; // another surface's choice (the Needs panel's), not the answer to this sheet's yes
    m_usesPending = false;
    m_usesToken = 0;
    for (const QString &hash : failed) {
        m_useDisk.remove(hash); // Try again offers the row with Download
        m_rowErrors.insert(hash, tr("Your file is not used. Download is chosen for it now."));
    }
    const QString said = m_message; // Workflows said which file, just before
    rebuild();
    m_message = said;
    if (!failed.isEmpty()) {
        m_ok = false;
        publish();
        return;
    }
    if (m_plan.empty()) { // everything was a file on disk: nothing to run, the sheet has done its work
        m_open = false;
        publish();
        emit succeeded();
        return;
    }
    runPlan();
}

void ReadinessSheet::say(const QString &message) {
    if (!m_open || m_thread || m_phase != QLatin1String("review"))
        return;
    m_message = message;
    m_stopped = false;
    m_ok = false;
    publish();
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
    m_useDisk.clear();
    m_optionalOn.clear();
    m_seenOffers.clear();
    m_rowErrors.clear();
    m_usesPending = false;
    m_usesToken = 0;
    rebuild();
    m_phase = QStringLiteral("review");
    m_message.clear();
    m_retry.clear();
    m_stopped = false;
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
    if (m_thread) {
        m_cancel = true;
        return;
    }
    // The choices are discarded with the sheet: the document is as it was, and a file being read is not applied.
    if (m_usesPending) {
        m_usesPending = false;
        const int token = std::exchange(m_usesToken, 0); // its answer, when it comes, is no one's
        emit useCancelled(token);
    }
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
    if (m_thread || m_usesPending || m_phase != QLatin1String("review") || m_plan.empty() || lowSpaceNow())
        return;
    // The yes: the files chosen go in first (the window reads them and sets them in the document), then the rest of the plan runs.
    QStringList chosen;
    for (const ReadinessModel &m : m_plan.models)
        if (usesFile(m))
            chosen << m.entry.value(QStringLiteral("hash")).toString();
    if (!chosen.isEmpty()) {
        m_usesPending = true;
        m_usesToken = ++m_lastToken;
        m_message.clear();
        m_rowErrors.clear();
        publish();
        emit useRequested(chosen, m_usesToken);
        return;
    }
    runPlan();
}

void ReadinessSheet::runPlan() {
    if (m_thread || m_phase != QLatin1String("review"))
        return;
    // A model set to Use my file is never downloaded, whatever order the answers came in: it leaves what is run.
    if (m_plan.models.removeIf([this](const ReadinessModel &m) { return usesFile(m); })) {
        m_states = QList<RowState>(m_plan.rowCount(), RowState::Waiting);
        m_details = QStringList(m_plan.rowCount());
        m_fractions = QList<double>(m_plan.rowCount(), 0.0);
        publish();
    }
    if (m_plan.empty() || lowSpaceNow())
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
                               {"text", tr("%1 also asks for: %2. Allow it, or skip this pack?").arg(pack.name.isEmpty() ? pack.id : pack.name, permissions.join(QStringLiteral(", ")))}};
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
    if (state != RowState::Running)
        refreshPartial(); // a failed or stopped download has left its `.part`
    publish();
}

void ReadinessSheet::ended(const ReadinessOutcome &outcome) {
    m_phase = QStringLiteral("finished");
    m_ok = outcome.ok && m_plan.problems.isEmpty();
    m_stopped = outcome.cancelled && !outcome.ok;
    m_retry = outcome.retry;
    if (outcome.ok && !m_plan.problems.isEmpty())
        m_message = tr("Done, but not everything could be made ready: the lines above say what is left.");
    else if (outcome.ok)
        m_message = tr("Everything is ready.");
    else if (!outcome.cancelled && outcome.failedRow < 0)
        m_message = tr("Some rows were skipped, so it is not everything: they are listed below.");
    else if (outcome.cancelled)
    {
        // What the stop did, only for what was running; the rows say what is not done.
        QStringList said{tr("Stopped.")};
        if (outcome.stoppedDownload)
            said << tr("Downloads keep what they had.");
        if (outcome.stoppedInstall)
            said << tr("The install that was running is undone.");
        if (outcome.stoppedSetup)
            said << tr("The setup that was running may have to be run again.");
        m_message = said.join(QLatin1Char(' '));
    }
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
    int downloads = 0;
    bool unknownSize = false;
    const qint64 downloadBytes = downloadBytesNow(&downloads, &unknownSize);
    const int uses = int(m_plan.models.size()) - downloads;
    const bool onlyUses = parts.isEmpty() && downloads == 0 && uses > 0;
    // Kept `.part` files shorten what is left (a stop, a failure, an earlier session): the headline counts that, and the primary says Resume.
    int paused = 0;
    qint64 keptBytes = 0;
    for (const ReadinessModel &m : m_plan.models)
        if (!usesFile(m) && !m.deferred && m_partial.contains(m.target.path)) {
            ++paused;
            keptBytes += m.size >= 0 ? std::min<qint64>(m.size, m_partial.value(m.target.path)) : 0;
        }
    if (downloads > 0) {
        const QString size = (unknownSize ? tr("at least ") : QString()) + ModelDownloads::sizeText(downloadBytes - keptBytes);
        parts << (downloadBytes <= 0 ? tr("download %n models", nullptr, downloads)
                  : paused > 0 ? tr("download %n models, %1 left", nullptr, downloads).arg(size)
                               : tr("download %n models, %1", nullptr, downloads).arg(size));
    }
    if (uses > 0 && !onlyUses)
        parts << tr("use %n files you already have", nullptr, uses);
    const bool onlyOptional = parts.isEmpty() && downloads == 0 && uses == 0 && m_plan.problems.isEmpty() && !m_plan.optionalModels.isEmpty();
    view["headline"] = onlyOptional ? tr("Nothing is needed to run. Models only another branch uses can be downloaded below.")
                       : onlyUses ? tr("Nothing to download: Fugue will use %n files you already have.", nullptr, uses)
                       : parts.isEmpty() ? (m_plan.problems.isEmpty() ? tr("There is nothing to get ready.") : tr("Nothing here can be made ready from this machine."))
                                         : tr("Fugue will %1.").arg(joinParts(parts));
    view["hasDownloads"] = downloads > 0;
    view["resumes"] = paused > 0; // the retry button says Resume while a kept part is left
    view["usesCount"] = uses;
    view["busy"] = m_usesPending;
    // The one primary: Install while anything is installed, trusted, set up or downloaded; "Use my file" when only the person's files are left.
    const bool morethanFiles = !m_plan.packs.isEmpty() || m_plan.installCount() > 0 || m_plan.trustOnlyCount() > 0 || m_plan.environmentCount() > 0 || downloads > 0;
    view["primary"] = morethanFiles || uses == 0 ? tr("Install") : tr("Use my %n files", nullptr, uses);
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
                             // What a setup's sandbox lets it do (6.7.1): the network, the host's.
                             {"setupLines", row.hasEnvironment() ? sandbox::needsLines({}, true) : QStringList()},
                             {"setupSize", p.setupSize >= 0 ? ModelDownloads::sizeText(p.setupSize) : QString()},
                             {"state", stateName(m_states.value(i))}, {"detail", m_details.value(i)}};
    }
    view["packs"] = packs;
    QVariantList models;
    QVariantList optionalRows;
    for (int i = 0; i < m_plan.models.size(); ++i) {
        const ReadinessModel &m = m_plan.models.at(i);
        const int row = int(m_plan.packs.size()) + i;
        if (m.optional) { // chosen (#594): listed in its own section, where it can be turned off again
            optionalRows << optionalView(m, true, stateName(m_states.value(row)), m_fractions.value(row), m_details.value(row));
            continue;
        }
        const QString hash = m.entry.value(QStringLiteral("hash")).toString();
        const bool reading = m_reading.contains(hash);
        const bool disk = m_useDisk.contains(hash);
        QString state = stateName(m_states.value(row));
        double fraction = m_fractions.value(row);
        if (reading) {
            state = QStringLiteral("reading");
            fraction = m_reading.value(hash).toInt() >= 0 ? m_reading.value(hash).toInt() / 100.0 : -1;
        }
        const bool partial = !m.deferred && !disk && state != QLatin1String("running") && state != QLatin1String("reading") && state != QLatin1String("done") && m_partial.contains(m.target.path);
        models << QVariantMap{{"hash", hash},
                              {"use", m_offers.value(hash)},
                              {"choice", m_offers.contains(hash) ? (disk ? QStringLiteral("disk") : QStringLiteral("download")) : QString()},
                              {"name", m.name}, {"size", m.size >= 0 ? ModelDownloads::sizeText(m.size) : tr("size not recorded")}, {"sizeKnown", m.size >= 0},
                              {"host", m.host}, {"license", m.license}, {"licenseUrl", m.licenseUrl},
                              {"folder", m.deferred ? tr("its folder, once the pack is installed") : m.folder},
                              {"typeFrom", !m.deferred ? QString() : m.typeFrom.isEmpty() ? tr("type from a pack in this list") : tr("type from %1").arg(m.typeFrom)},
                              // A download that stopped keeps its `.part`, and the next one resumes it (#418).
                              {"partial", partial},
                              {"paused", partial && state != QLatin1String("failed")}, // the row's word: Paused, not "Not done"
                              {"partialSize", partial ? ModelDownloads::sizeText(m_partial.value(m.target.path)) : QString()},
                              {"partialText", !partial ? QString()
                                              : m.size > 0 ? tr("%1 of %2").arg(ModelDownloads::sizeText(m_partial.value(m.target.path)), ModelDownloads::sizeText(m.size))
                                                           : ModelDownloads::sizeText(m_partial.value(m.target.path))},
                              {"state", state}, {"detail", m_rowErrors.value(hash, m_details.value(row))},
                              {"error", m_rowErrors.value(hash)},
                              {"fraction", fraction}};
    }
    view["models"] = models;
    for (const ReadinessModel &m : m_plan.optionalModels)
        optionalRows << optionalView(m, false, QStringLiteral("waiting"), 0, QString());
    std::sort(optionalRows.begin(), optionalRows.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("name").toString() < b.toMap().value("name").toString();
    });
    view["optionalModels"] = optionalRows;
    const QString &own = m_ownRoot; // not the settings again for every progress step
    view["destination"] = m_plan.destination;
    view["destinationTitle"] = rootTitle(m_plan.destination, own);
    QVariantList destinations;
    for (const QString &root : m_plan.destinations)
        destinations << QVariantMap{{"path", root}, {"title", rootTitle(root, own)}};
    view["destinations"] = destinations;
    view["hasModels"] = !m_plan.models.isEmpty();
    view["freeText"] = m_plan.freeBytes >= 0 ? tr("%1 free").arg(ModelDownloads::sizeText(m_plan.freeBytes)) : QString();
    view["sizeText"] = downloadBytes > 0 ? ModelDownloads::sizeText(downloadBytes) : QString();
    view["lowSpace"] = lowSpaceNow();
    QStringList problems;
    for (const Error &problem : m_plan.problems)
        problems << (problem.hint.isEmpty() ? problem.message : problem.message + QLatin1Char(' ') + problem.hint);
    view["problems"] = problems;
    view["empty"] = m_plan.empty(); // nothing to run: the optional models alone are not what Install does
    view["question"] = m_question;
    view["ok"] = m_ok;
    view["message"] = m_message;
    view["stopped"] = m_stopped; // the message is a plain note, not an error
    view["retry"] = m_retry;
    m_view = view;
    emit changed();
}

} // namespace fugue
