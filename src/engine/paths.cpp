#include "paths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include "engine/fugue_home.h"
#include "engine/settings.h"

namespace fugue {

namespace {

QString under(QStandardPaths::StandardLocation location) {
    return QStandardPaths::writableLocation(location) + QStringLiteral("/fugue");
}

} // namespace

QString configDir() { return under(QStandardPaths::GenericConfigLocation); }
QString stateDir() { return under(QStandardPaths::GenericStateLocation); }
QString dataDir() { return under(QStandardPaths::GenericDataLocation); }
QString cacheDir() { return under(QStandardPaths::GenericCacheLocation); }

namespace {
QString baseUnder(const char *variable, const QString &current) {
    const QString saved = qEnvironmentVariable(variable);
    return saved.isEmpty() ? current : QDir::cleanPath(saved) + QStringLiteral("/fugue");
}
} // namespace

QString baseConfigDir() { return baseUnder("FUGUE_BASE_XDG_CONFIG_HOME", configDir()); }
QString baseDataDir() { return baseUnder("FUGUE_BASE_XDG_DATA_HOME", dataDir()); }
QString baseStateDir() { return baseUnder("FUGUE_BASE_XDG_STATE_HOME", stateDir()); }
QString mediaThumbnailsDir() { return cacheDir() + QStringLiteral("/thumbs"); }
QString thumbnailsDir() { return cacheDir() + QStringLiteral("/thumbnails"); }

QString documentsDir() {
    if (const QString inRoot = fugueHomeDir(QStringLiteral("documents")); !inRoot.isEmpty())
        return inRoot;
    return QDir::homePath() + QStringLiteral("/Documents/Fugue");
}

QString runtimeDir() {
    if (const QString inRoot = fugueHomeDir(QStringLiteral("run")); !inRoot.isEmpty())
        return inRoot;
    if (const QString env = qEnvironmentVariable("XDG_RUNTIME_DIR"); !env.isEmpty() && QFileInfo(env).isDir())
        return env;
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
}

QString providersFilePath() {
    // Shown to the user in messages: "~/.config/fugue/providers.json" as it always was, or the real file
    // under a FUGUE_HOME root.
    if (fugueHome().isEmpty())
        return QStringLiteral("~/.config/fugue/providers.json");
    return configDir() + QStringLiteral("/providers.json");
}

QString defaultOutputDir() {
    if (const QString inRoot = fugueHomeDir(QStringLiteral("outputs")); !inRoot.isEmpty())
        return inRoot;
    const QString pictures = qEnvironmentVariable("XDG_PICTURES_DIR");
    return (pictures.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) : pictures) +
           QStringLiteral("/Fugue");
}

QString outputDir() {
    if (const QString chosen = currentSettings().outputFolder; !chosen.isEmpty())
        return QDir::cleanPath(expandHome(chosen));
    return defaultOutputDir();
}

QString inputsDir() {
    if (const QString chosen = currentSettings().inputsFolder; !chosen.isEmpty())
        return QDir::cleanPath(expandHome(chosen));
    return outputDir() + QStringLiteral("/inputs");
}

QString inputsDirBeside(const QString &workflowPath) {
    return QFileInfo(workflowPath).absoluteDir().filePath(QStringLiteral("inputs"));
}

QString expandHome(const QString &path) {
    if (path == QLatin1String("~"))
        return QDir::homePath();
    if (path.startsWith(QLatin1String("~/")))
        return QDir::homePath() + path.mid(1);
    return path;
}

QStringList searchPaths(const QString &own, const QStringList &extra) {
    QStringList paths{QDir::cleanPath(own)};
    for (const QString &entry : extra)
        if (const QString path = QDir::cleanPath(expandHome(entry)); QDir::isAbsolutePath(path) && !paths.contains(path))
            paths << path;
    return paths;
}

} // namespace fugue
