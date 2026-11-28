// `fugue template list` and `fugue workflow new <file> --template <id>` (spec 11.10, 13.1): the
// templates the window lists, from the same TemplateLibrary.

#include <QJsonArray>
#include <QJsonObject>

#include "cli/args.h"
#include "cli/commands.h"
#include "engine/file_hashes.h"
#include "engine/model_library.h"
#include "engine/template_feed.h"
#include "engine/templates.h"

namespace fugue::cli {

namespace {

// The packs the template folder carries (11.10), as data.
QJsonArray carriedAsJson(const Template &t) {
    QJsonArray out;
    for (const CarriedPack &pack : t.carriedPacks)
        out.append(QJsonObject{{"id", pack.id}, {"version", pack.version}, {"folder", pack.folder}});
    return out;
}

// `false` for a template that is not an app, else its card's words (11.10, #387).
QJsonValue appAsJson(const Template &t) {
    if (!t.app)
        return false;
    return QJsonObject{{"title", t.app->title}, {"summary", t.app->summary}, {"kind", t.app->kind}};
}

// `false` for a template that is not a community one, else the author and the issue it came in by (11.10).
QJsonValue communityAsJson(const Template &t) {
    if (!t.community)
        return false;
    QJsonObject out;
    if (!t.community->author.isEmpty())
        out.insert("author", t.community->author);
    if (t.community->issue > 0) {
        out.insert("issue", t.community->issue);
        out.insert("url", t.community->issueUrl());
    }
    return out;
}

} // namespace

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
                                  {"source", t.source},
                                  {"families", QJsonArray::fromStringList(t.families)},
                                  {"app", appAsJson(t)},
                                  {"community", communityAsJson(t)},
                                  {"canvas_action", t.canvasAction},
                                  {"own", t.own},
                                  {"inherited", t.inherited},
                                  {"online", t.online},
                                  {"origin", t.inherited ? QStringLiteral("inherited from Default") : t.own ? QStringLiteral("own")
                                                                                                          : t.online ? QStringLiteral("online") : QStringLiteral("bundled")},
                                  {"folder", t.folder},
                                  {"workflow", t.workflow},
                                  {"preview", t.preview},
                                  {"models", needs},
                                  {"needed_packs", QJsonArray::fromStringList(t.neededPacks)},
                                  {"carried_packs", carriedAsJson(t)}});
        QString line = QStringLiteral("%1  %2").arg(t.id, t.title);
        if (t.inherited)
            line += QStringLiteral("  [inherited from Default]");
        if (t.canvasAction)
            line += QStringLiteral("  [canvas action]");
        if (t.community)
            line += t.community->author.isEmpty() ? QStringLiteral("  [community]") : QStringLiteral("  [community, by %1]").arg(t.community->author);
        if (missing)
            line += QStringLiteral("  (%1 of %2 models missing)").arg(missing).arg(t.models.size());
        if (!t.neededPacks.isEmpty())
            line += QStringLiteral("  (needs %1)").arg(t.neededPacks.join(QStringLiteral(", ")));
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

// `fugue template update` (#699): fetch the templates now, whatever settings.json's fetch_templates says (the person asked).
Reply templateUpdate(const Command &, Context &) {
    if (TemplateFeed::disabledByEnvironment())
        return Reply::failure(makeError("usage", QStringLiteral("FUGUE_TEMPLATES is set, so Fugue reads that folder and never fetches templates."),
                                        QStringLiteral("Unset FUGUE_TEMPLATES to fetch.")));
    const FeedResult result = TemplateFeed::checkNow();
    if (result.outcome == FeedResult::Outcome::Busy)
        return Reply::success(QJsonObject{{"result", "busy"}, {"commit", result.sha}},
                              QStringLiteral("Another Fugue is updating the templates; nothing was done here."));
    if (result.outcome == FeedResult::Outcome::Failed)
        return Reply::failure(makeError("download_failed", result.error, QStringLiteral("The templates in use are unchanged.")));
    const bool updated = result.outcome == FeedResult::Outcome::Updated;
    QJsonObject data{{"result", updated ? "updated" : "current"},
                     {"commit", result.sha},
                     {"previous", result.previous},
                     {"folder", TemplateFeed::currentRoot()}};
    if (updated) {
        data.insert("files", result.files);
        data.insert("downloaded", result.downloaded);
    }
    return Reply::success(data, updated ? QStringLiteral("Templates updated to %1: %2 of %3 files downloaded.")
                                                .arg(result.sha.left(10)).arg(result.downloaded).arg(result.files)
                                        : QStringLiteral("Templates are up to date at %1.").arg(result.sha.left(10)));
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
    // What it still needs here, as opening it in the window says (4.3).
    FileHashes hashes(FileHashes::defaultPath());
    QJsonArray missing;
    QStringList lines{QStringLiteral("Wrote %1 from the template %2.").arg(path, chosen->title)};
    for (const MissingModel &model : ModelLibrary::current(&context.registry).missing(chosen->models, hashes)) {
        QJsonObject entry = model.handle;
        entry.insert("source", model.source);
        entry.insert("folder", model.folder);
        missing.append(entry);
        lines << QStringLiteral("Missing model %1: get it from %2 and put it in %3.")
                     .arg(model.handle.value("name").toString(),
                          model.source.isEmpty() ? QStringLiteral("its maker") : model.source, model.folder);
    }
    // A pack it carries that is not installed: never installed from here without the question
    // `fugue pack add` asks (6.6), so say how.
    const QStringList needed = chosen->neededPacks;
    QJsonArray carried;
    for (const CarriedPack &pack : chosen->carriedPacks) {
        if (!needed.contains(pack.id))
            continue;
        carried.append(QJsonObject{{"id", pack.id}, {"version", pack.version}, {"folder", pack.folder}});
        lines << QStringLiteral("The template carries the pack %1 %2, which its nodes need: run `fugue pack add %3`, then `fugue pack trust %1`.")
                     .arg(pack.id, pack.version, pack.folder);
    }
    return Reply::success(QJsonObject{{"file", path}, {"template", chosen->id}, {"missing_models", missing},
                                      {"carried_packs", carried}},
                          lines.join('\n'));
}

} // namespace fugue::cli
