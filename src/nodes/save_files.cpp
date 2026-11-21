// What every Save node shares (save_files.h). Moved out of Save Image unchanged, so the three nodes
// name, place and embed in one way.

#include "nodes/save_files.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>

#include "engine/embed.h"
#include "engine/file_roots.h"
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/share_copy.h"

namespace fugue::nodes {

Result<QString> saveName(const NodeContext &context, const QString &extension) {
    const QString name = context.input("name").text().trimmed();
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name.startsWith(QLatin1Char('.')))
        return makeInputError(QStringLiteral("node_failed"), QStringLiteral("name"),
                              QStringLiteral("\"%1\" cannot be a file name.").arg(name),
                              QStringLiteral("Use a plain name without folders; the folder input says where."));
    // A file name holds at most 255 bytes (NAME_MAX) with its extension.
    if (const qsizetype bytes = QFile::encodeName(name + QLatin1Char('.') + extension).size(); bytes > 255)
        return makeInputError(QStringLiteral("node_failed"), QStringLiteral("name"),
                              QStringLiteral("The name is too long for a file name: %1 bytes with .%2, at most 255.")
                                  .arg(bytes)
                                  .arg(extension),
                              QStringLiteral("Use a shorter name."));
    return name;
}

Result<QString> saveFolder(const NodeContext &context) {
    // Relative folders are under the output folder, so a workflow made elsewhere cannot
    // write outside it by accident.
    QString folder = context.input("folder").text().trimmed();
    if (folder.isEmpty()) {
        folder = outputDir();
    } else if (QDir::isRelativePath(folder)) {
        folder = outputDir() + QLatin1Char('/') + folder;
        if (!isInside(canonicalPath(folder), canonicalPath(outputDir())))
            return makeInputError(QStringLiteral("permission_denied"), QStringLiteral("folder"),
                                  QStringLiteral("%1 leads out of the output folder %2.").arg(context.input("folder").text().trimmed(), outputDir()),
                                  QStringLiteral("A relative folder is under the output folder; write an absolute one to go elsewhere."));
    }
    // The folder as the system resolves it (links followed where they stand, then `..`),
    // so making it, writing in it and checking it are about one folder. QDir::mkpath
    // would clean `link/..` away on paper while the write follows the link.
    folder = canonicalPath(folder);
    if (folder.isEmpty())
        return makeInputError(QStringLiteral("node_failed"), QStringLiteral("folder"),
                              QStringLiteral("The folder %1 goes through links that loop.").arg(context.input("folder").text().trimmed()));
    if (context.roots) {
        // Write into the path that was checked.
        const Result<QString> writable = context.roots->checkWrite(folder, QStringLiteral("folder"));
        if (!writable)
            return writable.errors();
        folder = writable.value();
    }
    if (Errors made = makeFolder(folder); !made.isEmpty()) {
        made.first().input = QStringLiteral("folder");
        made.first().hint = QStringLiteral("Pick another folder in the node.");
        return made;
    }
    return folder;
}

Result<QByteArray> workflowToEmbed(const NodeContext &context, const QString &embed, const QString &what) {
    if (embed == QLatin1String("none"))
        return QByteArray();
    if (context.document.isEmpty() || !context.registry) {
        Error warning = makeError(QStringLiteral("workflow_not_embedded"),
                                  QStringLiteral("The %1 holds no workflow: the run started from a compiled graph, "
                                                 "which has none to embed.").arg(what),
                                  QStringLiteral("Run the workflow file itself to embed it."));
        warning.severity = Error::Severity::Warning;
        return Result<QByteArray>(QByteArray(), Errors{warning});
    }
    ShareOptions options;
    options.withoutPrompts = embed == QLatin1String("no-prompts");
    QJsonObject document = shareCopy(context.document, *context.registry, options);
    document.insert("run", QJsonObject{{"nodes", context.usedUpstream}});
    return compactJson(document);
}

QString numberedName(const QString &name, int n, const QString &extension) {
    return n == 1 ? QStringLiteral("%1.%2").arg(name, extension) : QStringLiteral("%1-%2.%3").arg(name).arg(n).arg(extension);
}

Result<QString> commitAsNew(PartialFile &partial, const QString &folder, const QString &name, const QString &extension) {
    for (int n = 1; n <= 100000; ++n) {
        const QString target = QDir(folder).filePath(numberedName(name, n, extension));
        const Result<bool> committed = partial.commit(target, Overwrite::Never);
        if (!committed)
            return committed.errors();
        if (*committed)
            return target;
    }
    return makeError(QStringLiteral("node_failed"), QStringLiteral("No free name for %1 in %2.").arg(name, folder),
                     QStringLiteral("Pick another name or folder in the node."));
}

Result<QString> saveAsNew(const QString &folder, const QString &name, const QString &extension, const QByteArray &bytes) {
    PartialFile::removeStale(folder); // what a save that died here left behind
    PartialFile partial(folder, numberedName(name, 1, extension));
    if (const Errors written = partial.write(bytes, QDir(folder).filePath(numberedName(name, 1, extension)));
        hasErrors(written))
        return written;
    return commitAsNew(partial, folder, name, extension);
}

} // namespace fugue::nodes
