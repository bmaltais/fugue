#include "paths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

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

QString defaultOutputDir() {
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
