// `fugue project list|show|new|add|remove|inherits` (spec 1.4.2, 13.1, epic #204): the project registry
// and project.json files. The work is engine/project.h's; this reads the arguments and prints. The
// registry is always the ordinary session's (the base config folder), also from inside a project.
// Nothing here deletes a project's files.

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include "cli/commands.h"
#include "engine/fugue_home.h"
#include "engine/project.h"

namespace fugue::cli {

namespace {

bool isCurrent(const ProjectEntry &entry) { return !fugueHome().isEmpty() && QDir::cleanPath(fugueHome()) == entry.root; }

QJsonObject entryJson(const ProjectEntry &entry) {
    QJsonObject object{{"name", entry.name()},
                       {"path", entry.root},
                       {"state", projectStateName(entry.state)},
                       {"inherits", QJsonArray::fromStringList(entry.manifest.inherits)},
                       {"registered", entry.registered},
                       {"current", isCurrent(entry)}};
    if (!entry.manifest.ignored.isEmpty())
        object.insert("ignored", QJsonArray::fromStringList(entry.manifest.ignored));
    return object;
}

QString inheritsText(const QStringList &items) { return items.isEmpty() ? QStringLiteral("nothing") : items.join(QStringLiteral(", ")); }

QString line(const ProjectEntry &entry) {
    QString text = QStringLiteral("%1 %2").arg(isCurrent(entry) ? QStringLiteral("*") : QStringLiteral(" "), entry.name());
    if (entry.state != ProjectState::Ok)
        text += QStringLiteral(" [%1]").arg(projectStateName(entry.state));
    return text + QStringLiteral("  ") + entry.root;
}

QStringList items(const QString &text) {
    if (text.trimmed() == QLatin1String("none"))
        return {};
    return text.split(QLatin1Char(','), Qt::SkipEmptyParts);
}

// What a project's words are when they are warnings of the call.
Reply withWarnings(Reply reply, const Errors &warnings) {
    reply.errors = warnings;
    return reply;
}

} // namespace

Reply projectList(const Command &, Context &) {
    const Result<QList<ProjectEntry>> entries = listProjects();
    if (!entries)
        return Reply::failure(entries.errors());
    QJsonArray listed;
    QStringList lines{QStringLiteral("%1 default  (the ordinary session)").arg(fugueHome().isEmpty() ? QStringLiteral("*") : QStringLiteral(" "))};
    for (const ProjectEntry &entry : *entries) {
        listed.append(entryJson(entry));
        lines << line(entry);
    }
    return Reply::success(QJsonObject{{"projects", listed}, {"default_current", fugueHome().isEmpty()}}, lines.join('\n'));
}

Reply projectShow(const Command &command, Context &) {
    if (command.positionals.isEmpty()) {
        if (fugueHome().isEmpty())
            return Reply::success(QJsonObject{{"name", "default"}, {"path", QJsonValue::Null}, {"state", "ok"}, {"inherits", QJsonArray()}, {"registered", false}, {"current", true}},
                                  QStringLiteral("default: the ordinary session (no project is selected)."));
        ProjectEntry entry = inspectProject(fugueHome());
        if (entry.state == ProjectState::Missing)
            return Reply::success(QJsonObject{{"name", QJsonValue::Null}, {"path", fugueHome()}, {"state", "ok"}, {"inherits", QJsonArray()}, {"registered", false}, {"current", true}},
                                  QStringLiteral("%1 is a root with no project.json: it inherits nothing.").arg(fugueHome()));
        const Result<QList<ProjectEntry>> listed = listProjects();
        if (listed)
            for (const ProjectEntry &candidate : *listed)
                entry.registered = entry.registered || candidate.root == entry.root;
        return withWarnings(Reply::success(entryJson(entry),
                                           QStringLiteral("%1 (%2)\n  inherits: %3").arg(entry.name(), entry.root, inheritsText(entry.manifest.inherits))),
                            entry.problems);
    }
    const QString which = command.positionals.at(0);
    if (which.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0)
        return Reply::success(QJsonObject{{"name", "default"}, {"path", QJsonValue::Null}, {"state", "ok"}, {"inherits", QJsonArray()}, {"registered", false}, {"current", fugueHome().isEmpty()}},
                              QStringLiteral("default: the ordinary session."));
    const Result<ProjectEntry> entry = findProject(which);
    if (!entry)
        return Reply::failure(entry.errors());
    QString text = QStringLiteral("%1 (%2)\n  state: %3\n  inherits: %4\n  registered: %5")
                       .arg(entry->name(), entry->root, projectStateName(entry->state), inheritsText(entry->manifest.inherits), entry->registered ? QStringLiteral("yes") : QStringLiteral("no"));
    return withWarnings(Reply::success(entryJson(*entry), text), entry->problems);
}

Reply projectNew(const Command &command, Context &) {
    const QStringList inherits = command.has("inherits") ? items(command.value("inherits")) : defaultInherits();
    const Result<ProjectEntry> made = createProject(command.positionals.at(0), command.value("in"), inherits);
    if (!made)
        return Reply::failure(made.errors());
    return withWarnings(Reply::success(entryJson(*made), QStringLiteral("Made the project %1 in %2, inheriting %3.").arg(made->name(), made->root, inheritsText(made->manifest.inherits))),
                        made.warnings());
}

Reply projectAdd(const Command &command, Context &) {
    const Result<ProjectEntry> added = addProject(command.positionals.at(0));
    if (!added)
        return Reply::failure(added.errors());
    return withWarnings(Reply::success(entryJson(*added), QStringLiteral("Registered the project %1 (%2).").arg(added->name(), added->root)), added.warnings());
}

Reply projectRemove(const Command &command, Context &) {
    const Result<ProjectEntry> removed = removeProject(command.positionals.at(0));
    if (!removed)
        return Reply::failure(removed.errors());
    return Reply::success(QJsonObject{{"removed", removed->name()}, {"path", removed->root}},
                          QStringLiteral("Removed %1 from the list. Its folder %2 and everything in it are untouched.").arg(removed->name(), removed->root));
}

Reply projectInherits(const Command &command, Context &) {
    const Result<ProjectEntry> changed = setProjectInherits(command.positionals.at(0), items(command.positionals.at(1)));
    if (!changed)
        return Reply::failure(changed.errors());
    return withWarnings(Reply::success(entryJson(*changed), QStringLiteral("%1 inherits %2.").arg(changed->name(), inheritsText(changed->manifest.inherits))), changed.warnings());
}

} // namespace fugue::cli
