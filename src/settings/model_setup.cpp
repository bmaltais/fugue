#include "settings/model_setup.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QPointer>
#include <QStorageInfo>
#include <QThread>

#include <algorithm>

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
qint64 freeBytesAt(const QString &path) {
    QString dir = path;
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

ModelSetup::ModelSetup(SettingsModel *settings) : QObject(settings), m_settings(settings) {
    m_finder = [this](const QList<FinderLocation> &locations, const QStringList &skip, const std::atomic<bool> &cancel,
                      const std::function<void(const FoundFolder &)> &found) {
        FinderEnv env = this->env();
        env.cancel = &cancel;
        const Registry *registry = types();
        if (registry)
            findModelFolders(*registry, locations, env, skip, found);
    };
}

ModelSetup::~ModelSetup() {
    m_cancel = true;
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
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
    return !hasModelFile(dataDir() + QStringLiteral("/models"));
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
    m_cancel = true;
    m_open = true;
    m_firstRun = firstRun;
    m_phase = QStringLiteral("consent");
    m_looking = false;
    m_capped = false;
    m_rows.clear();
    m_unticked.clear();
    m_message.clear();
    m_downloadChosen = false;
    m_download = ownRoot();
    m_locations = finderLocations(env()); // one stat each; nothing below them is listed
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

void ModelSetup::addInUse() {
    const Settings settings = currentSettings();
    const ModelLibrary library(ModelLibrary::roots(settings), types(), ModelLibrary::rootKinds(settings));
    const FinderEnv environment = env();
    const QStringList own = settings.modelPaths;
    for (const SearchPath &root : ModelLibrary::taggedRoots(settings)) {
        if (root.path == ownRoot())
            continue;
        Row row;
        row.path = root.path;
        row.inUse = true;
        row.inherited = root.inherited;
        row.on = true;
        row.kind = library.kindOf(root.path);
        if (types())
            row.found = describeModelFolder(*types(), root.path, environment);
        row.found.path = root.path;
        row.found.tool = row.found.tool.isEmpty() ? QStringLiteral("Model folder") : row.found.tool;
        row.writable = QFileInfo(root.path).isWritable();
        m_rows.append(row);
    }
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
    begin(ticked);
    emit changed();
}

void ModelSetup::begin(const QList<FinderLocation> &locations) {
    const int generation = ++m_generation;
    QStringList skip;
    for (const Row &row : std::as_const(m_rows)) {
        skip << row.path;
        skip << QFileInfo(row.path).canonicalFilePath();
    }
    if (m_thread) {
        m_cancel = true;
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
    m_cancel = false;
    const Finder finder = m_finder;
    if (m_inline) {
        finder(locations, skip, m_cancel, [this, generation](const FoundFolder &folder) { found(generation, folder); });
        finished(generation);
        return;
    }
    QPointer<ModelSetup> self(this);
    m_thread = QThread::create([self, finder, locations, skip, generation, cancel = &m_cancel] {
        finder(locations, skip, *cancel, [self, generation](const FoundFolder &folder) {
            if (self)
                QMetaObject::invokeMethod(self.data(), [self, generation, folder] { if (self) self->found(generation, folder); }, Qt::QueuedConnection);
        });
        if (self)
            QMetaObject::invokeMethod(self.data(), [self, generation] { if (self) self->finished(generation); }, Qt::QueuedConnection);
    });
    m_thread->start();
}

ModelSetup::Row ModelSetup::makeRow(const FoundFolder &folder) const {
    Row row;
    row.path = folder.path;
    row.found = folder;
    row.writable = QFileInfo(folder.path).isWritable();
    row.kind = folder.layout ? ModelLibrary::kLayout : QString();
    // Ticked: found with models; InvokeAI sorts its own way (off); an install with no models yet is off.
    row.on = folder.tool != QLatin1String("InvokeAI") && (folder.models > 0 || folder.network);
    return row;
}

void ModelSetup::found(int generation, const FoundFolder &folder) {
    if (generation != m_generation || !m_open)
        return;
    for (const Row &row : std::as_const(m_rows))
        if (row.path == folder.path)
            return;
    m_rows.append(makeRow(folder));
    emit changed();
}

void ModelSetup::finished(int generation) {
    if (generation != m_generation || !m_open)
        return;
    m_looking = false;
    m_phase = QStringLiteral("results");
    emit changed();
}

QString ModelSetup::addFolder(const QVariant &value) {
    QString path = value.typeId() == QMetaType::QUrl ? value.toUrl().toLocalFile() : value.toString();
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    path = QDir::cleanPath(expandHome(path.trimmed()));
    if (path.isEmpty() || !QFileInfo(path).isDir())
        return tr("%1 is not a folder.").arg(tilde(path));
    const QString canonical = QFileInfo(path).canonicalFilePath();
    for (const Row &row : std::as_const(m_rows))
        if (row.path == path || QFileInfo(row.path).canonicalFilePath() == canonical)
            return tr("That folder is already in the list.");
    if (m_phase == QLatin1String("consent")) {
        // A pick with no search: the results, with what is in use and this folder.
        m_rows.clear();
        addInUse();
        m_phase = QStringLiteral("results");
    }
    FoundFolder folder = types() ? describeModelFolder(*types(), path, env()) : FoundFolder();
    folder.path = path;
    Row row = makeRow(folder);
    row.on = true;
    if (!folder.layout) {
        const ModelLibrary library({ownRoot(), path}, types(), {});
        row.kind = library.kindOf(path);
        if (row.kind == ModelLibrary::kLayout)
            row.found.layout = true;
    }
    m_rows.append(row);
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
        if (f.network) {
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
                                {"inherited", row.inherited},
                                {"readOnly", !row.writable},
                                {"needsKind", row.kind.isEmpty() && !row.inUse && f.tool != QLatin1String("InvokeAI")},
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
        if (row.inherited || !row.on || !row.writable || row.kind != ModelLibrary::kLayout || row.found.tool == QLatin1String("InvokeAI"))
            continue;
        if (row.found.size > biggest) {
            biggest = row.found.size;
            best = row.path;
        }
    }
    return best;
}

QVariantList ModelSetup::choices() const {
    QVariantList list;
    auto entry = [this](const QString &path, const QString &title) {
        const qint64 free = m_free ? m_free(path) : freeBytesAt(path);
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
            const ModelLibrary library({ownRoot(), row.path}, types(), {});
            auto addTold = [&](const QString &dir) {
                const QString kind = library.kindOf(dir);
                if (kind.isEmpty() || kind == ModelLibrary::kLayout)
                    return false;
                list.append(QVariantMap{{"path", dir}, {"kind", kind}, {"on", true}});
                return true;
            };
            for (const QFileInfo &level1 : QDir(row.path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                if (!hasModelFile(level1.filePath()))
                    continue;
                if (addTold(level1.filePath()))
                    continue;
                bool any = false;
                for (const QFileInfo &level2 : QDir(level1.filePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
                    if (hasModelFile(level2.filePath()))
                        any = addTold(level2.filePath()) || any;
                if (!any)
                    ++*left;
            }
            continue;
        }
        list.append(QVariantMap{{"path", row.path}, {"kind", row.kind}, {"on", row.on}});
    }
    return list;
}

void ModelSetup::skip() {
    if (!m_open)
        return;
    m_cancel = true;
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
    m_cancel = true;
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
