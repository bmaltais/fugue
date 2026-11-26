#include "settings/model_setup.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QStorageInfo>

#include <algorithm>
#include <thread>

#include "engine/fugue_home.h"
#include "engine/model_download.h"
#include "engine/model_library.h"
#include "engine/paths.h"
#include "engine/project.h"
#include "engine/registry.h"
#include "engine/settings.h"
#include "settings/settings_model.h"
#include "workflow/readiness_sheet.h"

namespace fugue {

namespace {

constexpr qint64 kLowSpace = 20LL * 1000 * 1000 * 1000; // "Only %1 free there." under 20 GB

QString tilde(const QString &path) {
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    return path.startsWith(home + QLatin1Char('/')) ? QLatin1Char('~') + path.mid(home.size()) : path;
}

// Free bytes where a folder is, or would be made: the nearest existing parent's disk. -1 when unknown.
qint64 freeBytesAt(const QString &path, const FinderEnv &env) {
    // Never a network or automount (a statfs there can mount it or hang the window), nor a path that leads into one by a link.
    const QString safe = finderResolve(path, env);
    if (safe.isEmpty())
        return -1;
    QString dir = safe;
    while (!dir.isEmpty() && !QFileInfo(dir).isDir()) {
        const QString parent = QFileInfo(dir).absolutePath();
        if (parent == dir)
            break;
        dir = parent;
    }
    const QStorageInfo info(dir);
    return info.isValid() && info.isReady() ? info.bytesAvailable() : -1;
}

bool hasModelFile(const QString &dir) {
    QDirIterator it(dir, {QStringLiteral("*.safetensors"), QStringLiteral("*.gguf"), QStringLiteral("*.ckpt"), QStringLiteral("*.pt"),
                          QStringLiteral("*.pth"), QStringLiteral("*.bin"), QStringLiteral("*.sft")},
                    QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext();
}

// The people's word for what a type holds, for the top three of a row; empty for a type the row does not name.
QString kindText(const QString &type, int n) {
    if (type == QLatin1String("fugue/lora"))
        return QCoreApplication::translate("fugue::ModelSetup", "%n LoRAs", nullptr, n);
    if (type == QLatin1String("fugue/model"))
        return QCoreApplication::translate("fugue::ModelSetup", "%n checkpoints", nullptr, n);
    if (type == QLatin1String("fugue/diffusion-model"))
        return QCoreApplication::translate("fugue::ModelSetup", "%n diffusion models", nullptr, n);
    if (type == QLatin1String("fugue/text-encoder"))
        return QCoreApplication::translate("fugue::ModelSetup", "%n text encoders", nullptr, n);
    if (type == QLatin1String("fugue/vae"))
        return QCoreApplication::translate("fugue::ModelSetup", "%n VAEs", nullptr, n);
    if (type == QLatin1String("fugue/upscale-model"))
        return QCoreApplication::translate("fugue::ModelSetup", "%n upscale models", nullptr, n);
    return {};
}

QString toolTitle(const QString &tool) {
    // One literal for each so the catalog check sees them.
    if (tool == QLatin1String("ComfyUI"))
        return QCoreApplication::translate("fugue::ModelSetup", "ComfyUI");
    if (tool == QLatin1String("ComfyUI portable"))
        return QCoreApplication::translate("fugue::ModelSetup", "ComfyUI portable");
    if (tool == QLatin1String("ComfyUI Desktop"))
        return QCoreApplication::translate("fugue::ModelSetup", "ComfyUI Desktop");
    if (tool == QLatin1String("Stability Matrix"))
        return QCoreApplication::translate("fugue::ModelSetup", "Stability Matrix");
    if (tool == QLatin1String("Automatic1111"))
        return QCoreApplication::translate("fugue::ModelSetup", "Automatic1111");
    if (tool == QLatin1String("Forge"))
        return QCoreApplication::translate("fugue::ModelSetup", "Forge");
    if (tool == QLatin1String("SwarmUI"))
        return QCoreApplication::translate("fugue::ModelSetup", "SwarmUI");
    if (tool == QLatin1String("InvokeAI"))
        return QCoreApplication::translate("fugue::ModelSetup", "InvokeAI");
    return QCoreApplication::translate("fugue::ModelSetup", "Model folder");
}

// The (kind, text) pairs a folder can be set to hold, as Settings > Folders offers them.
QVariantList kindChoices() {
    return {QVariantMap{{"value", ModelLibrary::kLayout}, {"text", QCoreApplication::translate("fugue::ModelSetup", "Model folders (ComfyUI layout)")}},
            QVariantMap{{"value", "fugue/lora"}, {"text", QCoreApplication::translate("fugue::ModelSetup", "LoRAs")}},
            QVariantMap{{"value", "fugue/vae"}, {"text", QCoreApplication::translate("fugue::ModelSetup", "VAEs")}},
            QVariantMap{{"value", "fugue/diffusion-model"}, {"text", QCoreApplication::translate("fugue::ModelSetup", "Diffusion models")}},
            QVariantMap{{"value", "fugue/text-encoder"}, {"text", QCoreApplication::translate("fugue::ModelSetup", "Text encoders")}},
            QVariantMap{{"value", "fugue/model"}, {"text", QCoreApplication::translate("fugue::ModelSetup", "Checkpoints")}}};
}

} // namespace

ModelSetup::ModelSetup(SettingsModel *settings) : QObject(settings), m_settings(settings), m_receiver(std::make_shared<Receiver>()) {
    m_receiver->target = this;
    // The default finder reads nothing of the window: the job carries the types, the mounts and the cancel flag as values.
    m_finder = [](const FinderJob &job, const std::function<void(const FoundFolder &)> &found) {
        FinderEnv env = job.env;
        env.cancel = job.cancel.get();
        for (const QString &path : job.describe)
            found(describeModelFolder(job.types, path, env));
        if (job.locations.isEmpty())
            return FindResult();
        return findModelFolders(job.types, job.locations, env, job.skip, found, job.offered);
    };
}

// No wait: a worker stuck in a stat of a slow or hung drive must never hold the window or the quit. It holds the cancel flag
// itself, answers to nobody when this is gone, and the process ends without it.
ModelSetup::~ModelSetup() {
    *m_cancel = true;
    QMutexLocker lock(&m_receiver->mutex);
    m_receiver->target = nullptr; // no worker posts to a freed object
}

void ModelSetup::setFinder(Finder finder, bool runInline) {
    m_finder = std::move(finder);
    m_inline = runInline;
}

const Registry *ModelSetup::types() const { return m_settings->types(); }

FinderEnv ModelSetup::env() const { return m_envSet ? m_env : FinderEnv::system(); }

bool ModelSetup::inProject() const {
    return !fugueHome().isEmpty() && QFileInfo::exists(projectManifestPath(fugueHome()));
}

bool ModelSetup::shouldOffer() const {
    if (inProject() || !m_settings->modelsSetup().isEmpty())
        return false;
    if (!currentSettings().modelPaths.isEmpty())
        return false;
    const QString own = finderResolve(dataDir() + QStringLiteral("/models"), env()); // empty: it leads into a network mount: not looked at
    return own.isEmpty() || !hasModelFile(own);
}

bool ModelSetup::maybeOffer() {
    if (m_open || !shouldOffer())
        return false;
    show(true);
    return true;
}

QString ModelSetup::ownRoot() const { return ModelLibrary::roots(currentSettings()).value(0); }

void ModelSetup::show(bool firstRun) {
    if (m_open)
        return;
    m_generation++;
    *m_cancel = true;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_open = true;
    m_firstRun = firstRun;
    m_phase = QStringLiteral("consent");
    m_looking = false;
    m_capped = false;
    m_rows.clear();
    m_unticked.clear();
    m_message.clear();
    // A rerun starts from the download folder the person has: it is kept unless they choose another.
    const Settings settings = currentSettings();
    m_downloadChosen = !settings.downloadFolder.isEmpty();
    m_download = m_settings->downloadFolder();
    m_envNow = env();
    m_locations = finderLocations(m_envNow); // an lstat each; nothing below them is listed, no link followed
    emit changed();
}

void ModelSetup::setLocationChecked(const QString &id, bool checked) {
    if (checked)
        m_unticked.removeAll(id);
    else if (!m_unticked.contains(id))
        m_unticked << id;
    emit changed();
}

bool ModelSetup::canSearch() const {
    return std::any_of(m_locations.begin(), m_locations.end(), [this](const FinderLocation &l) { return !m_unticked.contains(l.id); });
}

QVariantList ModelSetup::locations() const {
    QVariantList list;
    for (const FinderLocation &location : m_locations) {
        QString hint;
        if (location.id.startsWith(QLatin1String("probe:")))
            hint = tr("Usual install folder");
        else if (location.id == QLatin1String("home"))
            hint = tr("Your home folder, %n levels deep", nullptr, location.depth);
        else
            hint = tr("Each drive in it, %n levels deep", nullptr, location.depth);
        list.append(QVariantMap{{"id", location.id}, {"display", location.display}, {"hint", hint}, {"checked", !m_unticked.contains(location.id)}});
    }
    return list;
}

// The folders already in use, as rows still being counted: the worker counts them (a slow drive is its wait, not the window's).
void ModelSetup::addInUse() {
    const Settings settings = currentSettings();
    const QHash<QString, QString> pinned = ModelLibrary::rootKinds(settings);
    for (const SearchPath &root : ModelLibrary::taggedRoots(settings)) {
        if (root.path == ownRoot())
            continue;
        Row row;
        row.path = root.path;
        row.inUse = true;
        row.inherited = root.inherited;
        row.on = true;
        row.busy = true;
        row.kind = pinned.value(root.path);
        row.found.path = root.path;
        row.found.tool = QStringLiteral("Model folder");
        m_rows.append(row);
    }
}

ModelSetup::FinderJob ModelSetup::makeJob() const {
    FinderJob job;
    job.env = env();
    if (types())
        job.types = FinderTypes::from(*types()); // a copy made here, on this thread: a registry reload cannot pull it from a walk
    job.cancel = m_cancel;
    job.offered = m_locations;
    return job;
}

void ModelSetup::search() {
    if (m_phase != QLatin1String("consent"))
        return;
    QList<FinderLocation> ticked;
    for (const FinderLocation &location : std::as_const(m_locations))
        if (!m_unticked.contains(location.id))
            ticked << location;
    m_rows.clear();
    addInUse();
    m_phase = QStringLiteral("looking");
    m_looking = true;
    m_capped = false;
    begin(ticked);
    emit changed();
}

void ModelSetup::searchAgain() {
    if (!m_open || m_phase == QLatin1String("consent"))
        return;
    m_generation++;
    *m_cancel = true;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_looking = false;
    m_capped = false;
    m_phase = QStringLiteral("consent");
    m_rows.clear();
    m_message.clear();
    emit changed();
}

void ModelSetup::begin(const QList<FinderLocation> &locations) {
    ++m_generation;
    FinderJob job = makeJob();
    job.locations = locations;
    for (const Row &row : std::as_const(m_rows)) {
        job.skip << row.path;
        if (row.inUse)
            job.describe << row.path;
    }
    run(std::move(job), true);
}

// Runs a job on a thread of its own that is never waited for: it captures values only (never `this`), tells this object through a
// weak pointer, and a stalled one is simply left behind.
void ModelSetup::run(FinderJob job, bool search) {
    const int generation = m_generation;
    const Finder finder = m_finder;
    if (m_inline) {
        const FindResult result = finder(job, [this, generation](const FoundFolder &folder) { found(generation, folder); });
        if (search)
            finished(generation, result.capped);
        return;
    }
    // The worker posts to this object only through the receiver: ~ModelSetup clears it under the lock, so a post never meets a freed object.
    const std::shared_ptr<Receiver> receiver = m_receiver;
    std::thread([receiver, finder, job = std::move(job), generation, search] {
        const auto post = [&receiver](const std::function<void(ModelSetup *)> &call) {
            QMutexLocker lock(&receiver->mutex);
            if (receiver->target)
                QMetaObject::invokeMethod(receiver->target, [target = receiver->target, call] { call(target); }, Qt::QueuedConnection);
        };
        const FindResult result = finder(job, [&post, generation](const FoundFolder &folder) {
            post([generation, folder](ModelSetup *self) { self->found(generation, folder); });
        });
        if (search)
            post([generation, result](ModelSetup *self) { self->finished(generation, result.capped); });
    }).detach();
}

ModelSetup::Row ModelSetup::makeRow(const FoundFolder &folder) const {
    Row row;
    row.path = folder.path;
    row.found = folder;
    row.writable = folder.writable;
    row.kind = folder.kind; // decided on the worker (a found candidate is a layout by kindOf()'s own rule)
    // Ticked: found with models; InvokeAI sorts its own way (off); an install with no models yet is off; a network drive is
    // listed but never ticked for the person.
    row.on = folder.tool != QLatin1String("InvokeAI") && folder.models > 0 && !folder.network;
    return row;
}

void ModelSetup::found(int generation, const FoundFolder &folder) {
    if (generation != m_generation || !m_open)
        return;
    for (Row &row : m_rows) {
        if (row.path != folder.path)
            continue;
        if (row.busy) {
            // The count of a row that was waiting (in use, or just picked): what it holds was decided on the worker.
            row.busy = false;
            row.found = folder;
            row.writable = folder.writable;
            if (row.inUse) {
                if (row.kind.isEmpty())
                    row.kind = folder.kind;
            } else {
                // A picked folder is a layout only when kindOf()'s rule says so (a folder of LoRAs with a `Lora/` inside stays one of LoRAs);
                // a network drive is not looked into: the person says what it holds.
                row.kind = folder.kind;
                row.found.layout = row.kind == ModelLibrary::kLayout;
                // The same folder by another path: the second is dropped.
                for (const Row &other : std::as_const(m_rows))
                    if (&other != &row && !other.busy && !folder.canonical.isEmpty() && other.found.canonical == folder.canonical) {
                        m_message = tr("That folder is already in the list.");
                        m_rows.removeIf([&row](const Row &r) { return &r == &row; });
                        break;
                    }
            }
            emit changed();
        }
        return;
    }
    // A candidate the search found that is a folder already in use (the same one by its resolved path) is not listed twice.
    if (!folder.canonical.isEmpty())
        for (const Row &other : std::as_const(m_rows))
            if (!other.busy && other.found.canonical == folder.canonical)
                return;
    m_rows.append(makeRow(folder));
    emit changed();
}

void ModelSetup::finished(int generation, bool capped) {
    if (generation != m_generation || !m_open)
        return;
    m_looking = false;
    m_capped = capped;
    m_phase = QStringLiteral("results");
    // A folder in use that the finder did not count (a fake finder): shown as it is. A pick still being counted keeps waiting.
    for (Row &row : m_rows)
        row.busy = row.busy && !row.inUse;
    emit changed();
}

QString ModelSetup::addFolder(const QVariant &value) {
    QString path = value.typeId() == QMetaType::QUrl ? value.toUrl().toLocalFile() : value.toString();
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    path = QDir::cleanPath(expandHome(path.trimmed()));
    if (!path.isEmpty() && QDir::isRelativePath(path))
        path = QDir::cleanPath(QDir::homePath() + QLatin1Char('/') + path); // typed without a leading / or ~: from the home folder
    // The person's own pick, one stat, component by component: a network or automount (or a link into one) is taken as a folder
    // without a stat, and is listed, not counted.
    const FinderEnv now = env();
    bool network = false;
    for (const QString &mount : now.networkMounts)
        network = network || path == mount || path.startsWith(mount + QLatin1Char('/'));
    const QString real = network ? path : finderResolve(path, now); // empty: a link leads into a network or automount
    if (path.isEmpty() || (!network && !real.isEmpty() && !QFileInfo(real).isDir()))
        return tr("%1 is not a folder.").arg(tilde(path));
    for (const Row &row : std::as_const(m_rows))
        if (row.path == path)
            return tr("That folder is already in the list.");
    m_message.clear();
    if (m_phase == QLatin1String("consent")) {
        // A pick with no search: the results, with what is in use and this folder.
        m_rows.clear();
        addInUse();
        m_phase = QStringLiteral("results");
        m_capped = false;
        FinderJob inUse = makeJob();
        for (const Row &row : std::as_const(m_rows))
            inUse.describe << row.path;
        if (!inUse.describe.isEmpty())
            run(std::move(inUse), false);
    }
    // The count is the worker's; the row waits, busy, and the answer comes back to found().
    Row row;
    row.path = path;
    row.found.path = path;
    row.found.tool = QStringLiteral("Model folder");
    row.on = true;
    row.busy = true;
    m_rows.append(row);
    FinderJob job = makeJob();
    job.describe << path;
    run(std::move(job), false);
    emit changed();
    return {};
}

void ModelSetup::setRowOn(const QString &path, bool on) {
    for (Row &row : m_rows)
        if (row.path == path && !row.inherited)
            row.on = on;
    emit changed();
}

void ModelSetup::setRowKind(const QString &path, const QString &kind) {
    for (Row &row : m_rows)
        if (row.path == path)
            row.kind = kind;
    emit changed();
}

QVariantList ModelSetup::kinds() const { return kindChoices(); }

bool ModelSetup::nothing() const {
    return m_phase == QLatin1String("results") && !m_looking && m_rows.isEmpty();
}

// In use first, then by the models they hold (the most first), then the ones with none.
QList<int> ModelSetup::order() const {
    QList<int> index;
    for (int i = 0; i < m_rows.size(); ++i)
        index << i;
    auto rank = [this](int i) { return m_rows.at(i).inUse ? 0 : (m_rows.at(i).found.models > 0 || m_rows.at(i).found.network ? 1 : 2); };
    std::stable_sort(index.begin(), index.end(), [&](int a, int b) {
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        return m_rows.at(a).found.models > m_rows.at(b).found.models;
    });
    return index;
}

QVariantList ModelSetup::rows() const {
    QVariantList list;
    for (const int i : order()) {
        const Row &row = m_rows.at(i);
        const FoundFolder &f = row.found;
        // Line 2: the size, then what it holds (the three most, and "and more").
        QStringList parts;
        QString counted;
        if (row.busy) {
            counted = tr("Counting its models");
        } else if (f.network) {
            counted = tr("On a network drive: not counted");
        } else if (f.models == 0 && !row.inUse) {
            counted = tr("No models yet");
        } else {
            QStringList kindsText;
            QList<std::pair<int, QString>> sorted;
            for (auto it = f.counts.begin(); it != f.counts.end(); ++it)
                sorted.append({it.value(), it.key()});
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
            bool more = false;
            for (const auto &[n, type] : std::as_const(sorted)) {
                const QString text = kindText(type, n);
                if (!text.isEmpty() && kindsText.size() < 3)
                    kindsText << text;
                else
                    more = true;
            }
            QString what = kindsText.join(QStringLiteral(", "));
            if (what.isEmpty())
                what = f.countCapped ? tr("%1+ models").arg(f.models) : tr("%n models", nullptr, f.models);
            else if (more)
                what = tr("%1 and more").arg(what);
            if (!f.counts.isEmpty() && f.countCapped)
                what = tr("%1+ models").arg(f.models) + QStringLiteral(" · ") + what;
            counted = (f.size > 0 ? sizeText(f.size) + QStringLiteral(" · ") : QString()) + what;
        }
        if (f.countCapped)
            counted += QStringLiteral(" · ") + tr("Counting stopped: slow drive");
        const QString note = f.tool == QLatin1String("InvokeAI") && !f.network
                                 ? tr("InvokeAI sorts models its own way: Fugue adds the folders it can tell apart.") : QString();
        if (!row.writable && !row.inherited)
            counted += QStringLiteral(" · ") + tr("Read only");
        QString tooltip = row.path;
        if (!f.alsoReached.isEmpty())
            tooltip += QStringLiteral("\n") + tr("Also reached as %1").arg(f.alsoReached.join(QStringLiteral(", ")));
        if (!row.writable && !row.inherited)
            tooltip += QStringLiteral("\n") + tr("Read only: Fugue can use it but not download into it.");
        list.append(QVariantMap{{"path", row.path},
                                {"display", tilde(row.path)},
                                {"title", toolTitle(f.tool)},
                                {"hint2", counted},
                                {"note", note},
                                {"tooltip", tooltip},
                                {"on", row.on},
                                {"inUse", row.inUse},
                                {"busy", row.busy},
                                {"inherited", row.inherited},
                                {"readOnly", !row.writable},
                                {"needsKind", row.kind.isEmpty() && !row.inUse && !row.busy && f.tool != QLatin1String("InvokeAI")},
                                {"kind", row.kind},
                                {"models", f.models},
                                {"capped", f.countCapped}});
    }
    return list;
}

// The downloads' default: the largest (by the size of its models) of the picked folders that can be written in and takes any model,
// else Fugue's own. Never switched for lack of room: the dropdown says how much is free.
QString ModelSetup::defaultDownload() const {
    QString best = ownRoot();
    qint64 biggest = -1;
    for (const Row &row : m_rows) {
        if (row.inherited || !row.on || row.busy || !row.writable || row.kind != ModelLibrary::kLayout || row.found.tool == QLatin1String("InvokeAI"))
            continue;
        if (slowToWriteTo(row))
            continue; // a network or removable drive is a choice the person makes, never the default
        if (row.found.size > biggest) {
            biggest = row.found.size;
            best = row.path;
        }
    }
    return best;
}

// A network drive, an automount, or a removable drive (under /run/media or /media): never picked for the person.
bool ModelSetup::slowToWriteTo(const Row &row) const {
    if (row.found.network)
        return true;
    for (const QString &prefix : {QStringLiteral("/run/media/"), QStringLiteral("/media/")})
        if (row.path.startsWith(prefix))
            return true;
    for (const QString &mount : m_envNow.networkMounts)
        if (row.path == mount || row.path.startsWith(mount + QLatin1Char('/')))
            return true;
    return false;
}

QVariantList ModelSetup::choices() const {
    QVariantList list;
    auto entry = [this](const QString &path, const QString &title) {
        const qint64 free = m_free ? m_free(path) : freeBytesAt(path, m_envNow);
        const QString freeText = free >= 0 ? QCoreApplication::translate("fugue::ModelSetup", "%1 free").arg(sizeText(free)) : QString();
        return QVariantMap{{"value", path},
                           {"title", title},
                           {"text", freeText.isEmpty() ? title : title + QStringLiteral(" · ") + freeText},
                           {"free", freeText},
                           {"freeBytes", free},
                           {"low", free >= 0 && free < kLowSpace}};
    };
    const QString own = ownRoot();
    list.append(entry(own, ReadinessSheet::rootTitle(own, own)));
    QList<const Row *> picked;
    for (const Row &row : m_rows)
        if (!row.inherited && row.on && row.writable && row.kind == ModelLibrary::kLayout && row.found.tool != QLatin1String("InvokeAI"))
            picked << &row;
    std::stable_sort(picked.begin(), picked.end(), [](const Row *a, const Row *b) { return a->found.size > b->found.size; });
    for (const Row *row : std::as_const(picked))
        list.append(entry(row->path, tilde(row->path)));
    // The download folder the person has (set in Settings or by the CLI) that is not one of these rows is kept, not replaced.
    if (m_downloadChosen && m_download != own) {
        const bool listed = std::any_of(list.begin(), list.end(), [this](const QVariant &c) { return c.toMap().value("value").toString() == m_download; });
        const bool leftOff = std::any_of(m_rows.begin(), m_rows.end(), [this](const Row &r) { return r.path == m_download && !r.on; });
        if (!listed && !leftOff) // its row is on but not a layout (a single-kind root), or it has none: still the person's choice
            list.append(entry(m_download, tilde(m_download)));
    }
    return list;
}

// What the dropdown shows: the person's choice while it is still offered, else the default.
QString ModelSetup::download() const {
    if (m_downloadChosen)
        for (const QVariant &item : choices())
            if (item.toMap().value("value").toString() == m_download)
                return m_download;
    return defaultDownload();
}

QString ModelSetup::downloadHint() const {
    const QString now = download();
    for (const QVariant &item : choices()) {
        const QVariantMap choice = item.toMap();
        if (choice.value("value").toString() == now)
            return choice.value("free").toString().isEmpty() ? tilde(now) : tilde(now) + QStringLiteral(" · ") + choice.value("free").toString();
    }
    return tilde(now);
}

bool ModelSetup::downloadLow() const {
    const QString now = download();
    for (const QVariant &item : choices())
        if (item.toMap().value("value").toString() == now)
            return item.toMap().value("low").toBool();
    return false;
}

QString ModelSetup::downloadWarning() const {
    const QString now = download();
    for (const QVariant &item : choices()) {
        const QVariantMap choice = item.toMap();
        if (choice.value("value").toString() == now && choice.value("low").toBool())
            return tr("Only %1 free there.").arg(sizeText(choice.value("freeBytes").toLongLong()));
    }
    return {};
}

void ModelSetup::setDownload(const QString &path) {
    m_download = path;
    m_downloadChosen = true;
    emit changed();
}

QString ModelSetup::primaryText() const {
    if (m_phase == QLatin1String("consent"))
        return tr("Search");
    int used = 0;
    for (const Row &row : m_rows)
        if (row.on && !row.inherited)
            ++used;
    if (m_rows.isEmpty())
        return tr("Done");
    return tr("Use %n folders", nullptr, used);
}

bool ModelSetup::primaryEnabled() const {
    if (m_phase == QLatin1String("consent"))
        return canSearch();
    if (m_looking)
        return false;
    for (const Row &row : m_rows)
        if (row.busy)
            return false; // a folder still being counted
    for (const Row &row : m_rows)
        if (row.on && !row.inherited && row.kind.isEmpty() && !row.inUse && row.found.tool != QLatin1String("InvokeAI"))
            return false; // a folder the person must say what it holds first
    return true;
}

// The folders to write: each row's {path, kind, on}. InvokeAI is told apart by the folders under its models/.
QVariantList ModelSetup::foldersToWrite(int *left) const {
    QVariantList list;
    for (const Row &row : m_rows) {
        if (row.inherited)
            continue;
        if (row.found.tool == QLatin1String("InvokeAI") && row.on && !row.inUse) {
            // Told apart on the worker: nothing is read here.
            for (const auto &[dir, kind] : row.found.told)
                list.append(QVariantMap{{"path", dir}, {"kind", kind}, {"on", true}});
            *left += row.found.leftOut;
            continue;
        }
        list.append(QVariantMap{{"path", row.path}, {"kind", row.kind}, {"on", row.on}});
    }
    return list;
}

void ModelSetup::skip() {
    if (!m_open)
        return;
    *m_cancel = true;
    m_generation++;
    if (m_firstRun)
        m_settings->markModelSetup(QStringLiteral("skipped"));
    close();
}

void ModelSetup::apply() {
    if (!m_open || !primaryEnabled())
        return;
    if (m_phase == QLatin1String("consent")) {
        search();
        return;
    }
    *m_cancel = true;
    m_generation++;
    if (m_rows.isEmpty()) {
        // Nothing found: Done writes `done` and nothing else (a rerun, nothing at all).
        if (m_firstRun)
            m_settings->markModelSetup(QStringLiteral("done"));
        close();
        return;
    }
    int left = 0;
    const QVariantList folders = foldersToWrite(&left);
    const QString chosen = download();
    const QVariantMap result = m_settings->applyModelSetup(folders, chosen, m_firstRun);
    if (!result.value("error").toString().isEmpty()) {
        m_message = result.value("error").toString();
        emit changed();
        return;
    }
    int used = 0;
    for (const Row &row : std::as_const(m_rows))
        if (row.on && !row.inherited)
            ++used;
    QString notice = tr("Fugue now uses your models in %n folders. New downloads go to %1.", nullptr, used)
                         .arg(tilde(m_settings->downloadFolder()));
    if (left > 0)
        notice += QStringLiteral(" ") + tr("%n folders of InvokeAI were left out: Fugue could not tell what they hold.", nullptr, left);
    close();
    emit applied(notice);
}

void ModelSetup::close() {
    m_open = false;
    m_looking = false;
    m_rows.clear();
    emit changed();
    emit closed();
}

} // namespace fugue
