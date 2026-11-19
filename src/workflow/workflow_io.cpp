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
    file.seek(0);
    Result<Document> document = Document::load(file.readAll());
    if (!document.ok())
        return document.errors();
    imported.document = document.value();
    return imported;
}

Result<Document> readWorkflowText(const QString &text) {
    return Document::load(text.trimmed().toUtf8());
}

} // namespace fugue
