#include "fugue_home.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstring>

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
    const QByteArray realRuntime = qgetenv("XDG_RUNTIME_DIR");
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

bool applyFugueHome(int &argc, char **argv, QString *error) {
    QString root = qEnvironmentVariable("FUGUE_HOME");
    for (int i = 1; i < argc; ++i) {
        const bool separate = std::strcmp(argv[i], "--home") == 0;
        if (!separate && std::strncmp(argv[i], "--home=", 7) != 0)
            continue;
        int used = 1;
        if (separate) {
            if (i + 1 >= argc) {
                if (error)
                    *error = QStringLiteral("--home needs a folder");
                return false;
            }
            root = QString::fromLocal8Bit(argv[i + 1]);
            used = 2;
        } else {
            root = QString::fromLocal8Bit(argv[i] + 7);
        }
        for (int j = i; j + used <= argc; ++j) // also moves the terminating null
            argv[j] = argv[j + used];
        argc -= used;
        break;
    }
    return root.isEmpty() ? true : setFugueHome(root, error);
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
