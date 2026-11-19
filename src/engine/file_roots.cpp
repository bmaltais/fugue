#include "file_roots.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include "engine/paths.h"

namespace fugue {

namespace {

bool insideAny(const QString &path, const QStringList &roots) {
    const QString canonical = canonicalPath(path);
    for (const QString &root : roots)
        if (isInside(canonical, canonicalPath(root)))
            return true;
    return false;
}

Errors outside(const QString &path, const QString &input, const QString &verb, const QStringList &roots) {
    Error error = makeInputError(QStringLiteral("permission_denied"), input,
                                 QStringLiteral("%1 is outside the folders this run may %2.").arg(path, verb),
                                 QStringLiteral("Fugue may %1 only under: %2.").arg(verb, roots.join(QStringLiteral(", "))));
    return {error};
}

QStringList stringList(const QJsonValue &value) {
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list.append(item.toString());
    return list;
}

} // namespace

QString canonicalPath(const QString &path) {
    QString absolute = QDir::cleanPath(QDir(QDir::currentPath()).absoluteFilePath(path));
    // The longest part that exists, with its links resolved; what follows is not made yet.
    QString existing = absolute;
    QString rest;
    while (!QFileInfo::exists(existing)) {
        const qsizetype slash = existing.lastIndexOf(QLatin1Char('/'));
        if (slash <= 0)
            return absolute;
        rest = existing.mid(slash) + rest;
        existing = existing.left(slash);
    }
    const QString resolved = QFileInfo(existing).canonicalFilePath();
    return QDir::cleanPath((resolved.isEmpty() ? existing : resolved) + rest);
}

bool isInside(const QString &path, const QString &root) {
    if (root == QLatin1String("/"))
        return path.startsWith(QLatin1Char('/'));
    return path == root || path.startsWith(root + QLatin1Char('/'));
}

FileRoots FileRoots::defaults(const QString &workingDirectory) {
    FileRoots roots;
    roots.read = {QDir::homePath()};
    roots.write = {workingDirectory, outputDir(), QDir::homePath() + QStringLiteral("/Documents/Fugue")};
    return roots;
}

bool FileRoots::allowsRead(const QString &path) const { return insideAny(path, read); }
bool FileRoots::allowsWrite(const QString &path) const { return insideAny(path, write); }

Errors FileRoots::checkRead(const QString &path, const QString &input) const {
    return allowsRead(path) ? Errors() : outside(path, input, QStringLiteral("read"), read);
}

Errors FileRoots::checkWrite(const QString &path, const QString &input) const {
    return allowsWrite(path) ? Errors() : outside(path, input, QStringLiteral("write"), write);
}

QJsonObject FileRoots::toJson() const {
    return {{"read", QJsonArray::fromStringList(read)}, {"write", QJsonArray::fromStringList(write)}};
}

Result<FileRoots> FileRoots::fromJson(const QJsonObject &object) {
    if (!object.value("read").isArray() || !object.value("write").isArray())
        return makeError(QStringLiteral("invalid_json"), QStringLiteral("File roots need `read` and `write` lists."));
    FileRoots roots;
    roots.read = stringList(object.value("read"));
    roots.write = stringList(object.value("write"));
    return roots;
}

} // namespace fugue
