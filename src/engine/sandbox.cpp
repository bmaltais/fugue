#include "engine/sandbox.h"

#include "engine/paths.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <atomic>

namespace fugue::sandbox {

namespace {

QString homeOf(const QString &home) { return home.isEmpty() ? QDir::homePath() : QDir::cleanPath(home); }

bool inside(const QString &path, const QString &folder) {
    return path == folder || path.startsWith(folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/'));
}

// The path made absolute and canonical when it exists; cleaned only when it does not.
QString canonical(const QString &path) {
    const QString real = QFileInfo(path).canonicalFilePath();
    return real.isEmpty() ? QDir::cleanPath(path) : real;
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

// Why `path` may not be granted for reading or writing: the allowlist of 6.7.1 on top of denied(). Empty when it may.
QString grantProblem(const QString &path, bool write, const QString &home) {
    const QString clean = canonical(path);
    if (write) {
        const QString out = canonical(outputDir());
        if (!inside(clean, out))
            return QStringLiteral("%1 is outside the output folder (%2), the only place a pack may be given to write.").arg(shown(clean, home), shown(out, home));
    } else {
        if (clean == home)
            return QStringLiteral("~ itself is never given to a pack.");
        if (inside(clean, home)) {
            for (const QString &part : clean.mid(home.size() + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts))
                if (part.startsWith(QLatin1Char('.')))
                    return QStringLiteral("%1 lies in a dot folder or file of the home folder, which are never given to a pack.").arg(shown(clean, home));
        }
        for (const char *system : {"/tmp", "/var/tmp"})
            if (!inside(clean, home) && inside(clean, QLatin1String(system)))
                return QStringLiteral("%1 is in %2, which holds other programs' sockets and files.").arg(shown(clean, home), QLatin1String(system));
    }
    return deniedReason(clean, home);
}

Result<QStringList> pathList(const QJsonValue &value, const QString &which, const QString &home) {
    if (value.isUndefined())
        return QStringList{};
    if (!value.isArray())
        return makeError(QStringLiteral("invalid_document"), QStringLiteral("needs.files.%1 must be a list of paths.").arg(which));
    QStringList paths;
    for (const QJsonValue &entry : value.toArray()) {
        const QString text = entry.toString();
        const QString path = QDir::cleanPath(withHome(text, home));
        if (text.isEmpty() || !path.startsWith(QLatin1Char('/')))
            return makeError(QStringLiteral("invalid_document"),
                             QStringLiteral("needs.files.%1: \"%2\" must be an absolute path or start with ~/.").arg(which, text.left(80)));
        if (const QString why = grantProblem(path, which == QLatin1String("write"), home); !why.isEmpty())
            return makeError(QStringLiteral("invalid_document"), QStringLiteral("needs.files.%1: %2").arg(which, why));
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

QString uvCacheFor(const QString &packId) { return cacheDir() + QStringLiteral("/uv/") + packId; }

QStringList denied(const QString &homeFolder) {
    const QString home = homeOf(homeFolder);
    QStringList paths;
    for (const char *sub : {"/.ssh", "/.gnupg", "/.netrc", "/.git-credentials", "/.aws", "/.kube", "/.docker", "/.mozilla", "/.claude",
                            "/.config/gh", "/.config/uv", "/.config/fugue", "/.config/chromium", "/.config/google-chrome",
                            "/.config/BraveSoftware", "/.config/systemd", "/.config/autostart", "/.config/hypr", "/.cache/huggingface",
                            "/.local/bin", "/.local/state/fugue", "/.local/share/fugue", "/.local/share/uv/credentials", "/.local/share/keyrings",
                            "/.local/share/kwalletd", "/.local/share/applications", "/.password-store", "/.bashrc", "/.bash_profile",
                            "/.zshrc", "/.profile"})
        paths << home + QLatin1String(sub);
    paths << configDir() << stateDir() << dataDir() << cacheDir() << baseConfigDir() << baseDataDir() << baseStateDir()
          << "/etc" << "/proc" << "/sys" << "/dev" << "/run" << "/boot" << "/root";
    for (QString &path : paths)
        path = canonical(path);
    paths.removeDuplicates();
    return paths;
}

namespace {

// What a file handed to a program (an input, a model) may never be: keys and credentials and Fugue's own config and packs,
// which are in no model folder or picture folder. Fugue's data and cache folders are not here: models and runs live in them.
bool handedDenied(const QString &path, const QString &homeFolder) {
    const QString home = homeOf(homeFolder);
    QStringList blocked;
    for (const char *sub : {"/.ssh", "/.gnupg", "/.netrc", "/.git-credentials", "/.aws", "/.kube", "/.docker", "/.claude", "/.config/gh",
                            "/.config/uv", "/.config/fugue", "/.cache/huggingface/token", "/.local/share/uv/credentials",
                            "/.local/share/keyrings", "/.local/share/kwalletd", "/.password-store", "/.local/state/fugue"})
        blocked << home + QLatin1String(sub);
    blocked << configDir() << stateDir() << dataDir() + QStringLiteral("/packs") << "/etc" << "/proc" << "/sys" << "/dev" << "/run" << "/boot" << "/root";
    for (const QString &folder : std::as_const(blocked))
        if (inside(canonical(path), canonical(folder)))
            return true;
    return false;
}

} // namespace

QString deniedReason(const QString &path, const QString &home) {
    const QString clean = canonical(path);
    const QString where = shown(clean, homeOf(home));
    if (clean == QLatin1String("/"))
        return QStringLiteral("/ gives a pack the whole computer.");
    for (const QString &blocked : denied(home)) {
        if (inside(clean, blocked))
            return QStringLiteral("%1 is never given to a pack (it is, or lies in, %2).").arg(where, shown(blocked, homeOf(home)));
        if (inside(blocked, clean)) // a folder that holds a blocked one gives it away
            return QStringLiteral("%1 holds %2, which is never given to a pack.").arg(where, shown(blocked, homeOf(home)));
    }
    return {};
}

Result<Needs> parseNeeds(const QJsonObject &needs, const QString &homeFolder) {
    const QString home = homeOf(homeFolder);
    Needs result;
    result.network = needs.value("network").toBool(false);
    const QJsonValue gpu = needs.value("gpu");
    result.gpu = gpu.isObject() || gpu.toBool(false);
    if (result.gpu)
        result.gpuApi = gpu.isObject() ? gpu.toObject().value("api").toString(QStringLiteral("any")) : QStringLiteral("any");
    if (const QJsonValue files = needs.value("files"); files.isObject()) {
        const QJsonObject object = files.toObject();
        const Result<QStringList> read = pathList(object.value("read"), QStringLiteral("read"), home);
        if (!read)
            return read.error();
        const Result<QStringList> write = pathList(object.value("write"), QStringLiteral("write"), home);
        if (!write)
            return write.error();
        result.readPaths = *read;
        result.writePaths = *write;
    } else if (!files.isUndefined() && !files.isBool()) {
        return makeError(QStringLiteral("invalid_document"), QStringLiteral("needs.files must be {\"read\": [...], \"write\": [...]}."));
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
    result.writePaths = a.writePaths;
    for (const QString &path : b.writePaths)
        if (!result.writePaths.contains(path))
            result.writePaths << path;
    return result;
}

QStringList plainLines(const Needs &needs, bool sandboxed, bool hasSetup, const QString &homeFolder) {
    const QString home = homeOf(homeFolder);
    QStringList lines;
    if (!sandboxed)
        lines << QStringLiteral("NOT sandboxed: this pack's program runs as you.");
    if (needs.network)
        lines << QStringLiteral("Can use the network") << QStringLiteral("Can reach services on this computer");
    if (hasSetup)
        lines << QStringLiteral("Setup uses the network") << QStringLiteral("Setup can reach services on this computer");
    if (needs.gpu)
        lines << QStringLiteral("Can use the GPU");
    for (const QString &path : needs.readPaths)
        lines << QStringLiteral("Can read %1").arg(shown(path, home));
    for (const QString &path : needs.writePaths)
        lines << QStringLiteral("Can write %1").arg(shown(path, home));
    if (sandboxed && !needs.network && !needs.gpu && needs.readPaths.isEmpty() && needs.writePaths.isEmpty())
        lines << QStringLiteral("Runs in a sandbox: it sees only its own folder, the files it is given and the run's work folder.");
    return lines;
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

} // namespace

Command build(const Profile &profile) {
    const bool setup = profile.kind == Profile::Kind::Setup;
    const QString home = homeOf(profile.home);
    const QString pack = canonical(profile.packDir);
    const QString work = profile.workDir.isEmpty() ? QString() : canonical(profile.workDir);

    Command command;
    command.program = bwrapPath();
    QStringList &a = command.arguments;
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
        env << QStringLiteral("UV_CACHE_DIR=") + profile.uvCache << QStringLiteral("UV_PYTHON_INSTALL_DIR=") + uvPythonDir(home)
            << "UV_PYTHON_DOWNLOADS=never";
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
    // A venv's python is a link into uv's Python installs, which must be there at the same path.
    a << "--ro-bind-try" << uvPythonDir(home) << uvPythonDir(home);

    // A program that is not under /usr (uv in ~/.local/bin) is bound by itself.
    QString program = profile.program;
    if (!program.startsWith(QLatin1Char('/')))
        program = QStandardPaths::findExecutable(program);
    if (!program.isEmpty() && !inside(program, pack) && !inside(program, QStringLiteral("/usr")) && !inside(program, QStringLiteral("/bin")))
        a << "--ro-bind-try" << program << program;

    const auto realFolder = [](const QString &path) { // a folder that is not a link: a link there is never followed
        const QFileInfo info(path);
        return info.isDir() && !info.isSymLink();
    };
    const QString environment = pack + QStringLiteral("/.venv");
    if (setup) {
        // The pack's own files are read-only: what the setup makes is its environment folder and, if there is one, the lock file.
        bind(a, "--ro-bind", pack);
        if (realFolder(environment))
            bind(a, "--bind", environment);
        if (const QFileInfo lock(pack + QStringLiteral("/uv.lock")); lock.isFile() && !lock.isSymLink())
            bind(a, "--bind", lock.filePath());
        if (!profile.uvCache.isEmpty())
            a << "--bind-try" << profile.uvCache << profile.uvCache;
    } else {
        bind(a, "--ro-bind", pack);
        if (realFolder(environment))
            bind(a, "--bind", environment);
        if (!profile.exchangeDir.isEmpty())
            a << "--bind-try" << canonical(profile.exchangeDir) << canonical(profile.exchangeDir);
        if (profile.needs.gpu) {
            QStringList nodes = profile.gpuNodes;
            if (nodes.isEmpty()) {
                for (const QFileInfo &entry : QDir(QStringLiteral("/dev")).entryInfoList({QStringLiteral("nvidia*")}, QDir::System | QDir::Files | QDir::NoDotAndDotDot))
                    nodes << entry.filePath();
                if (profile.needs.gpuApi == QLatin1String("vulkan"))
                    nodes << QStringLiteral("/dev/dri");
            }
            for (const QString &node : std::as_const(nodes))
                a << "--dev-bind-try" << node << node;
            for (const char *module : {"/sys/module/nvidia", "/sys/module/nvidia_uvm"})
                a << "--ro-bind-try" << QLatin1String(module) << QLatin1String(module);
        }
        if (!work.isEmpty())
            bind(a, "--bind", work);
        for (const QString &path : profile.needs.writePaths)
            a << "--bind-try" << path << path;
        for (const QString &path : profile.needs.readPaths)
            a << "--ro-bind-try" << path << path;

        // The files it is handed, read-only. A link binds its target, and only inside an allowed root.
        for (const QString &handed : profile.handedFiles) {
            const QFileInfo info(handed);
            const QString target = info.canonicalFilePath();
            if (target.isEmpty()) {
                command.skipped << handed;
                continue;
            }
            if (info.isSymLink() || QFileInfo(info.absolutePath()).canonicalFilePath() != info.absolutePath()) {
                bool allowed = false;
                for (const QString &root : profile.allowedRoots)
                    allowed = allowed || inside(target, canonical(root));
                if (!allowed || handedDenied(target, profile.home)) {
                    command.skipped << handed;
                    continue;
                }
            } else if (handedDenied(target, profile.home)) {
                command.skipped << handed;
                continue;
            }
            if (inside(target, pack) || (!work.isEmpty() && inside(target, work)))
                continue;
            a << "--ro-bind" << target << target;
            if (target != QDir::cleanPath(handed))
                a << "--symlink" << target << QDir::cleanPath(handed);
            command.mounted << QDir::cleanPath(handed);
        }
    }

    a << "--chdir" << (setup || work.isEmpty() ? pack : work);
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
    if (!path.isEmpty()) {
        QProcess version;
        version.start(path, {"--version"});
        version.waitForFinished(5000);
        const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("(\\d+)\\.(\\d+)")).match(QString::fromUtf8(version.readAllStandardOutput()));
        const bool recent = match.hasMatch() && (match.captured(1).toInt() > 0 || match.captured(2).toInt() >= 8);
        if (recent) {
            QProcess probe;
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
