#include "model_folder_finder.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include "engine/model_library.h"
#include "engine/paths.h"
#include "engine/registry.h"

namespace fugue {

namespace {

// The exact install folders (spec 8.9), relative to the home: a data table, in one place. `ComfyUI-Installs` is a parent: each
// child folder's models/ is one (ComfyUI Desktop's installs).
const QStringList &kProbes = *new QStringList{
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
const QString &kInstalls = *new QString(QStringLiteral("ComfyUI-Installs")); // never destroyed: a worker may outlive main()
const QString &kShared = *new QString(QStringLiteral("ComfyUI-Shared"));

// Folders the walk never enters.
const QSet<QString> &kSkipNames = *new QSet<QString>{QStringLiteral("node_modules"),     QStringLiteral("site-packages"), QStringLiteral("venv"),
                               QStringLiteral("__pycache__"),       QStringLiteral("Trash"),         QStringLiteral("$RECYCLE.BIN"),
                               QStringLiteral("System Volume Information"), QStringLiteral("Windows")};

constexpr int kHomeDepth = 4;
constexpr int kDriveDepth = 5;
constexpr int kMaxLinkHops = 40;

QString tilde(const QString &path, const QString &home) {
    if (!home.isEmpty() && (path == home || path.startsWith(home + QLatin1Char('/'))))
        return QStringLiteral("~") + path.mid(home.size());
    return path;
}

bool under(const QString &path, const QString &folder) {
    return path == folder || path.startsWith(folder + QLatin1Char('/'));
}

// lstat: whether the entry is there, a link counted as it is, never what it leads to.
bool lexists(const QString &path) {
    struct stat info;
    return ::lstat(QFile::encodeName(path).constData(), &info) == 0;
}

bool hasExt(const QString &fileName, const QStringList &extensions) {
    for (const QString &extension : extensions) {
        const QString dotted = extension.startsWith(QLatin1Char('.')) ? extension : QLatin1Char('.') + extension;
        if (fileName.endsWith(dotted, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

// The tool of a models folder from what stands beside it; `has` says whether a path is there (and may refuse to look).
QString toolOf(const QString &modelsDir, const std::function<bool(const QString &)> &has) {
    const QString clean = QDir::cleanPath(modelsDir);
    const QString parent = QFileInfo(clean).absolutePath();
    const QString grand = QFileInfo(parent).absolutePath();
    const QStringList parts = clean.split(QLatin1Char('/'));
    if (parts.contains(kShared) || parts.contains(kInstalls))
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

// `path` with every link in it resolved by reading the link texts (readlink), component by component from the root: what a link
// leads to is never stat'd, and each step is offered to `blocked` (and to `touch`, after it passed) before anything touches it.
// Empty when a step is blocked or the links loop. Nothing here follows a link: each readlink is on a path with no link in it.
QString resolveLinks(const QString &path, const std::function<bool(const QString &)> &blocked,
                     const std::function<void(const QString &)> &touch) {
    QStringList pending = QDir::cleanPath(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString current;
    int hops = 0;
    while (!pending.isEmpty()) {
        const QString part = pending.takeFirst();
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..")) {
            current = current.left(std::max(0, int(current.lastIndexOf(QLatin1Char('/')))));
            continue;
        }
        const QString next = current + QLatin1Char('/') + part;
        if (blocked(next))
            return {};
        if (touch)
            touch(next);
        char buffer[4096];
        const ssize_t n = ::readlink(QFile::encodeName(next).constData(), buffer, sizeof buffer);
        if (n >= 0) {
            if (++hops > kMaxLinkHops)
                return {};
            const QString target = QFile::decodeName(QByteArray(buffer, int(n)));
            if (target.startsWith(QLatin1Char('/')))
                current.clear();
            pending = target.split(QLatin1Char('/'), Qt::SkipEmptyParts) + pending;
            continue;
        }
        current = next;
    }
    return current.isEmpty() ? QStringLiteral("/") : current;
}

// A network or autofs mount, by the mount lists of `env`.
bool inMount(const QString &path, const QStringList &mounts) {
    for (const QString &mount : mounts)
        if (under(path, mount))
            return true;
    return false;
}

struct Entry {
    QString name;
    bool isDir = false;
    bool isLink = false;
    QString resolved; // a link's target, fully resolved by reading link texts; empty for a link that may not be followed
};

// Every place that could be offered, with no stat: the unticked ones are told from these without touching them.
QList<FinderLocation> allLocations(const FinderEnv &env) {
    QList<FinderLocation> list;
    for (const QString &probe : kProbes) {
        const QString path = QDir::cleanPath(env.home + QLatin1Char('/') + probe);
        // An install folder's neighbours name its tool, so the place is the install folder (its models/'s parent).
        const QString name = QFileInfo(path).fileName();
        const QString scope = name == kShared || name == kInstalls ? path : QFileInfo(path).absolutePath();
        list.append({QStringLiteral("probe:") + path, path, tilde(path, env.home), 0, {}, {}, scope});
    }
    list.append({QStringLiteral("home"), env.home, QStringLiteral("~"), kHomeDepth, {}, {}, env.home});
    for (const QString &parent : env.driveParents)
        list.append({QStringLiteral("drive:") + parent, parent, parent, kDriveDepth, {}, {}, parent});
    return list;
}

class Finder {
public:
    Finder(const FinderTypes &types, const FinderEnv &env, const QStringList &skip, const QList<FinderLocation> &ticked,
           const QList<FinderLocation> &offered)
        : m_env(env), m_types(types), m_deadline(env.now() + env.findCapMs), m_totalDeadline(env.now() + env.totalCountCapMs) {
        for (const QString &path : skip)
            m_skipped.insert(path);
        // Only a place that was offered and left off is a place to keep out of; one that was never listed (it does not exist) is not.
        QSet<QString> tickedIds;
        // A place is kept as written and as resolved: the walk goes by resolved paths, and a home behind a link (/home -> /var/home) or a
        // link from elsewhere into it must meet the same scope.
        const auto addScope = [this](const FinderLocation &location, bool isTicked) {
            m_scopes.append({location.scope, isTicked});
            if (!location.resolved.isEmpty())
                m_scopes.append({location.resolved, isTicked}); // the place's own resolved folder, never its parent (a whole drive)
            if (!location.resolvedScope.isEmpty())
                m_scopes.append({location.resolvedScope, isTicked}); // an install folder as resolved by its own links
        };
        for (const FinderLocation &location : ticked) {
            tickedIds.insert(location.id);
            addScope(location, true);
        }
        for (const FinderLocation &location : offered)
            if (!tickedIds.contains(location.id))
                addScope(location, false);
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
    bool isNetwork(const QString &path) const {
        for (const QString &mount : m_env.networkMounts)
            if (under(path, mount))
                return true;
        return false;
    }
    bool isAutofs(const QString &path) const {
        for (const QString &mount : m_env.autofsMounts)
            if (under(path, mount))
                return true;
        return false;
    }
    // A path the finder may not stat into: a network or automount, or a place the person left off (the longest place that covers it
    // decides: an unticked place with a ticked one inside it is blocked except for the ticked one).
    // With `traversal` a folder that only leads to a ticked place (its ancestor) may be passed by name.
    bool blocked(const QString &path, bool traversal = false) const {
        if (isNetwork(path))
            return true;
        if (traversal)
            for (const auto &[scope, ticked] : m_scopes)
                if (ticked && scope.startsWith(path + QLatin1Char('/')))
                    return false;
        int best = -1;
        bool bestTicked = true;
        for (const auto &[scope, ticked] : m_scopes)
            if (under(path, scope) && int(scope.size()) > best) {
                best = int(scope.size());
                bestTicked = ticked;
            }
        return best >= 0 && !bestTicked;
    }

    // resolveLinks() with this finder's blocks: the network, and the places left off.
    QString resolve(const QString &path) const {
        return resolveLinks(path, [this](const QString &next) { return blocked(next, true); },
                            [this](const QString &next) { touch(next); });
    }

    bool lstatHas(const QString &path) const {
        if (blocked(path))
            return false;
        touch(path);
        return lexists(path);
    }

    // The entries of a resolved folder by readdir: no stat for a plain entry; a link is resolved by its text and followed only
    // when its target may be. False when it cannot be read or holds more than `limit` entries (`tooMany` says which); with
    // `keepPartial` the entries read so far are kept. `deadline` (ms, -1 none) stops the reading too: `tooMany` is set.
    bool list(const QString &dir, int limit, QList<Entry> &out, bool *tooMany = nullptr, bool keepPartial = false,
              qint64 deadline = -1) const {
        if (tooMany)
            *tooMany = false;
        if (blocked(dir))
            return false;
        if (deadline < 0)
            deadline = m_listDeadline;
        touch(dir);
        DIR *handle = opendir(QFile::encodeName(dir).constData());
        if (!handle)
            return false;
        int count = 0;
        while (const dirent *item = readdir(handle)) {
            const QByteArray raw(item->d_name);
            if (raw == "." || raw == "..")
                continue;
            const bool late = deadline >= 0 && (count & 63) == 63 && (m_env.now() > deadline || cancelled());
            if (++count > limit || late) {
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
            const QString path = dir + QLatin1Char('/') + entry.name;
            unsigned char type = item->d_type;
            if (type == DT_UNKNOWN) {
                // A file system that gives no type: asked with an lstat, but only where the finder may stat, and the probe sees it.
                if (blocked(path))
                    continue;
                touch(path);
                struct stat info;
                if (::lstat(QFile::encodeName(path).constData(), &info) == 0)
                    type = S_ISLNK(info.st_mode) ? DT_LNK : S_ISDIR(info.st_mode) ? DT_DIR : DT_REG;
            }
            if (type == DT_DIR) {
                entry.isDir = true;
            } else if (type == DT_LNK) {
                entry.isLink = true;
                entry.resolved = resolve(path); // empty when the target is in a place that may not be touched
                if (!entry.resolved.isEmpty()) {
                    touch(entry.resolved);
                    struct stat info;
                    entry.isDir = ::stat(QFile::encodeName(entry.resolved).constData(), &info) == 0 && S_ISDIR(info.st_mode);
                }
            }
            out.append(entry);
        }
        closedir(handle);
        std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
        return true;
    }

    static QString childPath(const QString &dir, const Entry &entry) { return entry.isLink ? entry.resolved : dir + QLatin1Char('/') + entry.name; }

    QString toolName(const QString &modelsDir) const {
        return toolOf(modelsDir, [this](const QString &path) { return lstatHas(path); });
    }

    // How many different kinds the type folders named are (`Lora/` and `LyCORIS/` are one): a folder needs two, and then it is a
    // layout by the same rule kindOf() uses (ModelLibrary::layoutFromFolders), so the finder and the library never disagree.
    bool isLayoutBy(const QStringList &names, bool topLevelHasModels) const {
        QSet<QString> kinds;
        for (const QString &name : names)
            if (m_types.typeOfFolder.contains(name))
                kinds.insert(m_types.typeOfFolder.value(name));
        return kinds.size() >= 2 && ModelLibrary::layoutFromFolders(m_types.typeOfFolder, names, topLevelHasModels);
    }
    bool holdsModels(const QList<Entry> &entries) const {
        return std::any_of(entries.begin(), entries.end(), [this](const Entry &e) { return !e.isDir && m_types.takes(QStringLiteral("fugue/model"), e.name); });
    }
    // The listing detectKind() and the InvokeAI sampling go by: the finder's own, so no place left off and no link is followed blindly,
    // and it stops at `deadline`.
    ModelLibrary::DirLister lister(qint64 deadline) const {
        return [this, deadline](const QString &dir, QList<ModelLibrary::DirEntry> &out) {
            QList<Entry> entries;
            if (cancelled() || !list(dir, m_env.tooManyEntries, entries, nullptr, true, deadline))
                return false;
            for (const Entry &entry : std::as_const(entries)) {
                if (entry.name.startsWith(QLatin1Char('.')) || (entry.isLink && entry.resolved.isEmpty()))
                    continue;
                out.append({entry.name, entry.isDir, childPath(dir, entry)});
            }
            return true;
        };
    }

    // What `dir` is, when it is a model folder: its type folders' names and the tool. A tool needs two type folders (InvokeAI,
    // which sorts models its own way, is told by invokeai.yaml beside its models/ instead).
    bool evaluate(const QString &dir, FoundFolder &folder, QHash<QString, QString> &typeDirs) {
        QList<Entry> entries;
        if (!list(dir, m_env.tooManyEntries, entries, nullptr, false, m_deadline))
            return false;
        for (const Entry &entry : std::as_const(entries))
            if (entry.isDir && m_types.typeOfFolder.contains(entry.name))
                typeDirs.insert(entry.name, m_types.typeOfFolder.value(entry.name));
        const QString parent = QFileInfo(dir).absolutePath();
        const bool invoke = QFileInfo(dir).fileName() == QLatin1String("models") && lstatHas(parent + QStringLiteral("/invokeai.yaml"));
        const bool layout = isLayoutBy(typeDirs.keys(), holdsModels(entries));
        if (!layout && !invoke)
            return false;
        folder = FoundFolder();
        folder.path = dir;
        folder.tool = invoke && !layout ? QStringLiteral("InvokeAI") : toolName(dir);
        folder.layout = layout && folder.tool != QLatin1String("InvokeAI");
        folder.kind = folder.layout ? ModelLibrary::kLayout : QString();
        return true;
    }

    // A folder found as `viaPath`; `resolved` is what it really is.
    void consider(const QString &viaPath, const QString &resolved) {
        if (resolved.isEmpty() || m_skipped.contains(viaPath) || m_skipped.contains(resolved))
            return;
        if (m_visited.contains(resolved)) {
            // The same folder by another path: remember how it was reached.
            const int at = m_index.value(resolved, -1);
            if (at >= 0 && m_pending[at].path != viaPath && !m_pending[at].alsoReached.contains(viaPath))
                m_pending[at].alsoReached << viaPath;
            return;
        }
        m_visited.insert(resolved);
        FoundFolder folder;
        QHash<QString, QString> typeDirs;
        if (!evaluate(resolved, folder, typeDirs))
            return;
        folder.path = viaPath;
        folder.canonical = resolved;
        // The type folders may be links: where they lead, by text, and none to a place that may not be touched.
        for (auto it = typeDirs.begin(); it != typeDirs.end(); ++it) {
            const QString real = resolve(resolved + QLatin1Char('/') + it.key());
            if (!real.isEmpty())
                folder.typeDirs << real;
        }
        m_typeDirs.insert(resolved, typeDirs);
        m_index.insert(resolved, m_pending.size());
        m_pending.append(folder);
    }

    // An exact install folder on a network drive (not an automount): listed, never listed inside nor counted.
    void considerNetwork(const QString &path) {
        if (m_visited.contains(path) || m_skipped.contains(path))
            return;
        touch(path);
        if (!lexists(path))
            return;
        m_visited.insert(path);
        FoundFolder folder;
        folder.path = path;
        folder.canonical = path;
        folder.network = true;
        folder.tool = path.contains(kShared) || path.contains(kInstalls) ? QStringLiteral("ComfyUI Desktop") : QStringLiteral("Model folder");
        m_index.insert(path, m_pending.size());
        m_pending.append(folder);
    }

    void walk(const QString &dir, int depthLeft) {
        if (dir.isEmpty() || stop())
            return;
        if (m_walked.contains(dir) || m_visited.contains(dir))
            return;
        m_walked.insert(dir);
        QList<Entry> entries;
        if (cancelled() || !list(dir, m_env.tooManyEntries, entries, nullptr, false, m_deadline))
            return;
        QStringList names;
        bool invoke = false;
        for (const Entry &entry : std::as_const(entries)) {
            if (entry.isDir && m_types.typeOfFolder.contains(entry.name))
                names << entry.name;
            if (!entry.isDir && entry.name == QLatin1String("invokeai.yaml"))
                invoke = true;
            if (!entry.isDir && entry.name == QLatin1String("model_index.json"))
                return; // a diffusers repo: its folders are not a model library
        }
        if (isLayoutBy(names, holdsModels(entries))) {
            consider(dir, dir);
            return; // nothing below a candidate
        }
        if (invoke)
            for (const Entry &entry : std::as_const(entries))
                if (entry.isDir && entry.name == QLatin1String("models")) {
                    consider(childPath(dir, entry), childPath(dir, entry));
                    return;
                }
        if (depthLeft <= 0)
            return;
        for (const Entry &entry : std::as_const(entries)) {
            if (!entry.isDir || entry.name.startsWith(QLatin1Char('.')) || kSkipNames.contains(entry.name))
                continue;
            if (entry.isLink && entry.resolved.isEmpty())
                continue; // a link into a place that may not be touched
            const QString child = childPath(dir, entry);
            if (!m_env.ownDataDir.isEmpty() && child == m_env.ownDataDir)
                continue;
            if (entry.isLink) {
                // Another way to a folder: what is found below it is reached by this path too.
                const QString via = dir + QLatin1Char('/') + entry.name;
                if (m_walked.contains(child) || m_visited.contains(child)) {
                    if (!m_aliases[child].contains(via))
                        m_aliases[child] << via;
                    continue;
                }
                m_walkedVia.insert(child, via);
            }
            walk(child, depthLeft - 1);
            if (stop())
                return;
        }
    }

    void search(const FinderLocation &location) {
        if (stop())
            return;
        if (location.id.startsWith(QLatin1String("probe:"))) {
            const QString base = location.path;
            if (isAutofs(base))
                return;
            if (isNetwork(base)) {
                considerNetwork(base);
                return;
            }
            const QString real = resolve(base);
            if (real.isEmpty())
                return;
            if (QFileInfo(base).fileName() == kInstalls) {
                QList<Entry> children;
                if (list(real, m_env.tooManyEntries, children))
                    for (const Entry &child : std::as_const(children))
                        if (child.isDir && !stop() && !(child.isLink && child.resolved.isEmpty())) {
                            const QString models = resolve(childPath(real, child) + QStringLiteral("/models"));
                            consider(models, models);
                        }
                return;
            }
            consider(base, real);
            // ComfyUI-Shared may hold its models/ inside.
            if (QFileInfo(base).fileName() == kShared && !m_index.contains(real)) {
                const QString models = resolve(real + QStringLiteral("/models"));
                consider(models, models);
            }
            return;
        }
        if (location.id == QLatin1String("home")) {
            walk(resolve(location.path), kHomeDepth);
            return;
        }
        // A folder of drives: each is a root of its own.
        const QString parent = resolve(location.path);
        QList<Entry> drives;
        if (parent.isEmpty() || isAutofs(parent) || !list(parent, m_env.tooManyEntries, drives))
            return;
        for (const Entry &drive : std::as_const(drives)) {
            if (!drive.isDir || drive.name.startsWith(QLatin1Char('.')) || stop() || (drive.isLink && drive.resolved.isEmpty()))
                continue;
            walk(childPath(parent, drive), kDriveDepth);
        }
    }

    bool hasModelBelow(const QString &dir, int depth) const {
        QList<Entry> entries;
        if (cancelled() || !list(dir, m_env.tooManyEntries, entries))
            return false;
        for (const Entry &entry : std::as_const(entries)) {
            if (entry.name.startsWith(QLatin1Char('.')) || (entry.isLink && entry.resolved.isEmpty()))
                continue;
            if (!entry.isDir && m_types.takes(QStringLiteral("fugue/model"), entry.name))
                return true;
            if (entry.isDir && depth > 0 && hasModelBelow(childPath(dir, entry), depth - 1))
                return true;
        }
        return false;
    }
    // InvokeAI sorts models its own way: the folders under its models/ (2 levels) whose files agree on one kind are told, by the
    // headers of a few of their files. Only for a row the person ticked (a job of its own), never while searching.
    void fillInvoke(FoundFolder &folder, const QString &real, qint64 deadline) {
        if (folder.tool != QLatin1String("InvokeAI") || folder.network || real.isEmpty())
            return;
        m_listDeadline = deadline;
        const ModelLibrary::DirLister listing = lister(deadline);
        const auto kindOf = [&](const QString &dir) {
            if (blocked(dir) || cancelled())
                return QString();
            const QString kind = ModelLibrary::detectKind(dir, m_types.typeOfFolder, m_types.typeOfFiles, listing, m_env.cancel);
            return kind == ModelLibrary::kLayout ? QString() : kind;
        };
        QList<Entry> level1;
        if (list(real, m_env.tooManyEntries, level1)) {
            for (const Entry &one : std::as_const(level1)) {
                if (cancelled())
                    break;
                if (!one.isDir || one.name.startsWith(QLatin1Char('.')) || (one.isLink && one.resolved.isEmpty()))
                    continue;
                const QString dir = childPath(real, one);
                if (!hasModelBelow(dir, 3))
                    continue;
                if (const QString kind = kindOf(dir); !kind.isEmpty()) {
                    folder.told.append({dir, kind});
                    continue;
                }
                bool any = false;
                QList<Entry> level2;
                if (list(dir, m_env.tooManyEntries, level2))
                    for (const Entry &two : std::as_const(level2)) {
                        if (!two.isDir || two.name.startsWith(QLatin1Char('.')) || (two.isLink && two.resolved.isEmpty()))
                            continue;
                        const QString sub = childPath(dir, two);
                        if (!hasModelBelow(sub, 2))
                            continue;
                        if (const QString kind = kindOf(sub); !kind.isEmpty()) {
                            folder.told.append({sub, kind});
                            any = true;
                        } else {
                            ++folder.leftOut;
                        }
                    }
                if (!any && folder.leftOut == 0)
                    ++folder.leftOut;
            }
        }
        m_listDeadline = -1;
    }

    // The folders under an InvokeAI folder, told (the job of a row the person ticked).
    FoundFolder tell(const QString &dir) {
        FoundFolder folder;
        folder.path = QDir::cleanPath(dir);
        folder.tool = QStringLiteral("InvokeAI");
        folder.toldReady = true;
        if (isNetwork(folder.path) || blocked(folder.path))
            return folder;
        const QString real = resolve(folder.path);
        folder.canonical = real;
        fillInvoke(folder, real, m_env.now() + m_env.countCapMs);
        return folder;
    }

    // A folder in use, or one the person chose: what it is and what it holds, with no need for two type folders. `readHeaders`: a
    // chosen folder's kind is told by a few headers (what the person asked for); a folder in use is told by its folder names only, so
    // searching opens no model file. A folder inside a place left off, or behind a link into one, is listed, not looked into.
    FoundFolder describe(const QString &dir, bool readHeaders) {
        const qint64 deadline = m_env.now() + m_env.countCapMs; // the budget starts before the first listing
        m_listDeadline = deadline;
        struct Reset {
            const Finder *finder;
            ~Reset() { finder->m_listDeadline = -1; }
        } reset{this};
        FoundFolder folder;
        folder.path = QDir::cleanPath(dir);
        folder.tool = QStringLiteral("Model folder");
        if (isNetwork(folder.path)) {
            folder.canonical = folder.path;
            folder.network = true;
            return folder;
        }
        if (blocked(folder.path)) {
            folder.canonical = folder.path;
            folder.leftOff = true; // no stat at all
            return folder;
        }
        const QString real = resolve(folder.path);
        folder.canonical = real;
        if (real.isEmpty()) {
            folder.leftOff = true; // by a link, it leads where it may not go
            return folder;
        }
        QHash<QString, QString> typeDirs;
        QList<Entry> entries;
        if (list(real, m_env.countCapEntries, entries, nullptr, true, deadline))
            for (const Entry &entry : std::as_const(entries))
                if (entry.isDir && m_types.typeOfFolder.contains(entry.name))
                    typeDirs.insert(entry.name, m_types.typeOfFolder.value(entry.name));
        folder.tool = toolName(real);
        // What it holds is kindOf()'s decision, made here on the worker from plain values; a folder of one kind keeps its top-level files.
        if (readHeaders)
            folder.kind = ModelLibrary::detectKind(real, m_types.typeOfFolder, m_types.typeOfFiles, lister(deadline), m_env.cancel);
        else
            folder.kind = ModelLibrary::layoutFromFolders(m_types.typeOfFolder, typeDirs.keys(), holdsModels(entries)) ? ModelLibrary::kLayout : QString();
        folder.layout = folder.kind == ModelLibrary::kLayout;
        if (!folder.layout)
            typeDirs.clear(); // counted as a whole folder, below
        for (auto it = typeDirs.begin(); it != typeDirs.end(); ++it) {
            const QString typeReal = resolve(real + QLatin1Char('/') + it.key());
            if (!typeReal.isEmpty())
                folder.typeDirs << typeReal;
        }
        m_typeDirs.insert(real, typeDirs);
        count(folder, real, deadline);
        if (readHeaders) {
            fillInvoke(folder, real, deadline);
            folder.toldReady = folder.tool == QLatin1String("InvokeAI");
        }
        folder.writable = ::access(QFile::encodeName(real).constData(), W_OK) == 0;
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
            for (auto it = m_walkedVia.constBegin(); it != m_walkedVia.constEnd(); ++it)
                if (folder.canonical == it.key() || folder.canonical.startsWith(it.key() + QLatin1Char('/')))
                    folder.alsoReached << it.value() + folder.canonical.mid(it.key().size());
            if (!folder.network) {
                const qint64 now = m_env.now();
                if (now >= m_totalDeadline) {
                    folder.countCapped = true; // the total for counting is spent: listed, not counted
                    m_capped = true;
                } else {
                    count(folder, folder.canonical, std::min(now + m_env.countCapMs, m_totalDeadline));
                    if (folder.countCapped && m_env.now() >= m_totalDeadline)
                        m_capped = true;
                }
            }
            folder.writable = folder.network || ::access(QFile::encodeName(folder.canonical).constData(), W_OK) == 0;
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
    // The model files below a folder's type folders (every file for InvokeAI and a folder of one kind), with stat sizes. Time and
    // cancellation are checked at every entry, in the listing and in the stat loop; whenever counting stops early `countCapped`
    // is set. A folder that a link leads out of the allowed places, or onto a network drive, is never entered.
    void count(FoundFolder &folder, const QString &real, qint64 deadline) {
        const QHash<QString, QString> typeDirs = m_typeDirs.value(real);
        int entries = 0;
        QSet<QString> seen;
        struct Item {
            QString dir;
            QString type;
        };
        QList<Item> stack;
        if (typeDirs.isEmpty()) {
            stack.append({real, QStringLiteral("fugue/model")});
        } else {
            QStringList names = typeDirs.keys();
            std::sort(names.begin(), names.end());
            for (const QString &name : std::as_const(names)) {
                const QString typeReal = resolve(real + QLatin1Char('/') + name);
                if (!typeReal.isEmpty())
                    stack.append({typeReal, typeDirs.value(name)});
            }
        }
        const auto out = [&] { return cancelled() || m_env.now() > deadline || entries > m_env.countCapEntries; };
        while (!stack.isEmpty()) {
            if (cancelled())
                return;
            if (out()) {
                folder.countCapped = true;
                return;
            }
            const Item item = stack.takeLast();
            if (seen.contains(item.dir))
                continue;
            seen.insert(item.dir);
            QList<Entry> children;
            bool tooMany = false;
            if (!list(item.dir, m_env.countCapEntries - entries, children, &tooMany, true, deadline))
                continue;
            if (tooMany)
                folder.countCapped = true;
            bool diffusers = false;
            for (const Entry &child : std::as_const(children))
                if (child.name == QLatin1String("model_index.json"))
                    diffusers = true;
            if (diffusers)
                continue; // a diffusers repo's files are not models Fugue lists
            for (const Entry &child : std::as_const(children)) {
                if (child.name.startsWith(QLatin1Char('.')))
                    continue;
                if (out()) {
                    folder.countCapped = true;
                    return;
                }
                ++entries;
                if (child.isLink && child.resolved.isEmpty())
                    continue; // into a place that may not be touched
                const QString path = childPath(item.dir, child);
                if (child.isDir) {
                    stack.append({path, item.type});
                } else if (m_types.takes(item.type, child.name)) {
                    touch(path);
                    struct stat info;
                    if (::stat(QFile::encodeName(path).constData(), &info) != 0 || !S_ISREG(info.st_mode))
                        continue;
                    folder.size += qint64(info.st_size);
                    ++folder.models;
                    if (!typeDirs.isEmpty())
                        folder.counts[item.type] += 1;
                }
            }
            if (tooMany) {
                folder.countCapped = true;
                return;
            }
        }
    }

    FinderEnv m_env;
    FinderTypes m_types;
    qint64 m_deadline;
    qint64 m_totalDeadline;
    mutable qint64 m_listDeadline = -1; // a listing started in describe()/tell() stops here too
    bool m_capped = false;
    QList<std::pair<QString, bool>> m_scopes; // every place's scope and whether it is ticked
    QSet<QString> m_skipped;
    QSet<QString> m_visited;
    QSet<QString> m_walked;
    QHash<QString, QString> m_walkedVia;     // a folder walked through a link -> the link path it was reached by
    QHash<QString, QStringList> m_aliases;   // a folder -> other paths that lead to it
    QHash<QString, int> m_index;
    QList<FoundFolder> m_pending;
    QHash<QString, QHash<QString, QString>> m_typeDirs;
};

// The mount points of `fileSystems`. An autofs point that a real file system is mounted on (a systemd automount of /mnt/windows with
// ntfs3 on top: a later line whose mount point is the same and whose parent is the autofs mount) is not one: it is there, its files
// can be read without mounting anything, and only a network file system on top blocks it (that one is listed in its own right).
QStringList mountsOf(const QByteArray &mountinfo, const QSet<QString> &fileSystems) {
    struct Mount {
        QByteArray id, parent;
        QString point, fs;
    };
    QList<Mount> all;
    for (const QByteArray &line : mountinfo.split('\n')) {
        const int dash = line.indexOf(" - ");
        if (dash < 0)
            continue;
        const QList<QByteArray> before = line.left(dash).split(' ');
        const QList<QByteArray> after = line.mid(dash + 3).split(' ');
        if (before.size() < 5 || after.isEmpty())
            continue;
        QByteArray point = before.at(4);
        point.replace("\\040", " "); // the only escape that matters for a folder name
        all.append({before.at(0), before.at(1), QDir::cleanPath(QFile::decodeName(point)), QString::fromLatin1(after.first())});
    }
    QStringList mounts;
    for (const Mount &mount : std::as_const(all)) {
        if (!fileSystems.contains(mount.fs))
            continue;
        if (mount.fs == QLatin1String("autofs")) {
            const bool mountedOn = std::any_of(all.begin(), all.end(), [&mount](const Mount &other) {
                return other.parent == mount.id && other.point == mount.point && other.fs != QLatin1String("autofs");
            });
            if (mountedOn)
                continue;
        }
        mounts << mount.point;
    }
    return mounts;
}

} // namespace

FinderTypes FinderTypes::from(const Registry &registry) {
    FinderTypes types;
    for (const DeclaredPortType *declared : registry.portTypes()) {
        if (declared->kind != PortKind::Resource)
            continue;
        const QString id = declared->type.toString();
        const QStringList names = declared->folders.isEmpty() ? QStringList{declared->type.id.section(QLatin1Char('/'), -1)} : declared->folders;
        for (const QString &name : names)
            types.typeOfFolder.insert(name, id);
        types.extensions.insert(id, declared->extensions);
        if (!declared->files.isEmpty())
            types.typeOfFiles.insert(declared->files, id);
    }
    return types;
}

bool FinderTypes::takes(const QString &typeId, const QString &fileName) const {
    const auto it = extensions.constFind(typeId);
    if (it == extensions.constEnd() || it->isEmpty())
        return hasExt(fileName, ModelLibrary::defaultModelExtensions());
    return hasExt(fileName, *it);
}

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
    if (mountinfo.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = mountinfo.readAll();
        env.networkMounts = networkMountsFrom(bytes);
        env.autofsMounts = autofsMountsFrom(bytes);
    }
    return env;
}

QStringList networkMountsFrom(const QByteArray &mountinfo) {
    static const QSet<QString> network{QStringLiteral("nfs"),         QStringLiteral("nfs4"),       QStringLiteral("cifs"),
                                       QStringLiteral("smb3"),        QStringLiteral("smbfs"),      QStringLiteral("fuse.sshfs"),
                                       QStringLiteral("fuse.rclone"), QStringLiteral("davfs"),      QStringLiteral("9p"),
                                       QStringLiteral("autofs")};
    return mountsOf(mountinfo, network);
}

QStringList autofsMountsFrom(const QByteArray &mountinfo) { return mountsOf(mountinfo, {QStringLiteral("autofs")}); }

QString finderResolve(const QString &path, const FinderEnv &env) {
    return resolveLinks(path, [&env](const QString &next) { return inMount(next, env.networkMounts); }, env.touched);
}

QList<FinderLocation> finderPlaces(const FinderEnv &env) { return allLocations(env); }

QList<FinderLocation> finderLocations(const FinderEnv &env) { return finderLocations(env, allLocations(env)); }

QList<FinderLocation> finderLocations(const FinderEnv &env, const QList<FinderLocation> &candidates) {
    QList<FinderLocation> list;
    for (const FinderLocation &location : candidates) {
        // Built component by component from the root, each link read and checked first: a place that leads (even by a link in its
        // parent) onto a network or automount is left out, and nothing past that link is stat'd. A network drive's exact install
        // folder is still listed (by lstat of the place itself), an automount never is.
        if (inMount(location.path, env.autofsMounts))
            continue;
        if (inMount(location.path, env.networkMounts)) {
            if (env.touched)
                env.touched(location.path);
            if (lexists(location.path))
                list.append(location);
            continue;
        }
        const QString real = finderResolve(location.path, env);
        if (real.isEmpty())
            continue;
        if (env.touched)
            env.touched(real);
        if (lexists(real)) {
            FinderLocation placed = location;
            placed.resolved = real; // kept beside the path as written: a scope is met by either
            placed.resolvedScope = location.scope == location.path ? real : finderResolve(location.scope, env);
            list.append(placed);
        }
    }
    return list;
}

QString finderToolName(const QString &modelsDir, const FinderEnv &env) {
    return toolOf(modelsDir, [&env](const QString &path) {
        if (env.touched)
            env.touched(path);
        return lexists(path);
    });
}

FoundFolder describeModelFolder(const FinderTypes &types, const QString &path, const FinderEnv &env, bool readHeaders,
                                const QList<FinderLocation> &ticked, const QList<FinderLocation> &offered) {
    Finder finder(types, env, {}, ticked, offered);
    return finder.describe(path, readHeaders);
}

FoundFolder describeModelFolder(const Registry &registry, const QString &path, const FinderEnv &env) {
    return describeModelFolder(FinderTypes::from(registry), path, env);
}

FoundFolder tellInvokeFolders(const FinderTypes &types, const QString &path, const FinderEnv &env, const QList<FinderLocation> &ticked,
                              const QList<FinderLocation> &offered) {
    Finder finder(types, env, {}, ticked, offered);
    return finder.tell(path);
}

FindResult findModelFolders(const FinderTypes &types, const QList<FinderLocation> &locations, const FinderEnv &env,
                            const QStringList &skip, const std::function<void(const FoundFolder &)> &found,
                            const QList<FinderLocation> &offered) {
    Finder finder(types, env, skip, locations, offered);
    // The exact folders first, then the walks.
    QList<FinderLocation> ordered = locations;
    std::stable_sort(ordered.begin(), ordered.end(), [](const FinderLocation &a, const FinderLocation &b) { return a.depth < b.depth; });
    for (const FinderLocation &location : std::as_const(ordered))
        finder.search(location);
    FindResult result;
    result.folders = finder.finish(found);
    result.capped = finder.capped();
    return result;
}

FindResult findModelFolders(const Registry &registry, const QList<FinderLocation> &locations, const FinderEnv &env,
                            const QStringList &skip, const std::function<void(const FoundFolder &)> &found,
                            const QList<FinderLocation> &offered) {
    return findModelFolders(FinderTypes::from(registry), locations, env, skip, found, offered);
}

} // namespace fugue
