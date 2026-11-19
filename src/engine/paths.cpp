#include "paths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace fugue {

namespace {

QString under(QStandardPaths::StandardLocation location) {
    return QStandardPaths::writableLocation(location) + QStringLiteral("/fugue");
}

} // namespace

QString configDir() { return under(QStandardPaths::GenericConfigLocation); }
QString stateDir() { return under(QStandardPaths::GenericStateLocation); }
QString cacheDir() { return under(QStandardPaths::GenericCacheLocation); }

QString outputDir() {
    const QString pictures = qEnvironmentVariable("XDG_PICTURES_DIR");
    return (pictures.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) : pictures) +
           QStringLiteral("/Fugue");
}

QString inputsDir() { return outputDir() + QStringLiteral("/inputs"); }

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

} // namespace fugue
