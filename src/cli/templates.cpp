// `fugue template list` and `fugue workflow new <file> --template <id>` (spec 11.10, 13.1): the
// templates the window lists, from the same TemplateLibrary.

#include <QJsonArray>
#include <QJsonObject>

#include "cli/args.h"
#include "cli/commands.h"
#include "engine/file_hashes.h"
#include "engine/model_library.h"
#include "engine/templates.h"

namespace fugue::cli {

Reply templateList(const Command &, Context &context) {
    const TemplateLibrary library(TemplateLibrary::standardRoots(), &context.registry);
    FileHashes hashes(FileHashes::defaultPath());
    const ModelLibrary models = ModelLibrary::current(&context.registry);
    QJsonArray listed;
    QStringList lines;
    for (const Template &t : library.list()) {
        QJsonArray needs;
        int missing = 0;
        const QList<MissingModel> absent = models.missing(t.models, hashes);
        for (const QJsonValue model : t.models) {
            QJsonObject entry = model.toObject();
            bool here = true;
            for (const MissingModel &gone : absent)
                if (gone.handle.value("hash") == entry.value("hash")) {
                    here = false;
                    entry.insert("folder", gone.folder);
                }
            entry.insert("here", here);
            missing += here ? 0 : 1;
            needs.append(entry);
        }
        listed.append(QJsonObject{{"id", t.id},
                                  {"title", t.title},
                                  {"description", t.description},
                                  {"category", t.category},
                                  {"tags", QJsonArray::fromStringList(t.tags)},
                                  {"own", t.own},
                                  {"folder", t.folder},
                                  {"workflow", t.workflow},
                                  {"preview", t.preview},
                                  {"models", needs},
                                  {"missing_packs", QJsonArray::fromStringList(t.missingPacks)}});
        QString line = QStringLiteral("%1  %2").arg(t.id, t.title);
        if (missing)
            line += QStringLiteral("  (%1 of %2 models missing)").arg(missing).arg(t.models.size());
        if (!t.missingPacks.isEmpty())
            line += QStringLiteral("  (needs %1)").arg(t.missingPacks.join(QStringLiteral(", ")));
        lines << line;
    }
    // What is wrong with a template (a broken one is left out): warnings, so the templates
    // repo's check can fail on them while a person's list still prints.
    Reply reply = Reply::success(QJsonObject{{"templates", listed}}, lines.join('\n'));
    for (Error problem : library.problems()) {
        problem.severity = Error::Severity::Warning;
        reply.errors.append(problem);
    }
    return reply;
}

Reply workflowFromTemplate(const Command &command, Context &context) {
    const QString path = command.positionals.at(0);
    const TemplateLibrary library(TemplateLibrary::standardRoots(), &context.registry);
    const std::optional<Template> chosen = library.find(command.value("template"));
    if (!chosen) {
        Error error = makeError("usage", QStringLiteral("There is no template \"%1\".").arg(command.value("template")),
                                "Use the id of one `fugue template list` shows.");
        for (const Template &t : library.list())
            error.valid.append(t.id);
        return Reply::failure(error);
    }
    const Errors errors = library.copyTo(*chosen, path);
    if (!errors.isEmpty())
        return Reply::failure(errors);
    return Reply::success(QJsonObject{{"file", path}, {"template", chosen->id}},
                          QStringLiteral("Wrote %1 from the template %2.").arg(path, chosen->title));
}

} // namespace fugue::cli
