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

QString joinParts(const QStringList &parts) {
    if (parts.size() < 2)
        return parts.join(QString());
    return parts.mid(0, parts.size() - 1).join(QStringLiteral(", ")) + ReadinessSheet::tr(" and ") + parts.last();
}

} // namespace

ReadinessSheet::ReadinessSheet(const Registry *registry, QString trustPath, Request request, QObject *parent)
    : QObject(parent), m_registry(registry), m_trustPath(std::move(trustPath)), m_request(std::move(request)) {
    publish(); // QML reads the view before anything is shown
}

ReadinessSheet::~ReadinessSheet() {
    stopPreview();
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

bool ReadinessSheet::needed() const {
    FileHashes hashes(FileHashes::defaultPath());
    TrustStore trust(m_trustPath);
    ReadinessEnv env{ModelLibrary::current(m_registry)};
    env.registry = m_registry;
    env.hashes = &hashes;
    env.trust = &trust;
    env.packsDir = packsDir();
    return !buildReadinessPlan(m_request(), env).empty();
}

void ReadinessSheet::rebuild() {
    FileHashes hashes(FileHashes::defaultPath());
    TrustStore trust(m_trustPath);
    const ModelCatalog catalog = ModelCatalog::load();
    ReadinessEnv env{ModelLibrary::current(m_registry)};
    env.registry = m_registry;
    env.hashes = &hashes;
    env.trust = &trust;
    env.packsDir = packsDir();
    env.catalog = &catalog;
    env.deferGit = true; // a clone never runs on the window's thread
    stopPreview();
    ++m_generation;
    m_plan = buildReadinessPlan(m_request(), env);
    m_states = QList<RowState>(m_plan.rowCount(), RowState::Waiting);
    m_details = QStringList(m_plan.rowCount());
    m_fractions = QList<double>(m_plan.rowCount(), 0.0);
    if (m_plan.resolving())
        startPreview();
}

void ReadinessSheet::stopPreview() {
    if (!m_preview)
        return;
    m_previewCancel = true;
    m_preview->wait();
    delete m_preview;
    m_preview = nullptr;
}

void ReadinessSheet::startPreview() {
    QList<QPair<QString, PackSource>> work;
    for (const ReadinessPack &row : std::as_const(m_plan.packs))
        if (row.pending)
            work.append({row.pack.id, row.pack.source});
    m_previewCancel = false;
    const int generation = m_generation;
    const QString folder = packsDir();
    QPointer<ReadinessSheet> self(this);
    std::atomic<bool> *cancel = &m_previewCancel; // the destructor waits for the thread
    m_preview = QThread::create([self, work, generation, folder, cancel] {
        for (const auto &[id, source] : work) {
            PackPrompts prompts;
            prompts.cancelled = [cancel] { return cancel->load(); };
            const Result<PackSummary> summary = previewPack(source, folder, prompts);
            if (cancel->load() || !self)
                return;
            const bool ok = summary.ok();
            const PackSummary value = ok ? summary.value() : PackSummary();
            const Errors errors = ok ? Errors() : summary.errors();
            QMetaObject::invokeMethod(self.data(), [self, generation, id = id, ok, value, errors] {
                if (self)
                    self->resolved(generation, id, ok, value, errors);
            }, Qt::QueuedConnection);
        }
    });
    m_preview->start();
}

void ReadinessSheet::resolved(int generation, const QString &id, bool ok, const PackSummary &summary, const Errors &errors) {
    if (generation != m_generation)
        return;
    for (int i = 0; i < m_plan.packs.size(); ++i) {
        if (!m_plan.packs.at(i).pending || m_plan.packs.at(i).pack.id != id)
            continue;
        if (ok) {
            m_plan.packs[i].pack = summary;
            m_plan.packs[i].pending = false;
        } else {
            // It cannot be installed: a line saying why, and the row goes (the rest of the list is unchanged).
            for (Error e : errors) {
                e.severity = Error::Severity::Warning;
                m_plan.problems << e;
            }
            m_plan.packs.removeAt(i);
            m_states.removeAt(i);
            m_details.removeAt(i);
            m_fractions.removeAt(i);
        }
        break;
    }
    m_plan.recount();
    publish();
}

void ReadinessSheet::show() {
    if (m_thread)
        return;
    rebuild();
    m_phase = QStringLiteral("review");
    m_message.clear();
    m_retry.clear();
    m_ok = false;
    m_open = true;
    publish();
}

void ReadinessSheet::close() {
    if (m_thread)
        return;
    stopPreview(); // a pack being read is dropped with its staging folder
    ++m_generation;
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
    const ModelLibrary library = ModelLibrary::current(m_registry);
    // Fugue's own folder is the default: no key.
    changeSetting(settingsPath(), QStringLiteral("download_folder"),
                  root == library.roots().value(0) ? QJsonValue(QJsonValue::Undefined) : QJsonValue(root));
    rebuild();
    publish();
}

void ReadinessSheet::pickDestination(const QString &folder) {
    if (m_thread || m_phase != QLatin1String("review"))
        return;
    const QUrl url(folder);
    const QString path = QDir::cleanPath(url.isLocalFile() ? url.toLocalFile() : folder);
    if (path.isEmpty() || !QFileInfo(path).isDir())
        return;
    const Settings settings = currentSettings();
    // A folder Fugue downloads into must be one it searches: it joins the model folders, in ComfyUI's layout.
    if (!ModelLibrary::roots(settings).contains(path)) {
        QStringList paths = settings.modelPaths;
        paths << path;
        if (hasErrors(changeSetting(settingsPath(), QStringLiteral("model_paths"), QJsonArray::fromStringList(paths))))
            return;
        QJsonObject kinds;
        for (auto it = settings.modelFolderKinds.cbegin(); it != settings.modelFolderKinds.cend(); ++it)
            kinds.insert(it.key(), it.value());
        kinds.insert(path, ModelLibrary::kLayout);
        changeSetting(settingsPath(), QStringLiteral("model_folder_kinds"), kinds);
    }
    rebuild();
    changeDestination(path);
}

void ReadinessSheet::start() {
    if (m_thread || m_phase != QLatin1String("review") || m_plan.empty() || m_plan.resolving())
        return;
    m_cancel = false;
    m_phase = QStringLiteral("running");
    publish();
    const ReadinessPlan plan = m_plan;
    const QString trustPath = m_trustPath;
    QPointer<ReadinessSheet> self(this);
    // The packs' git and setup commands, and the downloads' event loop, block: a thread of their own.
    m_thread = QThread::create([self, plan, trustPath] {
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
                    sheet.publish();
                }
            });
        };
        run.cancelled = [self] { return !self || self->m_cancel.load(); };
        const ReadinessOutcome outcome = runReadinessPlan(plan, run);
        tell([outcome](ReadinessSheet &sheet) { sheet.ended(outcome); });
    });
    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_thread = nullptr;
    });
    m_thread->start();
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
    m_ok = outcome.ok;
    m_retry = outcome.retry;
    if (outcome.ok)
        m_message = tr("Everything is ready.");
    else if (outcome.cancelled)
        m_message = tr("Stopped. Nothing half-done is left: what is not done yet is listed below.");
    else {
        QStringList lines;
        for (const Error &error : outcome.errors)
            lines << (error.hint.isEmpty() ? error.message : error.message + QLatin1Char(' ') + error.hint);
        m_message = tr("It stopped at the first failure: %1").arg(lines.join(QLatin1Char(' ')));
    }
    publish();
    if (outcome.packsChanged)
        emit packsModified();
    emit finished(outcome.ok);
}

bool ReadinessSheet::waitIdle(int msec) {
    if (!m_thread)
        return true;
    QThread *thread = m_thread;
    return thread->wait(msec);
}

void ReadinessSheet::publish() {
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
    view["headline"] = parts.isEmpty() ? tr("There is nothing to get ready.") : tr("Fugue will %1.").arg(joinParts(parts));
    QVariantList packs;
    for (int i = 0; i < m_plan.packs.size(); ++i) {
        const ReadinessPack &row = m_plan.packs.at(i);
        const PackSummary &p = row.pack;
        QString from = p.source.location;
        if (!p.source.ref.isEmpty())
            from += tr(" at %1").arg(p.source.ref);
        packs << QVariantMap{{"id", p.id}, {"name", p.name.isEmpty() ? p.id : p.name}, {"version", p.version}, {"install", row.installs()}, {"pending", row.pending},
                             {"source", from}, {"runtime", p.runtimeKinds.join(QStringLiteral(", "))},
                             {"permissions", p.permissions.isEmpty() ? tr("nothing (network, files, GPU)") : p.permissions.join(QStringLiteral(", "))},
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
        models << QVariantMap{{"name", m.name}, {"size", m.size >= 0 ? ModelDownloads::sizeText(m.size) : tr("size not recorded")},
                              {"host", m.host}, {"license", m.license}, {"licenseUrl", m.licenseUrl}, {"folder", m.folder},
                              {"state", stateName(m_states.value(row))}, {"detail", m_details.value(row)},
                              {"fraction", m_fractions.value(row)}};
    }
    view["models"] = models;
    const ModelLibrary library = ModelLibrary::current(m_registry);
    const QString own = library.roots().value(0);
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
    view["resolving"] = m_plan.resolving();
    view["ok"] = m_ok;
    view["message"] = m_message;
    view["retry"] = m_retry;
    m_view = view;
    emit changed();
}

} // namespace fugue
