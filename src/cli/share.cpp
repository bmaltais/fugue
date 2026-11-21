// `fugue workflow export <file> --share` (spec 11.5, 13.1, #91): the workflow packed to share,
// as File > Export for sharing writes it (engine/share_package.h).

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

#include "cli/commands.h"
#include "engine/document.h"
#include "engine/share_package.h"

namespace fugue::cli {

Reply workflowExport(const Command &command, Context &context) {
    if (!command.has("share"))
        return Reply::failure(makeError("usage", "Only a share copy can be exported: add --share.",
                                        "fugue workflow export <file> --share [--out <file.fuguepack>]"));
    const QString file = command.positionals.at(0);
    const Result<Document> document = Document::loadFile(file);
    if (!document)
        return Reply::failure(document.errors());
    const QFileInfo info(file);
    QString out = command.has("out") ? command.value("out") : info.completeBaseName() + QStringLiteral(".fuguepack");
    if (QFileInfo(out).suffix().compare(QLatin1String("fuguepack"), Qt::CaseInsensitive) != 0)
        out += QStringLiteral(".fuguepack"); // its own extension, which the desktop opens with Fugue
    out = QFileInfo(out).absoluteFilePath();

    ShareRequest request;
    request.title = info.completeBaseName();
    request.workflowPath = info.absoluteFilePath();
    request.options.withoutPrompts = command.has("no-prompts");
    request.options.withoutLayout = command.has("no-layout");
    request.appVersion = QCoreApplication::applicationVersion();
    const Errors errors = writeSharePackage(*document, context.registry, request, out);
    if (hasErrors(errors))
        return Reply::failure(errors);
    Reply reply = Reply::success(QJsonObject{{"file", out}},
                                 QStringLiteral("Wrote %1. Models are not in it: who opens it can download them.").arg(out));
    reply.errors = errors; // warnings: a file not there
    return reply;
}

} // namespace fugue::cli
