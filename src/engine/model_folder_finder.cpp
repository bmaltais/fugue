#include "model_folder_finder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QElapsedTimer>

#include <dirent.h>

#include <algorithm>

#include "engine/model_library.h"
#include "engine/paths.h"
#include "engine/registry.h"

namespace fugue {

namespace {

// The exact install folders (spec 8.9), relative to the home: a data table, in one place. A trailing `/*` means every
// child folder's models/ (ComfyUI Desktop's installs).
const QStringList kProbes{
    QStringLiteral("ComfyUI/models"),
    QStringLiteral("ComfyUI-Shared"),
    QStringLiteral("ComfyUI-Installs"),
    QStringLiteral("StabilityMatrix/Data/Models"),
    QStringLiteral("StabilityMatrix/Models"),
    QStringLiteral("stable-diffusion-webui/models"),
    QStringLiteral("stable-diffusion-webui-forge/models"),
    QStringLiteral("SwarmUI/Models"),
    QStringLiteral("invokeai/models"),
};
const QString kInstalls = QStringLiteral("ComfyUI-Installs");

// Folders the walk never enters.
const QSet<QString> kSkipNames{QStringLiteral("node_modules"),     QStringLiteral("site-packages"), QStringLiteral("venv"),
                               QStringLiteral("__pycache__"),       QStringLiteral("Trash"),         QStringLiteral("$RECYCLE.BIN"),
                               QStringLiteral("System Volume Information"), QStringLiteral("Windows")};

struct Entry {
    QString name;
    bool isDir = false;
};

constexpr int kHomeDepth = 4;
constexpr int kDriveDepth = 5;

class Finder {
public:
    Finder(const Registry &registry, const FinderEnv &env, const QStringList &skip)
        : m_env(env), m_library({}, &registry), m_deadline(env.now() + env.findCapMs) {
        for (const DeclaredPortType *declared : registry.portTypes()) {
            if (declared->kind != PortKind::Resource)
                continue;
            const QStringList names = declared->folders.isEmpty() ? QStringList{declared->type.id.section(QLatin1Char('/'), -1)}
                                                                  : declared->folders;
            for (const QString &name : names)
                m_typeOf.insert(name, declared->type);
        }
        for (const QString &path : skip)
            m_skipped.insert(path);
        m_modelType = PortType::parse(QStringLiteral("fugue/model")).value_or(PortType());
    }

    bool cancelled() const { return m_env.cancel && m_env.cancel->load(); }
    bool timeUp() {
        if (m_capped)
            return true;
        if (m_env.now() > m_deadline)
            m_capped = true;
        return m_capped;
    }
    bool stop() { return cancelled() || timeUp(); }
    bool capped() const { return m_capped; }

    void touch(const QString &path) const {
        if (m_env.touched)
            m_env.touched(path);
    }
    bool exists(const QString &path) const {
        touch(path);
        return QFileInfo::exists(path);
    }
    bool isNetwork(const QString &path) const {
        for (const QString &mount : m_env.networkMounts)
            if (path == mount || path.startsWith(mount + QLatin1Char('/')))
                return true;
        return false;
    }

    // The entries of a folder by readdir: no stat for a plain entry, one for a link. False when it cannot be read or holds
    // more than `limit` entries (`tooMany` says which).
    bool list(const QString &dir, int limit, QList<Entry> &out, bool *tooMany = nullptr, bool keepPartial = false) const {
        touch(dir);
        if (tooMany)
            *tooMany = false;
        DIR *handle = opendir(QFile::encodeName(dir).constData());
        if (!handle)
            return false;
        int count = 0;
        while (const dirent *item = readdir(handle)) {
            const QByteArray raw(item->d_name);
            if (raw == "." || raw == "..")
                continue;
            if (++count > limit) {
                if (tooMany)
                    *tooMany = true;
                if (keepPartial)
                    break; // what was read so far is kept: a count of a huge folder is a lower bound
                closedir(handle);
                out.clear();
                return false;
            }
            Entry entry;
            entry.name = QFile::decodeName(raw);
            if (item->d_type == DT_DIR)
                entry.isDir = true;
            else if (item->d_type == DT_LNK || item->d_type == DT_UNKNOWN)
                entry.isDir = QFileInfo(dir + QLatin1Char('/') + entry.name).isDir();
            out.append(entry);
        }
        closedir(handle);
        std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
        return true;
    }

    // What `dir` is, when it is a model folder: its type folders' names and the tool. A tool needs two type folders (InvokeAI,
    // which sorts models its own way, is told by invokeai.yaml beside its models/ instead).
    bool evaluate(const QString &dir, FoundFolder &folder, QHash<QString, PortType> &typeDirs) {
        QList<Entry> entries;
        if (!list(dir, m_env.tooManyEntries, entries))
            return false;
        for (const Entry &entry : std::as_const(entries))
            if (entry.isDir && m_typeOf.contains(entry.name))
                typeDirs.insert(entry.name, m_typeOf.value(entry.name));
        const QString parent = QFileInfo(dir).absolutePath();
        const bool invoke = QFileInfo(dir).fileName() == QLatin1String("models") && exists(parent + QStringLiteral("/invokeai.yaml"));
        if (typeDirs.size() < 2 && !invoke)
            return false;
        folder = FoundFolder();
        folder.path = QDir::cleanPath(dir);
        folder.tool = invoke && typeDirs.size() < 2 ? QStringLiteral("InvokeAI") : finderToolName(dir, m_env);
        folder.layout = folder.tool != QLatin1String("InvokeAI");
        return true;
    }

    void consider(const QString &dir) {
        const QString clean = QDir::cleanPath(dir);
        const QString canonical = QFileInfo(clean).canonicalFilePath();
        if (canonical.isEmpty() || m_skipped.contains(clean) || m_skipped.contains(canonical))
            return;
        if (m_visited.contains(canonical)) {
            // The same folder by another path: remember how it was reached.
            const int at = m_index.value(canonical, -1);
            if (at >= 0 && m_pending[at].path != clean && !m_pending[at].alsoReached.contains(clean))
                m_pending[at].alsoReached << clean;
            return;
        }
        m_visited.insert(canonical);
        FoundFolder folder;
        QHash<QString, PortType> typeDirs;
        if (!evaluate(clean, folder, typeDirs))
            return;
        folder.canonical = canonical;
        if (isNetwork(clean)) {
            folder.network = true;
        } else {
            for (auto it = typeDirs.begin(); it != typeDirs.end(); ++it)
                folder.typeDirs << QFileInfo(clean + QLatin1Char('/') + it.key()).canonicalFilePath();
            m_typeDirs.insert(canonical, typeDirs);
        }
        m_index.insert(canonical, m_pending.size());
        m_pending.append(folder);
    }

    void walk(const QString &dir, int depthLeft) {
        if (stop() || isNetwork(dir))
            return;
        const QString canonical = QFileInfo(dir).canonicalFilePath();
        if (canonical.isEmpty())
            return;
        if (m_walked.contains(canonical) || m_visited.contains(canonical)) {
            // The same folder by a link: what is found below the first path is also reached below this one.
            if (m_walkedAs.value(canonical) != dir && !m_aliases[canonical].contains(dir))
                m_aliases[canonical] << dir;
            return;
        }
        m_walked.insert(canonical);
        m_walkedAs.insert(canonical, dir);
        QList<Entry> entries;
        bool tooMany = false;
        if (!list(dir, m_env.tooManyEntries, entries, &tooMany))
            return;
        int named = 0;
        bool invoke = false;
        for (const Entry &entry : std::as_const(entries)) {
            if (entry.isDir && m_typeOf.contains(entry.name))
                ++named;
            if (!entry.isDir && entry.name == QLatin1String("invokeai.yaml"))
                invoke = true;
        }
        if (named >= 2) {
            consider(dir);
            return; // nothing below a candidate
        }
        if (invoke) {
            for (const Entry &entry : std::as_const(entries))
                if (entry.isDir && entry.name == QLatin1String("models")) {
                    consider(dir + QStringLiteral("/models"));
                    return;
                }
        }
        if (depthLeft <= 0)
            return;
        for (const Entry &entry : std::as_const(entries)) {
            if (!entry.isDir || entry.name.startsWith(QLatin1Char('.')) || kSkipNames.contains(entry.name))
                continue;
            const QString child = QDir::cleanPath(dir + QLatin1Char('/') + entry.name);
            if (m_excluded.contains(child) || (!m_env.ownDataDir.isEmpty() && child == m_env.ownDataDir))
                continue;
            walk(child, depthLeft - 1);
            if (stop())
                return;
        }
    }

    void setExcluded(const QSet<QString> &paths) { m_excluded = paths; }

    void search(const FinderLocation &location) {
        if (stop())
            return;
        if (location.id.startsWith(QLatin1String("probe:"))) {
            const QString base = location.path;
            if (QFileInfo(base).fileName() == kInstalls) {
                QList<Entry> children;
                if (list(base, m_env.tooManyEntries, children))
                    for (const Entry &child : std::as_const(children))
                        if (child.isDir && !stop())
                            consider(base + QLatin1Char('/') + child.name + QStringLiteral("/models"));
                return;
            }
            consider(base);
            // ComfyUI-Shared may hold its models/ inside.
            if (QFileInfo(base).fileName() == QLatin1String("ComfyUI-Shared") && !m_index.contains(QFileInfo(base).canonicalFilePath()))
                consider(base + QStringLiteral("/models"));
            return;
        }
        if (location.id == QLatin1String("home")) {
            walk(location.path, kHomeDepth);
            return;
        }
        // A folder of drives: each is a root of its own.
        QList<Entry> drives;
        if (!list(location.path, m_env.tooManyEntries, drives))
            return;
        for (const Entry &drive : std::as_const(drives)) {
            if (!drive.isDir || drive.name.startsWith(QLatin1Char('.')) || stop())
                continue;
            const QString root = location.path + QLatin1Char('/') + drive.name;
            if (m_excluded.contains(root))
                continue;
            walk(root, kDriveDepth);
        }
    }

    // A folder the person chose: what it is and what it holds, with no need for two type folders.
    FoundFolder describe(const QString &dir) {
        FoundFolder folder;
        folder.path = QDir::cleanPath(dir);
        folder.canonical = QFileInfo(folder.path).canonicalFilePath();
        QHash<QString, PortType> typeDirs;
        QList<Entry> entries;
        if (list(folder.path, m_env.countCapEntries, entries, nullptr, true))
            for (const Entry &entry : std::as_const(entries))
                if (entry.isDir && m_typeOf.contains(entry.name))
                    typeDirs.insert(entry.name, m_typeOf.value(entry.name));
        folder.tool = finderToolName(folder.path, m_env);
        folder.layout = !typeDirs.isEmpty();
        if (isNetwork(folder.path)) {
            folder.network = true;
            return folder;
        }
        for (auto it = typeDirs.begin(); it != typeDirs.end(); ++it)
            folder.typeDirs << QFileInfo(folder.path + QLatin1Char('/') + it.key()).canonicalFilePath();
        m_typeDirs.insert(folder.canonical, typeDirs);
        count(folder);
        return folder;
    }

    // Counts the models of every candidate, then tells links from copies.
    QList<FoundFolder> finish(const std::function<void(const FoundFolder &)> &found) {
        QList<FoundFolder> done;
        for (FoundFolder folder : std::as_const(m_pending)) {
            if (cancelled())
                break;
            for (auto it = m_aliases.constBegin(); it != m_aliases.constEnd(); ++it)
                if (folder.canonical == it.key() || folder.canonical.startsWith(it.key() + QLatin1Char('/')))
                    for (const QString &other : it.value())
                        folder.alsoReached << other + folder.canonical.mid(it.key().size());
            if (!folder.network)
                count(folder);
            // An unnamed folder with no model in it is a coincidence of folder names (a source tree's `model/` and `docs/`).
            if (folder.tool == QLatin1String("Model folder") && !folder.network && folder.models == 0 && !folder.countCapped)
                continue;
            done.append(folder);
        }
        // B whose every type folder resolves inside A is A's alias (Stability Matrix linked into ComfyUI): one row, files once.
        QList<bool> alias(done.size(), false);
        for (int b = 0; b < done.size(); ++b) {
            if (done[b].typeDirs.isEmpty())
                continue;
            for (int a = 0; a < done.size(); ++a) {
                if (a == b || alias[a])
                    continue;
                const QString prefix = done[a].canonical + QLatin1Char('/');
                if (std::all_of(done[b].typeDirs.begin(), done[b].typeDirs.end(),
                                [&prefix](const QString &d) { return d.startsWith(prefix); })) {
                    done[a].alsoReached << done[b].path << done[b].alsoReached;
                    alias[b] = true;
                    break;
                }
            }
        }
        QList<FoundFolder> out;
        for (int i = 0; i < done.size(); ++i) {
            if (alias[i])
                continue;
            FoundFolder folder = done[i];
            folder.alsoReached.removeDuplicates();
            folder.alsoReached.removeAll(folder.path);
            if (found)
                found(folder);
            out.append(folder);
        }
        return out;
    }

private:
    // The model files below a folder's type folders (every file for InvokeAI), with stat sizes. Stops at a cap.
    void count(FoundFolder &folder) {
        const QHash<QString, PortType> typeDirs = m_typeDirs.value(folder.canonical);
        const qint64 deadline = m_env.now() + m_env.countCapMs;
        int entries = 0;
        QSet<QString> seen;
        struct Item {
            QString dir;
            PortType type;
        };
        QList<Item> stack;
        if (typeDirs.isEmpty()) {
            stack.append({folder.path, m_modelType});
        } else {
            QStringList names = typeDirs.keys();
            std::sort(names.begin(), names.end());
            for (const QString &name : std::as_const(names))
                stack.append({folder.path + QLatin1Char('/') + name, typeDirs.value(name)});
        }
        while (!stack.isEmpty()) {
            if (cancelled())
                return;
            if (m_env.now() > deadline || entries > m_env.countCapEntries) {
                folder.countCapped = true;
                return;
            }
            const Item item = stack.takeLast();
            const QString canonical = QFileInfo(item.dir).canonicalFilePath();
            if (canonical.isEmpty() || seen.contains(canonical))
                continue;
            seen.insert(canonical);
            QList<Entry> children;
            bool tooMany = false;
            if (!list(item.dir, m_env.countCapEntries - entries, children, &tooMany, true))
                continue;
            if (tooMany)
                folder.countCapped = true;
            for (const Entry &child : std::as_const(children)) {
                if (child.name.startsWith(QLatin1Char('.')))
                    continue;
                if (++entries > m_env.countCapEntries) {
                    folder.countCapped = true;
                    return;
                }
                const QString path = item.dir + QLatin1Char('/') + child.name;
                if (child.isDir) {
                    stack.append({path, item.type});
                } else if (m_library.takesFile(item.type, child.name)) {
                    touch(path);
                    folder.size += QFileInfo(path).size();
                    ++folder.models;
                    if (!typeDirs.isEmpty())
                        folder.counts[item.type.toString()] += 1;
                }
            }
        }
    }

    FinderEnv m_env;
    ModelLibrary m_library;
    qint64 m_deadline;
    bool m_capped = false;
    QHash<QString, PortType> m_typeOf;
    PortType m_modelType;
    QSet<QString> m_skipped;
    QSet<QString> m_excluded;
    QSet<QString> m_visited;
    QSet<QString> m_walked;
    QHash<QString, QString> m_walkedAs;      // canonical folder walked -> the path it was walked by
    QHash<QString, QStringList> m_aliases;   // canonical folder -> the other paths that lead to it
    QHash<QString, int> m_index;
    QList<FoundFolder> m_pending;
    QHash<QString, QHash<QString, PortType>> m_typeDirs;
};

QString tilde(const QString &path, const QString &home) {
    if (!home.isEmpty() && (path == home || path.startsWith(home + QLatin1Char('/'))))
        return QStringLiteral("~") + path.mid(home.size());
    return path;
}

} // namespace

qint64 FinderEnv::now() const {
    if (nowMs)
        return nowMs();
    static const QElapsedTimer started = [] { QElapsedTimer t; t.start(); return t; }();
    return started.elapsed();
}

FinderEnv FinderEnv::system() {
    FinderEnv env;
    env.home = QDir::cleanPath(QDir::homePath());
    const QString user = qEnvironmentVariable("USER");
    env.driveParents << QStringLiteral("/mnt");
    if (!user.isEmpty())
        env.driveParents << QStringLiteral("/media/") + user << QStringLiteral("/run/media/") + user;
    env.ownDataDir = QDir::cleanPath(dataDir());
    QFile mountinfo(QStringLiteral("/proc/self/mountinfo"));
    if (mountinfo.open(QIODevice::ReadOnly))
        env.networkMounts = networkMountsFrom(mountinfo.readAll());
    return env;
}

QStringList networkMountsFrom(const QByteArray &mountinfo) {
    static const QSet<QString> network{QStringLiteral("nfs"),        QStringLiteral("nfs4"),       QStringLiteral("cifs"),
                                       QStringLiteral("smb3"),       QStringLiteral("smbfs"),      QStringLiteral("fuse.sshfs"),
                                       QStringLiteral("fuse.rclone"), QStringLiteral("davfs"),     QStringLiteral("9p")};
    QStringList mounts;
    for (const QByteArray &line : mountinfo.split('\n')) {
        const int dash = line.indexOf(" - ");
        if (dash < 0)
            continue;
        const QList<QByteArray> before = line.left(dash).split(' ');
        const QList<QByteArray> after = line.mid(dash + 3).split(' ');
        if (before.size() < 5 || after.isEmpty() || !network.contains(QString::fromLatin1(after.first())))
            continue;
        QByteArray point = before.at(4);
        point.replace("\\040", " "); // the only escape that matters for a folder name
        mounts << QDir::cleanPath(QFile::decodeName(point));
    }
    return mounts;
}

namespace {
// Every place that could be offered, with no stat: the unticked ones are told from these without touching them.
QList<FinderLocation> allLocations(const FinderEnv &env) {
    QList<FinderLocation> list;
    for (const QString &probe : kProbes) {
        const QString path = QDir::cleanPath(env.home + QLatin1Char('/') + probe);
        list.append({QStringLiteral("probe:") + path, path, tilde(path, env.home), 0});
    }
    list.append({QStringLiteral("home"), env.home, QStringLiteral("~"), kHomeDepth});
    for (const QString &parent : env.driveParents)
        list.append({QStringLiteral("drive:") + parent, parent, parent, kDriveDepth});
    return list;
}
} // namespace

QList<FinderLocation> finderLocations(const FinderEnv &env) {
    QList<FinderLocation> list;
    for (const FinderLocation &location : allLocations(env)) {
        if (env.touched)
            env.touched(location.path);
        if (QFileInfo::exists(location.path))
            list.append(location);
    }
    return list;
}

QString finderToolName(const QString &modelsDir, const FinderEnv &env) {
    auto has = [&env](const QString &path) {
        if (env.touched)
            env.touched(path);
        return QFileInfo::exists(path);
    };
    const QString clean = QDir::cleanPath(modelsDir);
    const QString parent = QFileInfo(clean).absolutePath();
    const QString grand = QFileInfo(parent).absolutePath();
    const QStringList parts = clean.split(QLatin1Char('/'));
    if (parts.contains(QStringLiteral("ComfyUI-Shared")) || parts.contains(QStringLiteral("ComfyUI-Installs")))
        return QStringLiteral("ComfyUI Desktop");
    if (has(parent + QStringLiteral("/modules_forge")))
        return QStringLiteral("Forge");
    if (has(parent + QStringLiteral("/webui.py")))
        return QStringLiteral("Automatic1111");
    if (has(parent + QStringLiteral("/custom_nodes")))
        return has(grand + QStringLiteral("/python_embeded")) ? QStringLiteral("ComfyUI portable") : QStringLiteral("ComfyUI");
    if (has(parent + QStringLiteral("/launch-linux.sh")) || has(parent + QStringLiteral("/launch-windows.bat")))
        return QStringLiteral("SwarmUI");
    if (has(parent + QStringLiteral("/Packages")) || QFileInfo(parent).fileName() == QLatin1String("Data"))
        return QStringLiteral("Stability Matrix");
    if (has(parent + QStringLiteral("/invokeai.yaml")))
        return QStringLiteral("InvokeAI");
    return QStringLiteral("Model folder");
}

FoundFolder describeModelFolder(const Registry &registry, const QString &path, const FinderEnv &env) {
    Finder finder(registry, env, {});
    return finder.describe(path);
}

FindResult findModelFolders(const Registry &registry, const QList<FinderLocation> &locations, const FinderEnv &env,
                            const QStringList &skip, const std::function<void(const FoundFolder &)> &found) {
    Finder finder(registry, env, skip);
    // What is not ticked is never touched: the walk steps round it.
    QSet<QString> ticked;
    for (const FinderLocation &location : locations)
        ticked.insert(location.id);
    QSet<QString> excluded;
    for (const FinderLocation &location : allLocations(env))
        if (!ticked.contains(location.id))
            excluded.insert(location.path);
    // Fugue's own folder is never a candidate.
    excluded.insert(QDir::cleanPath(dataDir() + QStringLiteral("/models")));
    finder.setExcluded(excluded);
    // The exact folders first, then the walks.
    QList<FinderLocation> ordered = locations;
    std::stable_sort(ordered.begin(), ordered.end(), [](const FinderLocation &a, const FinderLocation &b) {
        return a.depth < b.depth;
    });
    for (const FinderLocation &location : std::as_const(ordered))
        finder.search(location);
    FindResult result;
    result.capped = finder.capped();
    result.folders = finder.finish(found);
    return result;
}

} // namespace fugue
