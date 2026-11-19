#include "workflow/workflow_io.h"

#include "engine/embed.h"

#include <QFile>
#include <QFileInfo>

namespace fugue {

SourceKind detectKind(const QByteArray &head) {
    if (head.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8)))
        return SourceKind::Image;
    if (head.size() >= 12 && head.startsWith("RIFF") && head.mid(8, 4) == "WEBP")
        return SourceKind::Image;
    return SourceKind::Document;
}

Result<ImportedWorkflow> readWorkflowFile(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists() || info.isDir())
        return makeError(QStringLiteral("file_not_found"),
                         info.isDir() ? QStringLiteral("%1 is a folder, not a workflow file.").arg(path)
                                      : QStringLiteral("There is no file %1.").arg(path));
    // A pipe, socket or device is never opened: opening a pipe waits for a writer forever.
    if (!info.isFile())
        return makeError(QStringLiteral("file_not_found"), QStringLiteral("%1 is not a file.").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return makeError(QStringLiteral("permission_denied"),
                         QStringLiteral("%1 cannot be read: %2.").arg(path, file.errorString()));
    ImportedWorkflow imported;
    imported.kind = detectKind(file.read(16));
    if (imported.kind == SourceKind::Image) {
        file.close();
        const Result<QJsonObject> embedded = readEmbeddedWorkflow(path);
        if (!embedded.ok())
            return embedded.errors();
        Result<Document> document = Document::fromJson(embedded.value());
        if (!document.ok())
            return document.errors();
        imported.document = document.value();
        return imported;
    }
    // Refused before it is read: a video dropped by mistake is not loaded whole.
    if (info.size() > maxEmbeddedWorkflowBytes)
        return makeError(QStringLiteral("invalid_document"),
                         QStringLiteral("%1 is too big to be a workflow (%2 MB; at most %3 MB).")
                             .arg(info.fileName())
                             .arg(info.size() / (1024 * 1024))
                             .arg(maxEmbeddedWorkflowBytes / (1024 * 1024)));
    file.seek(0);
    Result<Document> document = Document::load(file.read(maxEmbeddedWorkflowBytes));
    if (!document.ok())
        return document.errors();
    imported.document = document.value();
    return imported;
}

Result<Document> readWorkflowText(const QString &text) {
    return Document::load(text.trimmed().toUtf8());
}

} // namespace fugue
