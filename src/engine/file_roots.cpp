#include "file_roots.h"

#include <climits>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include "engine/paths.h"

namespace fugue {

namespace {

bool insideAny(const QString &canonical, const QStringList &roots) {
    if (canonical.isEmpty())
        return false;
    for (const QString &root : roots) {
        const QString base = canonicalPath(root);
        if (!base.isEmpty() && isInside(canonical, base))
            return true;
    }
    return false;
}

Error outside(const QString &path, const QString &input, const QString &verb, const QStringList &roots) {
    Error error = makeInputError(QStringLiteral("permission_denied"), input,
                                 QStringLiteral("%1 is outside the folders this run may %2.").arg(path, verb),
                                 QStringLiteral("Fugue may %1 only under: %2.").arg(verb, roots.join(QStringLiteral(", "))));
    return error;
}

QStringList stringList(const QJsonValue &value) {
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list.append(item.toString());
    return list;
}

} // namespace

QString canonicalPath(const QString &path) {
    QStringList pending = QDir(QDir::currentPath()).absoluteFilePath(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList resolved;
    int links = 0;
    while (!pending.isEmpty()) {
        const QString part = pending.takeFirst();
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..")) {
            if (!resolved.isEmpty())
                resolved.removeLast();
            continue;
        }
        resolved.append(part);
        const QByteArray here = QFile::encodeName(QLatin1Char('/') + resolved.join(QLatin1Char('/')));
        char target[PATH_MAX];
        const ssize_t length = ::readlink(here.constData(), target, sizeof(target) - 1);
        if (length < 0)
            continue; // not a link (or not there yet): kept as written
        if (++links > 40)
            return {};
        resolved.removeLast();
        const QString to = QFile::decodeName(QByteArray(target, qsizetype(length)));
        if (to.startsWith(QLatin1Char('/')))
            resolved.clear();
        pending = to.split(QLatin1Char('/'), Qt::SkipEmptyParts) + pending;
    }
    return QLatin1Char('/') + resolved.join(QLatin1Char('/'));
}

bool isInside(const QString &path, const QString &root) {
    if (root == QLatin1String("/"))
        return path.startsWith(QLatin1Char('/'));
    return path == root || path.startsWith(root + QLatin1Char('/'));
}

FileRoots FileRoots::defaults(const QString &workingDirectory, const QString &workflowPath) {
    FileRoots roots;
    roots.read = {QDir::homePath(), inputsDir()};
    if (!workflowPath.isEmpty())
        roots.read.append(inputsDirBeside(workflowPath));
    roots.write = {workingDirectory, outputDir(), QDir::homePath() + QStringLiteral("/Documents/Fugue")};
    return roots;
}

FileRoots FileRoots::forInputs() {
    FileRoots roots;
    roots.read = {inputsDir()};
    roots.write = roots.read;
    return roots;
}

bool FileRoots::allowsRead(const QString &path) const { return insideAny(canonicalPath(path), read); }
bool FileRoots::allowsWrite(const QString &path) const { return insideAny(canonicalPath(path), write); }

Result<QString> FileRoots::checkRead(const QString &path, const QString &input) const {
    const QString canonical = canonicalPath(path);
    if (!insideAny(canonical, read))
        return outside(path, input, QStringLiteral("read"), read);
    return canonical;
}

Result<QString> FileRoots::checkWrite(const QString &path, const QString &input) const {
    const QString canonical = canonicalPath(path);
    if (!insideAny(canonical, write))
        return outside(path, input, QStringLiteral("write"), write);
    return canonical;
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
