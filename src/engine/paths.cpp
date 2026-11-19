#include "paths.h"

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

} // namespace fugue
