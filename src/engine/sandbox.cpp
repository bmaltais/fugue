#include "engine/sandbox.h"

#include "engine/fugue_home.h"
#include "engine/paths.h"
#include "engine/project.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>

#include <atomic>
#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>

namespace fugue::sandbox {

namespace {

QString homeOf(const QString &home) { return home.isEmpty() ? QDir::homePath() : QDir::cleanPath(home); }

// The path made absolute and canonical when it exists; cleaned only when it does not.
QString canonical(const QString &path) {
    const QString real = QFileInfo(path).canonicalFilePath();
    return real.isEmpty() ? QDir::cleanPath(path) : real;
}

// The home folder as the kernel names it (no link in it): grants are written against it, so that they open without links.
QString realHome(const QString &home) { return canonical(homeOf(home)); }

bool inside(const QString &path, const QString &folder) {
    return path == folder || path.startsWith(folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/'));
}

// ~ and ~/... as the home folder; other text as it is.
QString withHome(const QString &path, const QString &home) {
    if (path == QLatin1String("~"))
        return home;
    if (path.startsWith(QLatin1String("~/")))
        return home + path.mid(1);
    return path;
}

QString shown(const QString &path, const QString &home) {
    if (path == home)
        return QStringLiteral("~");
    return path.startsWith(home + QLatin1Char('/')) ? QStringLiteral("~") + path.mid(home.size()) : path;
}

// The base session's cache folder (paths.h has the other three): FUGUE_BASE_XDG_CACHE_HOME/fugue, or this session's.
QString baseCacheDir() {
    const QString saved = qEnvironmentVariable("FUGUE_BASE_XDG_CACHE_HOME");
    return saved.isEmpty() ? cacheDir() : QDir::cleanPath(saved) + QStringLiteral("/fugue");
}

// The roots of the projects Fugue knows (the registry, and this session's own root): another project's keys are in it.
QStringList projectRoots() {
    QStringList roots;
    if (const Result<ProjectRegistry> registry = readProjectRegistry())
        roots = registry->projects;
    if (const QString root = fugueHome(); !root.isEmpty())
        roots << root;
    for (QString &root : roots)
        root = canonical(root);
    roots.removeDuplicates();
    return roots;
}

// The folder of an unregistered project that `path` is or lies in (a project.json that reads as one), or empty. One that
// `path` holds deeper down is not found: that would take a walk of the whole tree.
QString projectAround(const QString &path) {
    for (QString folder = path; folder.size() > 1; folder = QFileInfo(folder).path())
        if (QFileInfo(projectManifestPath(folder)).isFile() && readProject(folder).ok())
            return folder;
    return {};
}

Result<QStringList> pathList(const QJsonValue &value, const QString &home) {
    if (value.isUndefined())
        return QStringList{};
    if (!value.isArray())
        return makeError(QStringLiteral("invalid_document"), QStringLiteral("needs.files.read must be a list of paths."));
    QStringList paths;
    for (const QJsonValue &entry : value.toArray()) {
        const QString text = entry.toString();
        const QString path = QDir::cleanPath(withHome(text, home));
        if (text.isEmpty() || !path.startsWith(QLatin1Char('/')))
            return makeError(QStringLiteral("invalid_document"),
                             QStringLiteral("needs.files.read: \"%1\" must be an absolute path or start with ~/.").arg(text.left(80)));
        // Checked as the kernel would find it now; build() checks again, on what it opened, at every start.
        if (const QString why = grantProblem(canonical(path), home); !why.isEmpty())
            return makeError(QStringLiteral("invalid_document"), QStringLiteral("needs.files.read: %1").arg(why));
        if (const QString why = grantProblem(path, home); !why.isEmpty())
            return makeError(QStringLiteral("invalid_document"), QStringLiteral("needs.files.read: %1").arg(why));
        if (!paths.contains(path))
            paths << path;
    }
    return paths;
}

} // namespace

QString uvPythonDir(const QString &homeFolder) {
    const QString set = qEnvironmentVariable("UV_PYTHON_INSTALL_DIR");
    if (set.startsWith(QLatin1Char('/')))
        return set;
    const QString data = qEnvironmentVariable("XDG_DATA_HOME");
    return (data.startsWith(QLatin1Char('/')) ? data : homeOf(homeFolder) + QStringLiteral("/.local/share")) + QStringLiteral("/uv/python");
}

QString uvCacheFor(const QString &packId) {
    quint64 bits[2];
    QRandomGenerator::system()->fillRange(bits);
    return cacheDir() + QStringLiteral("/uv/") + packId + QLatin1Char('+') + // '+' is in no pack id
           QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(bits), sizeof bits).toHex());
}

QStringList denied(const QString &homeFolder) {
    const QString home = realHome(homeFolder);
    QStringList paths;
    for (const char *sub : {"/.ssh", "/.gnupg", "/.netrc", "/.git-credentials", "/.aws", "/.kube", "/.docker", "/.mozilla", "/.claude",
                            "/.config/gh", "/.config/uv", "/.config/fugue", "/.config/chromium", "/.config/google-chrome",
                            "/.config/BraveSoftware", "/.config/systemd", "/.config/autostart", "/.config/hypr", "/.cache/huggingface",
                            "/.local/bin", "/.local/state/fugue", "/.local/share/fugue", "/.local/share/uv/credentials", "/.local/share/keyrings",
                            "/.local/share/kwalletd", "/.local/share/applications", "/.password-store", "/.bashrc", "/.bash_profile",
                            "/.zshrc", "/.profile", "/snap"})
        paths << home + QLatin1String(sub);
    paths << configDir() << stateDir() << dataDir() << cacheDir() << baseConfigDir() << baseDataDir() << baseStateDir() << baseCacheDir()
          << "/etc" << "/proc" << "/sys" << "/dev" << "/run" << "/boot" << "/root" << "/var/tmp";
    paths << projectRoots();
    for (QString &path : paths)
        path = canonical(path);
    paths.removeDuplicates();
    return paths;
}

QString deniedReason(const QString &path, const QString &home) {
    const QString clean = canonical(path);
    const QString where = shown(clean, realHome(home));
    if (clean == QLatin1String("/"))
        return QStringLiteral("/ gives a pack the whole computer.");
    for (const QString &blocked : denied(home)) {
        if (inside(clean, blocked))
            return QStringLiteral("%1 is never given to a pack (it is, or lies in, %2).").arg(where, shown(blocked, realHome(home)));
        if (inside(blocked, clean)) // a folder that holds a blocked one gives it away
            return QStringLiteral("%1 holds %2, which is never given to a pack.").arg(where, shown(blocked, realHome(home)));
    }
    return {};
}

QString grantProblem(const QString &path, const QString &homeFolder) {
    const QString home = realHome(homeFolder);
    const QString clean = QDir::cleanPath(path);
    if (clean == home || clean == homeOf(homeFolder))
        return QStringLiteral("~ itself is never given to a pack.");
    for (const QString &own : {home, homeOf(homeFolder)}) {
        if (!inside(clean, own))
            continue;
        for (const QString &part : clean.mid(own.size() + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts))
            if (part.startsWith(QLatin1Char('.')))
                return QStringLiteral("%1 lies in a dot folder or file of the home folder, which are never given to a pack.").arg(shown(clean, own));
    }
    // /tmp holds other programs' sockets and files; a home folder under /tmp (a test's) is the home folder.
    if (!inside(clean, home) && !inside(clean, homeOf(homeFolder)) && inside(clean, QStringLiteral("/tmp")))
        return QStringLiteral("%1 is in /tmp, which holds other programs' sockets and files.").arg(clean);
    if (inside(QStringLiteral("/tmp"), clean))
        return QStringLiteral("%1 holds /tmp, which holds other programs' sockets and files.").arg(clean);
    if (const QString out = canonical(outputDir()); inside(clean, out))
        return QStringLiteral("%1 lies in the output folder (%2), which holds the results of every run.").arg(shown(clean, home), shown(out, home));
    if (const QString project = projectAround(clean); !project.isEmpty())
        return QStringLiteral("%1 lies in the Fugue project %2, whose keys are in it.").arg(shown(clean, home), shown(project, home));
    return deniedReason(clean, homeFolder);
}

Result<Needs> parseNeeds(const QJsonObject &needs, const QString &homeFolder) {
    const QString home = realHome(homeFolder);
    Needs result;
    result.network = needs.value("network").toBool(false);
    const QJsonValue gpu = needs.value("gpu");
    result.gpu = gpu.isObject() || gpu.toBool(false);
    if (result.gpu)
        result.gpuApi = gpu.isObject() ? gpu.toObject().value("api").toString(QStringLiteral("any")) : QStringLiteral("any");
    if (const QJsonValue files = needs.value("files"); files.isObject()) {
        const QJsonObject object = files.toObject();
        if (const QJsonValue write = object.value("write"); !write.isUndefined() && !(write.isArray() && write.toArray().isEmpty()))
            return makeError(QStringLiteral("invalid_document"),
                             QStringLiteral("needs.files.write: write grants are not supported yet; return files as outputs."));
        const Result<QStringList> read = pathList(object.value("read"), home);
        if (!read)
            return read.error();
        result.readPaths = *read;
    } else if (!files.isUndefined() && !files.isBool()) {
        return makeError(QStringLiteral("invalid_document"), QStringLiteral("needs.files must be {\"read\": [...]}."));
    }
    return result;
}

QString needsProblem(const QJsonObject &needs) {
    const Result<Needs> parsed = parseNeeds(needs);
    return parsed ? QString() : parsed.error().message;
}

Needs merged(const Needs &a, const Needs &b) {
    Needs result;
    result.network = a.network || b.network;
    result.gpu = a.gpu || b.gpu;
    result.gpuApi = a.gpuApi == QLatin1String("vulkan") || b.gpuApi == QLatin1String("vulkan") ? QStringLiteral("vulkan") : (a.gpuApi.isEmpty() ? b.gpuApi : a.gpuApi);
    result.readPaths = a.readPaths;
    for (const QString &path : b.readPaths)
        if (!result.readPaths.contains(path))
            result.readPaths << path;
    return result;
}

QStringList needsLines(const Needs &needs, bool hasSetup, const QString &homeFolder) {
    const QString home = realHome(homeFolder);
    QStringList lines;
    if (needs.network)
        lines << QStringLiteral("Can use the network") << QStringLiteral("Can reach services on this computer and its X11/Xwayland display");
    if (hasSetup)
        lines << QStringLiteral("Setup uses the network") << QStringLiteral("Setup can reach services on this computer and its X11/Xwayland display");
    if (needs.gpu)
        lines << QStringLiteral("Can use the GPU");
    for (const QString &path : needs.readPaths)
        lines << QStringLiteral("Can read %1").arg(shown(path, home));
    return lines;
}

QStringList plainLines(const Needs &needs, bool sandboxed, bool hasSetup, const QString &homeFolder) {
    QStringList lines;
    if (!sandboxed)
        lines << QStringLiteral("NOT sandboxed: this pack's program runs as you.");
    lines << needsLines(needs, hasSetup, homeFolder);
    if (sandboxed && !needs.network && !needs.gpu && needs.readPaths.isEmpty())
        lines << QStringLiteral("Runs in a sandbox: it sees only its own folder, the files it is given and the run's work folder.");
    return lines;
}

QList<int> Descriptors::numbers() const {
    QList<int> out;
    for (const beneath::Fd &fd : held)
        out << fd.get();
    return out;
}

bool handedDenied(const QString &path, const QString &homeFolder) {
    // denied(), the one list: but this session's data folder and the base one hold the runs and the models that are handed
    // over, so of them only the packs are; and /tmp, which denied() leaves to the grant rules, is no area of its own here.
    const QStringList own{canonical(dataDir()), canonical(baseDataDir())};
    for (const QString &blocked : denied(homeFolder)) {
        if (own.contains(blocked)) {
            if (inside(path, blocked + QStringLiteral("/packs")))
                return true;
            continue;
        }
        if (inside(path, blocked) && !inside(own.first(), blocked)) // this session's own project root holds its runs and models
            return true;
    }
    return false;
}

namespace {

// Where the clean environment's HOME and XDG folders are: inside the empty /tmp.
constexpr const char *kHome = "/tmp/home";

// /bin, /sbin, /lib and /lib64 as the host has them: links into /usr (usr-merge), or folders of their own.
void systemFolders(QStringList &args) {
    for (const QString &name : {QStringLiteral("/bin"), QStringLiteral("/sbin"), QStringLiteral("/lib"), QStringLiteral("/lib64")}) {
        const QFileInfo info(name);
        if (info.isSymLink())
            args << "--symlink" << info.symLinkTarget().replace(QStringLiteral("/usr/"), QStringLiteral("usr/")) << name;
        else if (info.isDir())
            args << "--ro-bind" << name << name;
    }
}

void bind(QStringList &args, const char *how, const QString &path) { args << how << path << path; }

// What build() binds by descriptor: each folder or file opened without links, at the path the kernel has for it.
class Binder {
public:
    explicit Binder(Command &command) : m_command(command) {}

    // `path` (canonical) opened without links; its descriptor bound at its real path with `how` (--ro-bind-fd, --bind-fd).
    // Empty on success; else why not (`missing` set when there is nothing at `path`).
    QString bindPath(const char *how, const QString &path, bool folder, bool *missing = nullptr, QString *real = nullptr) {
        beneath::Fd fd = beneath::openNoLinks(path, O_PATH | (folder ? O_DIRECTORY : 0));
        if (!fd) {
            if (missing)
                *missing = errno == ENOENT;
            return QStringLiteral("%1 cannot be opened without following a link (%2).").arg(path, beneath::lastProblem());
        }
        const QString at = beneath::realPathOf(fd.get());
        if (at != path)
            return QStringLiteral("%1 is not where it was looked at (now %2).").arg(path, at);
        // A file or a folder only, checked on what was opened: a socket, a FIFO or a device is never handed over.
        struct stat info {};
        if (::fstat(fd.get(), &info) != 0 || !(S_ISREG(info.st_mode) || S_ISDIR(info.st_mode)))
            return QStringLiteral("%1 is not a file or a folder (a socket, a FIFO or a device is never handed over).").arg(path);
        if (real)
            *real = at;
        bindFd(how, std::move(fd), at);
        return {};
    }
    void bindFd(const char *how, beneath::Fd fd, const QString &at) {
        m_command.arguments << how << QString::number(fd.get()) << at;
        m_command.descriptors->held.push_back(std::move(fd));
    }

private:
    Command &m_command;
};

} // namespace

Command build(const Profile &profile) {
    const bool setup = profile.kind == Profile::Kind::Setup;
    const QString home = homeOf(profile.home);
    const QString pack = canonical(profile.packDir);
    const QString work = profile.workDir.isEmpty() ? QString() : canonical(profile.workDir);

    Command command;
    Binder binder(command);
    command.program = bwrapPath();
    QStringList &a = command.arguments;
    const auto refuse = [&command](const QString &why) {
        if (command.refused.isEmpty())
            command.refused = why;
    };
    a << "--unshare-all" << "--unshare-user" << "--disable-userns" << "--new-session" << "--die-with-parent";
    if (profile.needs.network || setup)
        a << "--share-net";

    // The clean environment: what the protocol and the language runtimes need, with every folder inside the empty /tmp.
    QStringList env{"PATH=/usr/bin:/bin", QStringLiteral("HOME=") + kHome, "TMPDIR=/tmp",
                    QStringLiteral("XDG_CACHE_HOME=%1/.cache").arg(kHome), QStringLiteral("XDG_CONFIG_HOME=%1/.config").arg(kHome),
                    QStringLiteral("XDG_DATA_HOME=%1/.local/share").arg(kHome), QStringLiteral("XDG_STATE_HOME=%1/.local/state").arg(kHome),
                    "PYTHONDONTWRITEBYTECODE=1"};
    for (const char *name : {"LANG", "LC_ALL", "LC_CTYPE", "TZ"})
        if (qEnvironmentVariableIsSet(name))
            env << QStringLiteral("%1=%2").arg(QLatin1String(name), qEnvironmentVariable(name));
    if (setup) {
        // UV_LINK_MODE=copy: nothing in .venv points into the cache, which goes when the setup ends.
        env << QStringLiteral("UV_CACHE_DIR=") + profile.uvCache << QStringLiteral("UV_PYTHON_INSTALL_DIR=") + uvPythonDir(home)
            << "UV_PYTHON_DOWNLOADS=never" << "UV_LINK_MODE=copy";
    }
    env << profile.environment;
    a << "--clearenv";
    for (const QString &entry : std::as_const(env)) {
        const qsizetype equals = entry.indexOf(QLatin1Char('='));
        a << "--setenv" << entry.left(equals) << entry.mid(equals + 1);
    }

    // The system: /usr and the few files of /etc a program and its libraries read.
    bind(a, "--ro-bind", "/usr");
    systemFolders(a);
    a << "--proc" << "/proc" << "--dev" << "/dev" << "--tmpfs" << "/tmp";
    for (const char *sub : {"", "/.cache", "/.config", "/.local", "/.local/share", "/.local/state"})
        a << "--dir" << QString::fromLatin1(kHome) + QLatin1String(sub);
    for (const char *file : {"/etc/ld.so.cache", "/etc/ld.so.conf", "/etc/ld.so.conf.d", "/etc/localtime", "/etc/nsswitch.conf",
                             "/etc/passwd", "/etc/group", "/etc/hosts", "/etc/os-release", "/etc/resolv.conf", "/etc/ssl",
                             "/etc/ca-certificates", "/etc/pki", "/etc/alternatives", "/etc/mime.types"})
        a << "--ro-bind-try" << QLatin1String(file) << QLatin1String(file);
    // A venv's python is a link into uv's Python installs, which must be there at the same path: bound by descriptor, and never
    // when UV_PYTHON_INSTALL_DIR (Fugue's environment) names a denied area.
    if (const QString python = uvPythonDir(home); deniedReason(canonical(python), home).isEmpty())
        if (beneath::Fd fd = beneath::openNoLinks(canonical(python), O_PATH | O_DIRECTORY); fd)
            binder.bindFd("--ro-bind-fd", std::move(fd), QDir::cleanPath(python));

    // A program that is not under /usr (uv in ~/.local/bin) is bound by itself.
    QString program = profile.program;
    if (!program.startsWith(QLatin1Char('/')))
        program = QStandardPaths::findExecutable(program);
    if (!program.isEmpty() && !inside(program, pack) && !inside(program, QStringLiteral("/usr")) && !inside(program, QStringLiteral("/bin")))
        a << "--ro-bind-try" << program << program;

    // The pack's own files are read-only; its environment folder, when it is a real folder (never a link), is writable for the
    // setup alone: a run reads it.
    if (const QString why = binder.bindPath("--ro-bind-fd", pack, true); !why.isEmpty())
        refuse(QStringLiteral("The pack folder: ") + why);
    if (beneath::Fd venv = beneath::openNoLinks(pack + QStringLiteral("/.venv"), O_PATH | O_DIRECTORY); venv)
        binder.bindFd(setup ? "--bind-fd" : "--ro-bind-fd", std::move(venv), pack + QStringLiteral("/.venv"));
    if (setup) {
        if (!profile.uvCache.isEmpty())
            a << "--bind-try" << profile.uvCache << profile.uvCache; // Fugue's own folder, which no program can reach
    } else {
        if (profile.exchangeFd >= 0) {
            beneath::Fd exchange(::fcntl(profile.exchangeFd, F_DUPFD_CLOEXEC, 0));
            if (exchange)
                binder.bindFd("--bind-fd", std::move(exchange), profile.exchangeDir);
            else
                refuse(QStringLiteral("The exchange folder cannot be handed over."));
        }
        if (profile.needs.gpu) {
            QStringList nodes = profile.gpuNodes;
            if (nodes.isEmpty()) {
                // The driver's nodes by name: the control node, each GPU, nvidia-uvm and nvidia-uvm-tools; not nvidia-modeset or the caps.
                static const QRegularExpression driverNode(QStringLiteral("^nvidia(ctl|-uvm|-uvm-tools|[0-9]+)$"));
                for (const QFileInfo &entry : QDir(QStringLiteral("/dev")).entryInfoList({QStringLiteral("nvidia*")}, QDir::System | QDir::Files | QDir::NoDotAndDotDot))
                    if (driverNode.match(entry.fileName()).hasMatch())
                        nodes << entry.filePath();
                if (profile.needs.gpuApi == QLatin1String("vulkan")) // the render nodes only: a card node can drive the displays
                    for (const QFileInfo &entry : QDir(QStringLiteral("/dev/dri")).entryInfoList({QStringLiteral("renderD*")}, QDir::System | QDir::Files | QDir::NoDotAndDotDot))
                        nodes << entry.filePath();
            }
            for (const QString &node : std::as_const(nodes))
                a << "--dev-bind-try" << node << node;
            for (const char *module : {"/sys/module/nvidia", "/sys/module/nvidia_uvm"})
                a << "--ro-bind-try" << QLatin1String(module) << QLatin1String(module);
        }
        if (!work.isEmpty())
            if (const QString why = binder.bindPath("--bind-fd", work, true); !why.isEmpty())
                refuse(QStringLiteral("The work folder: ") + why);

        // needs.files.read, checked again on what was opened: a root registered since the load, or a link swapped in, is caught.
        for (const QString &path : profile.needs.readPaths) {
            beneath::Fd fd = beneath::openNoLinks(path, O_PATH);
            if (!fd) {
                if (errno != ENOENT) // a grant that is not there is left out, as before
                    refuse(QStringLiteral("needs.files.read: %1 cannot be opened without following a link (%2); the sandbox binds only a path with no link in it.")
                               .arg(shown(path, realHome(home)), beneath::lastProblem()));
                continue;
            }
            const QString real = beneath::realPathOf(fd.get());
            struct stat info {};
            if (::fstat(fd.get(), &info) != 0 || !(S_ISREG(info.st_mode) || S_ISDIR(info.st_mode))) {
                refuse(QStringLiteral("needs.files.read: %1 is not a file or a folder.").arg(shown(path, realHome(home))));
                continue;
            }
            if (real.isEmpty() || real != QDir::cleanPath(path)) {
                refuse(QStringLiteral("needs.files.read: %1 is not where it was looked at.").arg(shown(path, realHome(home))));
                continue;
            }
            if (const QString why = grantProblem(real, home); !why.isEmpty()) {
                refuse(QStringLiteral("needs.files.read: ") + why);
                continue;
            }
            binder.bindFd("--ro-bind-fd", std::move(fd), real);
        }

        QStringList roots;
        for (const QString &root : profile.modelRoots)
            roots << canonical(root);
        const auto inRoots = [&roots](const QString &path) {
            for (const QString &root : std::as_const(roots))
                if (inside(path, root))
                    return true;
            return false;
        };
        // Binds `handed` read-only by descriptor at its real path (a link to it at `handed` when they differ). Empty on success.
        const auto hand = [&](const QString &handed, bool model) -> QString {
            const QString clean = QDir::cleanPath(handed);
            const QString target = QFileInfo(clean).canonicalFilePath();
            if (target.isEmpty())
                return QStringLiteral("%1 is not there.").arg(clean);
            const bool linked = target != clean;
            if ((model || linked) && !inRoots(target))
                return QStringLiteral("%1 is not in a model folder.").arg(clean);
            if (handedDenied(target, profile.home))
                return QStringLiteral("%1 is never given to a pack.").arg(clean);
            if (inside(target, pack) || (!work.isEmpty() && inside(target, work)))
                return {}; // in sight already
            QString real;
            if (const QString why = binder.bindPath("--ro-bind-fd", target, false, nullptr, &real); !why.isEmpty())
                return why;
            if (!inRoots(real) && (model || linked))
                return QStringLiteral("%1 is not in a model folder.").arg(clean);
            if (real != clean)
                a << "--symlink" << real << clean;
            command.mounted << clean;
            return {};
        };
        for (const QString &model : profile.modelFiles)
            if (const QString why = hand(model, true); !why.isEmpty())
                refuse(QStringLiteral("The model file ") + why);
        for (const QString &handed : profile.handedFiles)
            if (!hand(handed, false).isEmpty())
                command.skipped << handed;
    }

    a << "--chdir" << (setup ? pack : !work.isEmpty() ? work : profile.exchangeFd >= 0 ? profile.exchangeDir : pack);
    a << "--" << (program.isEmpty() ? profile.program : program) << profile.arguments;
    return command;
}

namespace {

QMutex &stateLock() {
    static QMutex mutex;
    return mutex;
}
std::optional<QString> &bwrapOverride() {
    static std::optional<QString> path;
    return path;
}
std::optional<bool> &cachedAvailable() {
    static std::optional<bool> answer;
    return answer;
}

} // namespace

QProcessEnvironment bwrapEnvironment() {
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    return environment;
}

QString bwrapPath() {
    QMutexLocker lock(&stateLock());
    if (bwrapOverride())
        return *bwrapOverride();
    return QFileInfo(QStringLiteral("/usr/bin/bwrap")).isExecutable() ? QStringLiteral("/usr/bin/bwrap") : QString();
}

bool available() {
    {
        QMutexLocker lock(&stateLock());
        if (cachedAvailable())
            return *cachedAvailable();
    }
    const QString path = bwrapPath();
    bool works = false;
    if (!path.isEmpty() && beneath::available()) {
        QProcess version;
        version.setProcessEnvironment(bwrapEnvironment());
        version.setChildProcessModifier([] { beneath::inheritOnly({}); });
        version.start(path, {"--version"});
        version.waitForFinished(5000);
        const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("(\\d+)\\.(\\d+)")).match(QString::fromUtf8(version.readAllStandardOutput()));
        const bool recent = match.hasMatch() && (match.captured(1).toInt() > 0 || match.captured(2).toInt() >= 8);
        if (recent) {
            QProcess probe;
            probe.setProcessEnvironment(bwrapEnvironment());
            probe.setChildProcessModifier([] { beneath::inheritOnly({}); });
            probe.start(path, {"--unshare-all", "--unshare-user", "--disable-userns", "--new-session", "--die-with-parent", "--ro-bind", "/", "/", "true"});
            works = probe.waitForFinished(10000) && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
        }
    }
    QMutexLocker lock(&stateLock());
    cachedAvailable() = works;
    return works;
}

QString unavailableHint() {
    if (available())
        return {};
    if (!beneath::available())
        return QStringLiteral("This kernel has no openat2 (Linux 5.6 or newer), which the sandbox needs to hand files over safely. Update the "
                              "kernel, or run this pack without a sandbox (fugue pack sandbox-off).");
    if (bwrapPath().isEmpty())
        return QStringLiteral("bubblewrap (bwrap) is not installed in /usr/bin. Bubblewrap 0.8 or newer is needed. Install it (Arch: pacman -S bubblewrap; Debian and Ubuntu: apt install "
                              "bubblewrap; Fedora: dnf install bubblewrap) and try again.");
    return QStringLiteral("bubblewrap 0.8 or newer is needed, and it must be able to make a user namespace here (the kernel or a security policy refuses "
                          "unprivileged user namespaces). Allow them, or run this pack without a sandbox (fugue pack sandbox-off).");
}

namespace {
std::atomic<bool> g_setupUnsandboxed{false};
} // namespace

void runSetupUnsandboxedForTest(bool on) { g_setupUnsandboxed = on; }
bool setupUnsandboxedForTest() { return g_setupUnsandboxed; }

void setBwrapForTest(std::optional<QString> path) {
    QMutexLocker lock(&stateLock());
    bwrapOverride() = std::move(path);
    cachedAvailable().reset();
}

} // namespace fugue::sandbox
