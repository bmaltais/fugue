// What every Save node shares (save_files.h). Moved out of Save Image unchanged, so the three nodes
// name, place and embed in one way.

#include "nodes/save_files.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>

#include "engine/embed.h"
#include "engine/file_roots.h"
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/share_copy.h"

namespace fugue::nodes {

namespace {

// What the folder input says, resolved and made (see saveLocation).
Result<QString> baseFolder(const NodeContext &context) {
    // Relative folders are under the output folder, so a workflow made elsewhere cannot
    // write outside it by accident.
    const QString typed = context.input("folder").text().trimmed();
    // The folder as the system resolves it (links followed where they stand, then `..`),
    // so making it, writing in it and checking it are about one folder. QDir::mkpath
    // would clean `link/..` away on paper while the write follows the link. Resolved once:
    // the folder that is checked is the folder that is returned.
    const bool relative = !typed.isEmpty() && QDir::isRelativePath(typed);
    const QString folder = canonicalPath(typed.isEmpty() ? outputDir() : relative ? outputDir() + QLatin1Char('/') + typed : typed);
    if (relative && !isInside(folder, canonicalPath(outputDir())))
        return makeInputError(QStringLiteral("permission_denied"), QStringLiteral("folder"),
                              QStringLiteral("%1 leads out of the output folder %2.").arg(typed, outputDir()),
                              QStringLiteral("A relative folder is under the output folder; write an absolute one to go elsewhere."));
    if (folder.isEmpty())
        return makeInputError(QStringLiteral("node_failed"), QStringLiteral("folder"),
                              QStringLiteral("The folder %1 goes through links that loop.").arg(typed));
    return folder;
}

Error badName(const QString &name, const QString &why) {
    return makeInputError(QStringLiteral("node_failed"), QStringLiteral("name"),
                          QStringLiteral("\"%1\" cannot be a file name: %2.").arg(name, why),
                          QStringLiteral("Use a plain name; `/` makes a subfolder, and {date}, {time} and {model} are filled in."));
}

} // namespace

QString modelLabel(const QJsonObject &handle) {
    QString name = handle.value("name").toString();
    name = name.mid(name.lastIndexOf(QRegularExpression(QStringLiteral("[/\\\\]"))) + 1);
    static const QStringList extensions{"gguf", "safetensors", "sft", "ckpt", "pt", "pth", "bin", "onnx"};
    if (const qsizetype dot = name.lastIndexOf(QLatin1Char('.')); dot > 0 && extensions.contains(name.mid(dot + 1).toLower()))
        name.truncate(dot);
    name.removeIf([](QChar c) { return c.category() == QChar::Other_Control; });
    for (QChar &c : name)
        if (!c.isLetterOrNumber() && !QStringLiteral("._-+() ").contains(c))
            c = QLatin1Char('_');
    name = name.trimmed();
    while (name.startsWith(QLatin1Char('.')))
        name.remove(0, 1);
    return name.isEmpty() ? QStringLiteral("unknown") : name;
}

Result<QString> expandSaveName(const QString &typed, const QDateTime &now, const QString &model, const QString &extension) {
    QString name = typed.trimmed();
    name.replace(QStringLiteral("{date}"), now.toString(QStringLiteral("yyyy-MM-dd")));
    name.replace(QStringLiteral("{time}"), now.toString(QStringLiteral("hh-mm-ss")));
    name.replace(QStringLiteral("{model}"), model);
    if (name.contains(QLatin1Char('\\')))
        return badName(name, QStringLiteral("a backslash is not allowed"));
    for (const QChar c : std::as_const(name))
        if (c.category() == QChar::Other_Control)
            return badName(QStringLiteral("(a name with a control character)"),
                           QStringLiteral("a control character is not allowed"));
    const QStringList parts = name.split(QLatin1Char('/'));
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QString &part = parts.at(i);
        if (part.isEmpty())
            return badName(name, name.isEmpty() ? QStringLiteral("it is empty") : QStringLiteral("it is absolute or has an empty folder"));
        if (part.startsWith(QLatin1Char('.')))
            return badName(name, QStringLiteral("a part starting with a dot (. or ..) is not allowed"));
        // A file name holds at most 255 bytes (NAME_MAX), the file's with its extension.
        const QString full = i == parts.size() - 1 ? part + QLatin1Char('.') + extension : part;
        if (const qsizetype bytes = QFile::encodeName(full).size(); bytes > 255)
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("name"),
                                  QStringLiteral("The name is too long for a file name: %1 bytes with .%2, at most 255.")
                                      .arg(bytes)
                                      .arg(extension),
                                  QStringLiteral("Use a shorter name."));
    }
    return name;
}

Result<SaveLocation> saveLocation(const NodeContext &context, const QString &extension) {
    const Result<QString> relative = expandSaveName(context.input("name").text(), QDateTime::currentDateTime(),
                                                    modelLabel(context.upstreamModel), extension);
    if (!relative)
        return relative.errors();
    const Result<QString> base = baseFolder(context);
    if (!base)
        return base.errors();
    const qsizetype slash = relative->lastIndexOf(QLatin1Char('/'));
    const auto checked = [&](const QString &folder, const QString &input) -> Result<QString> {
        if (!context.roots)
            return folder;
        // Write into the path that was checked.
        return context.roots->checkWrite(folder, input);
    };
    // The write roots only say yes or no: the folder opened is the one resolved above, never a
    // second resolve of it.
    const Result<QString> baseOk = checked(base.value(), QStringLiteral("folder"));
    if (!baseOk)
        return baseOk.errors();
    if (baseOk.value() != base.value())
        return makeInputError(QStringLiteral("permission_denied"), QStringLiteral("folder"),
                              QStringLiteral("The folder %1 changed while it was checked.").arg(base.value()));
    QString folder = baseOk.value();
    if (slash >= 0) {
        // The subfolders as the system resolves them: a link inside the folder that leads out
        // is judged by where it leads.
        const QString wanted = canonicalPath(base.value() + QLatin1Char('/') + relative->left(slash));
        if (wanted.isEmpty())
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("name"),
                                  QStringLiteral("\"%1\" goes through links that loop.").arg(relative.value()),
                                  QStringLiteral("Use another name."));
        if (!isInside(wanted, base.value()))
            return makeInputError(QStringLiteral("permission_denied"), QStringLiteral("name"),
                                  QStringLiteral("\"%1\" leads out of the folder %2.").arg(relative.value(), base.value()),
                                  QStringLiteral("A name's folders stay inside the folder the file is saved in."));
        const Result<QString> subOk = checked(wanted, QStringLiteral("name"));
        if (!subOk)
            return subOk.errors();
        if (subOk.value() != wanted)
            return makeInputError(QStringLiteral("permission_denied"), QStringLiteral("name"),
                                  QStringLiteral("The folder %1 changed while it was checked.").arg(wanted));
        folder = wanted;
    }
    // Opened once, here: from now on the folders are held by descriptor, not by name. The base
    // is opened (and made) from the root without following a link; the name's subfolders below it.
    Result<FolderFd> opened = FolderFd::open(baseOk.value(), true);
    if (!opened) {
        Errors made = opened.errors();
        made.first().input = QStringLiteral("folder");
        made.first().hint = QStringLiteral("Pick another folder in the node.");
        return made;
    }
    if (folder != baseOk.value()) {
        // `folder` is inside the base after links were followed, so the way down is the path
        // between them; a link met there now (one swapped in since) is refused.
        const QString prefix = baseOk.value().endsWith(QLatin1Char('/')) ? baseOk.value() : baseOk.value() + QLatin1Char('/');
        if (!folder.startsWith(prefix))
            return makeInputError(QStringLiteral("permission_denied"), QStringLiteral("name"),
                                  QStringLiteral("\"%1\" is not inside the folder %2 any more.").arg(relative.value(), baseOk.value()));
        opened = opened->below(folder.mid(prefix.size()), true);
        if (!opened) {
            Errors made = opened.errors();
            made.first().input = QStringLiteral("name");
            made.first().hint = QStringLiteral("Use another name: a folder in it may be a file already.");
            return made;
        }
    }
    return SaveLocation{folder, relative->mid(slash + 1), std::make_shared<FolderFd>(std::move(opened.value()))};
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
    if (n == 1)
        return QStringLiteral("%1.%2").arg(name, extension);
    // The suffix takes room from the name: a name that fits (255 bytes) with no suffix would not
    // with `-100000`, so its end is cut (at a character) to make the numbered one fit too.
    const QString tail = QStringLiteral("-%1.%2").arg(n).arg(extension);
    QString stem = name;
    const qsizetype room = 255 - QFile::encodeName(tail).size();
    while (!stem.isEmpty() && QFile::encodeName(stem).size() > room)
        stem.chop(1);
    if (!stem.isEmpty() && stem.back().isHighSurrogate())
        stem.chop(1);
    return stem + tail;
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

Result<QString> saveAsNew(const SaveLocation &where, const QString &extension, const QByteArray &bytes) {
    PartialFile::removeStale(*where.dir); // what a save that died here left behind
    PartialFile partial(*where.dir, numberedName(where.name, 1, extension));
    if (const Errors written = partial.write(bytes, QDir(where.folder).filePath(numberedName(where.name, 1, extension)));
        hasErrors(written))
        return written;
    return commitAsNew(partial, where.folder, where.name, extension);
}

} // namespace fugue::nodes
