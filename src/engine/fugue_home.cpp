#include "fugue_home.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <cstring>

#include "engine/project.h"

namespace fugue {

namespace {

// The root's folders: the five XDG variables and TMPDIR, each a folder of the same name.
struct Redirect {
    const char *variable;
    const char *folder;
};
constexpr Redirect kRedirects[] = {
    {"XDG_CONFIG_HOME", "config"}, {"XDG_DATA_HOME", "data"}, {"XDG_STATE_HOME", "state"},
    {"XDG_CACHE_HOME", "cache"},   {"XDG_RUNTIME_DIR", "run"}, {"TMPDIR", "tmp"},
};

bool makePrivateFolder(const QString &path, QString *error) {
    if (!QDir().mkpath(path) || !QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        if (error)
            *error = QStringLiteral("cannot make the folder %1").arg(path);
        return false;
    }
    return true;
}

// The Wayland socket, the session bus and the sound server live in the real XDG_RUNTIME_DIR; the new
// one is empty. Keep them reachable by naming them absolutely, unless the user already did.
void keepDesktopSockets(const QByteArray &realRuntime) {
    if (realRuntime.isEmpty())
        return;
    const QByteArray display = qgetenv("WAYLAND_DISPLAY");
    if (!display.isEmpty() && !display.startsWith('/'))
        qputenv("WAYLAND_DISPLAY", realRuntime + '/' + display);
    if (!qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS") && QFileInfo::exists(QString::fromLocal8Bit(realRuntime) + "/bus"))
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + realRuntime + "/bus");
    for (const char *variable : {"PIPEWIRE_RUNTIME_DIR", "PULSE_RUNTIME_PATH"})
        if (!qEnvironmentVariableIsSet(variable))
            qputenv(variable, realRuntime);
}

} // namespace

bool setFugueHome(const QString &given, QString *error) {
    if (given.isEmpty() || !QDir::isAbsolutePath(given)) {
        if (error)
            *error = QStringLiteral("FUGUE_HOME must be an absolute folder, not \"%1\"").arg(given);
        return false;
    }
    const QString root = QDir::cleanPath(given);
    if (QFileInfo::exists(root) && !QFileInfo(root).isDir()) {
        if (error)
            *error = QStringLiteral("FUGUE_HOME %1 is not a folder").arg(root);
        return false;
    }
    if (!makePrivateFolder(root, error))
        return false;
    for (const Redirect &redirect : kRedirects)
        if (!makePrivateFolder(root + QLatin1Char('/') + QLatin1String(redirect.folder), error))
            return false;
    // The ordinary session's folders, once: a project inside a project (or a child process) must not
    // take the redirected ones for the base.
    struct Base {
        const char *variable;
        QStandardPaths::StandardLocation location;
    };
    for (const Base &base : {Base{"FUGUE_BASE_XDG_CONFIG_HOME", QStandardPaths::GenericConfigLocation},
                             Base{"FUGUE_BASE_XDG_DATA_HOME", QStandardPaths::GenericDataLocation},
                             Base{"FUGUE_BASE_XDG_STATE_HOME", QStandardPaths::GenericStateLocation},
                             Base{"FUGUE_BASE_XDG_CACHE_HOME", QStandardPaths::GenericCacheLocation}})
        if (qEnvironmentVariable(base.variable).isEmpty())
            qputenv(base.variable, QStandardPaths::writableLocation(base.location).toLocal8Bit());
    const QByteArray realRuntime = qgetenv("XDG_RUNTIME_DIR");
    // The runtime folder too (the cross-root GPU lock lives in it), once, as the ordinary session had it.
    if (qEnvironmentVariable("FUGUE_BASE_XDG_RUNTIME_DIR").isEmpty()) {
        const QString real = QString::fromLocal8Bit(realRuntime);
        qputenv("FUGUE_BASE_XDG_RUNTIME_DIR",
                (!real.isEmpty() && QFileInfo(real).isDir() ? real
                                                            : QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
                    .toLocal8Bit());
    }
    for (const Redirect &redirect : kRedirects)
        qputenv(redirect.variable, (root + QLatin1Char('/') + QLatin1String(redirect.folder)).toLocal8Bit());
    keepDesktopSockets(realRuntime);
    // The GPU drivers' compiled-kernel caches are not under XDG (`~/.nv`); their own variables name them.
    for (const char *variable : {"CUDA_CACHE_PATH", "__GL_SHADER_DISK_CACHE_PATH"})
        qputenv(variable, (root + QStringLiteral("/cache/gpu")).toLocal8Bit());
    makePrivateFolder(root + QStringLiteral("/cache/gpu"), nullptr);
    qputenv("FUGUE_HOME", root.toLocal8Bit());
    return true;
}

namespace {

bool g_selected = false; // a root or Default was named by an option or a variable: no "last used"

// Takes `--<name> <value>` or `--<name>=<value>` out of argv. `present` says it was there.
bool takeOption(int &argc, char **argv, const char *name, QString *value, bool *present, QString *error) {
    const QByteArray flag = QByteArray("--") + name;
    *present = false;
    for (int i = 1; i < argc; ++i) {
        const bool separate = flag == argv[i];
        if (!separate && std::strncmp(argv[i], flag.constData(), size_t(flag.size())) != 0)
            continue;
        if (!separate && argv[i][flag.size()] != '=')
            continue;
        int used = 1;
        if (separate) {
            if (i + 1 >= argc) {
                if (error)
                    *error = QStringLiteral("%1 needs a value").arg(QString::fromLatin1(flag));
                return false;
            }
            *value = QString::fromLocal8Bit(argv[i + 1]);
            used = 2;
        } else {
            *value = QString::fromLocal8Bit(argv[i] + flag.size() + 1);
        }
        for (int j = i; j + used <= argc; ++j) // also moves the terminating null
            argv[j] = argv[j + used];
        argc -= used;
        *present = true;
        break;
    }
    return true;
}

// The root a project named by `selector` has; empty with `ok` for Default.
bool projectRoot(const QString &selector, QString *root, QString *error) {
    root->clear();
    if (selector.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0) {
        if (!qEnvironmentVariable("FUGUE_HOME").isEmpty()) {
            if (error)
                *error = QStringLiteral("cannot select the default project from inside a project; start fugue from a normal shell");
            return false;
        }
        return true;
    }
    const Result<QString> found = resolveProjectRoot(selector);
    if (!found) {
        if (error)
            *error = found.error().message + (found.error().hint.isEmpty() ? QString() : QStringLiteral(" (") + found.error().hint + QLatin1Char(')'));
        return false;
    }
    *root = *found;
    return true;
}

} // namespace

bool applyFugueHome(int &argc, char **argv, QString *error) {
    g_selected = false;
    QString home, project;
    bool hasHome = false, hasProject = false;
    if (!takeOption(argc, argv, "home", &home, &hasHome, error) || !takeOption(argc, argv, "project", &project, &hasProject, error))
        return false;
    if (hasHome && hasProject) {
        if (error)
            *error = QStringLiteral("--home and --project cannot be used together");
        return false;
    }
    QString root;
    if (hasHome) {
        root = home;
    } else if (hasProject) {
        g_selected = true;
        if (!projectRoot(project, &root, error))
            return false;
    } else if (const QString env = qEnvironmentVariable("FUGUE_HOME"); !env.isEmpty()) {
        root = env;
    } else if (const QString named = qEnvironmentVariable("FUGUE_PROJECT"); !named.isEmpty()) {
        g_selected = true;
        if (!projectRoot(named, &root, error))
            return false;
    }
    if (!root.isEmpty())
        g_selected = true;
    return root.isEmpty() ? true : setFugueHome(root, error);
}

bool applyLastUsedProject(QString *error) {
    if (g_selected || !fugueHome().isEmpty())
        return true;
    const QString last = lastUsedProjectRoot();
    return last.isEmpty() ? true : setFugueHome(last, error);
}

QString fugueHome() {
    const QString root = qEnvironmentVariable("FUGUE_HOME");
    return root.isEmpty() ? QString() : QDir::cleanPath(root);
}

QString fugueHomeDir(const QString &name) {
    const QString root = fugueHome();
    return root.isEmpty() ? QString() : root + QLatin1Char('/') + name;
}

} // namespace fugue
