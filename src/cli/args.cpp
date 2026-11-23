#include "cli/args.h"

#include "cli/commands.h"
#include "cli/runs.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

namespace fugue::cli {

namespace {

struct Spec {
    int minPositionals = 0;
    int maxPositionals = 0;   // -1: no limit
    QStringList valueOptions; // --name <value> or --name=<value>
    QStringList flagOptions;  // --name
    Handler handler = nullptr; // null: a later milestone, parsed but not run
};

const Spec runSpec{1, 1, {"to", "provider", "set"}, {"wait", "first"}, startRun};
const Spec validateSpec{1, 1, {}, {}, validateWorkflow};
const Spec compileSpec{1, 1, {}, {}, compileWorkflow};

// The commands of 13.1 by the first word. A group with an empty-named entry in
// `subs` takes no subcommand.
const QMap<QString, QMap<QString, Spec>> &table() {
    static const QMap<QString, QMap<QString, Spec>> commands{
        {"run", {{"", runSpec}}},
        {"validate", {{"", validateSpec}}},
        {"compile", {{"", compileSpec}}},
        {"workflow",
         {{"new", {1, 1, {"template"}, {}, workflowNew}},
          {"describe", {1, 1, {}, {}, workflowDescribe}},
          {"export", {1, 1, {"out"}, {"share", "no-prompts", "no-layout"}, workflowExport}},
          {"run", runSpec},
          {"validate", validateSpec},
          {"compile", compileSpec}}},
        {"runs",
         {{"list", {0, 0, {}, {}, runsList}},
          {"status", {1, 1, {}, {}, runsStatus}},
          {"events", {1, 1, {}, {"follow"}, runsEvents}},
          {"result", {1, 1, {}, {}, runsResult}},
          {"cancel", {1, 1, {}, {}, cancelRun}}}},
        {"node",
         {{"list", {0, 0, {"type"}, {}, nodeList}},
          {"describe", {1, -1, {}, {}, nodeDescribe}},
          {"new", {1, 1, {"id"}, {}, nodeNew}},
          {"test", {1, 1, {}, {}, nodeTest}}}},
        {"pack",
         {{"list", {0, 0, {}, {}, packList}},
          {"add", {1, 1, {"ref"}, {}, packAdd}},
          {"remove", {1, 1, {}, {}, packRemove}},
          {"update", {0, 1, {"ref"}, {}, packUpdate}},
          {"trust", {1, 1, {}, {}, packTrust}}}},
        {"provider", {{"list", {0, 0, {}, {}, nullptr}}}},
        {"model", {{"list", {0, 0, {"type"}, {"hash"}, modelList}}, {"get", {1, 1, {"into"}, {}, modelGet}}}},
        {"template", {{"list", {0, 0, {}, {}, templateList}}}},
        {"cache",
         {{"status", {0, 0, {}, {}, cacheStatus}},
          {"clear", {0, 0, {"kind", "workflow", "node", "older-than"}, {"dry-run"}, cacheClear}}}},
        {"project",
         {{"list", {0, 0, {}, {}, projectList}},
          {"show", {0, 1, {}, {}, projectShow}},
          {"new", {1, 1, {"in", "inherits"}, {}, projectNew}},
          {"add", {1, 1, {}, {}, projectAdd}},
          {"remove", {1, 1, {}, {}, projectRemove}},
          {"inherits", {2, 2, {}, {}, projectInherits}}}},
        {"canvas",
         {{"new", {0, 0, {"name", "size", "background"}, {}, canvasNew}},
          {"import", {1, 1, {"name"}, {}, canvasImport}},
          {"export", {1, 2, {"format", "quality", "scale", "size", "width", "height", "background", "prefix"}, {"layers", "history", "estimate", "lossless", "only-visible", "no-masks"}, canvasExport}},
          {"batch", {3, 3, {"format", "quality", "scale", "size", "width", "height", "background", "prompt", "seed"}, {"lossless", "keep"}, canvasBatch}},
          {"log", {1, 1, {"limit"}, {}, canvasLog}},
          {"show", {1, 1, {}, {}, canvasShow}},
          {"list", {0, 0, {}, {}, canvasList}},
          {"candidates", {1, 1, {}, {"all"}, canvasCandidates}},
          {"accept", {1, 1, {}, {}, canvasAccept}},
          {"drop", {1, 1, {}, {}, canvasDrop}},
          {"diff", {1, 2, {}, {}, canvasDiff}},
          {"layer", {2, -1, {"to", "visible", "opacity", "blend", "mask", "lock", "name"}, {}, canvasLayer}},
          {"resize", {1, 1, {"to", "fill", "blur"}, {}, canvasResize}},
          {"undo", {1, 1, {}, {}, canvasUndo}},
          {"redo", {1, 1, {}, {}, canvasRedo}}}},
        {"settings", {{"list", {0, 0, {}, {}, settingsList}}}},
        {"command", {{"list", {0, 0, {}, {}, nullptr}}}},
        {"skill",
         {{"install", {0, 0, {"target"}, {"dry-run", "force"}, skillInstall}},
          {"uninstall", {0, 0, {"target"}, {"dry-run", "force"}, skillUninstall}}}},
        // Not for people: what `fugue run` starts, detached, to run one queued run.
        {"__worker", {{"", {1, 1, {}, {}, runWorker}}}},
    };
    return commands;
}

// Spec 13.1: the plural is an alias for these five only.
QString canonicalGroup(const QString &word) {
    if (word == "nodes" || word == "packs" || word == "providers" || word == "models" || word == "templates")
        return word.chopped(1);
    return word;
}

Error usage(const QString &message, const QString &hint = {}, const QStringList &valid = {}) {
    Error error = makeError("usage", message, hint);
    for (const QString &item : valid)
        error.valid.append(item);
    return error;
}

QString quoted(const QString &text) { return QStringLiteral("\"%1\"").arg(text); }

} // namespace

QList<CommandInfo> implementedCommands() {
    QList<CommandInfo> listed;
    QSet<QString> seen;
    for (auto group = table().constBegin(); group != table().constEnd(); ++group) {
        if (group.key().startsWith("__"))
            continue;
        for (auto sub = group->constBegin(); sub != group->constEnd(); ++sub) {
            // `workflow run|validate|compile` are `run|validate|compile`, listed once.
            const bool alias = group.key() == "workflow" && (sub.key() == "run" || sub.key() == "validate" || sub.key() == "compile");
            const QString name = sub.key().isEmpty() ? group.key() : group.key() + "." + sub.key();
            if (!sub->handler || alias || seen.contains(name))
                continue;
            seen.insert(name);
            listed.append({name, sub->valueOptions, sub->flagOptions});
        }
    }
    return listed;
}

Result<Command> parseArguments(const QStringList &rawArgs) {
    Command command;
    QStringList args;
    for (const QString &arg : rawArgs) {
        if (arg == "--json")
            command.json = true;
        else
            args << arg;
    }

    // The command words come first, then the options and positionals of that command.
    if (args.isEmpty() || args.first().startsWith('-'))
        return usage("No command given.", "Try `fugue node list` or `fugue runs list`.", table().keys());
    const QString group = canonicalGroup(args.takeFirst());
    const auto entry = table().constFind(group);
    if (entry == table().constEnd())
        return usage(QStringLiteral("Unknown command %1.").arg(quoted(group)), {}, table().keys());
    command.topic = group;

    QString sub;
    // `fugue canvas --help`: the group's usage, with no subcommand to name.
    if (!entry->contains("") && !args.isEmpty() && (args.first() == "--help" || args.first() == "-h")) {
        command.help = true;
        command.name = group;
        return command;
    }
    if (!entry->contains("")) {
        if (args.isEmpty() || args.first().startsWith('-'))
            return usage(QStringLiteral("%1 needs a subcommand.").arg(quoted(group)), {}, entry->keys());
        sub = args.takeFirst();
        if (!entry->contains(sub))
            return usage(QStringLiteral("Unknown subcommand %1 of %2.").arg(quoted(sub), quoted(group)), {},
                         entry->keys());
    }
    const Spec spec = entry->value(sub);
    // `workflow run|validate|compile` are `run|validate|compile`.
    command.name = group == "workflow" && (sub == "run" || sub == "validate" || sub == "compile")
                       ? sub
                       : (sub.isEmpty() ? group : group + "." + sub);
    command.handler = spec.handler;

    bool optionsDone = false;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (optionsDone || !arg.startsWith('-') || arg == "-") {
            command.positionals << arg;
            continue;
        }
        if (arg == "--") {
            optionsDone = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            command.help = true;
            continue;
        }
        if (!arg.startsWith("--"))
            return usage(QStringLiteral("Unknown option %1.").arg(quoted(arg)));
        QString name = arg.mid(2);
        QString attached;
        const bool hasAttached = name.contains('=');
        if (hasAttached) {
            attached = name.mid(name.indexOf('=') + 1);
            name = name.left(name.indexOf('='));
        }
        if (spec.flagOptions.contains(name)) {
            if (hasAttached)
                return usage(QStringLiteral("--%1 takes no value.").arg(name));
            command.options[name] << QString();
        } else if (spec.valueOptions.contains(name)) {
            if (hasAttached) {
                command.options[name] << attached;
            } else if (i + 1 < args.size()) {
                command.options[name] << args.at(++i);
            } else {
                return usage(QStringLiteral("--%1 needs a value.").arg(name));
            }
        } else {
            QStringList valid;
            for (const QString &option : spec.valueOptions + spec.flagOptions)
                valid << "--" + option;
            return usage(QStringLiteral("Unknown option %1 for `fugue %2`.").arg(quoted("--" + name), command.name),
                         {}, valid);
        }
    }

    if (command.help)
        return command; // nothing runs, so the arguments need not be complete
    if (command.positionals.size() < spec.minPositionals
        || (spec.maxPositionals >= 0 && command.positionals.size() > spec.maxPositionals))
        return usage(QStringLiteral("`fugue %1` takes %2 argument(s); got %3.")
                         .arg(command.name,
                              spec.maxPositionals < 0 ? QStringLiteral("at least %1").arg(spec.minPositionals)
                              : spec.minPositionals == spec.maxPositionals
                                  ? QString::number(spec.minPositionals)
                                  : QStringLiteral("%1 to %2").arg(spec.minPositionals).arg(spec.maxPositionals))
                         .arg(command.positionals.size()));

    for (const QString &set : command.options.value("set")) {
        // <node>.<input>=<value>
        const int dot = set.indexOf('.');
        const int equals = set.indexOf('=');
        if (dot <= 0 || equals < dot + 2)
            return usage(QStringLiteral("--set takes <node>.<input>=<value>, not %1.").arg(quoted(set)));
    }
    return command;
}

bool wantsCli(int argc, char **argv) {
    if (argc < 2)
        return false;
    const QString first = QString::fromLocal8Bit(argv[1]);
    if (first == "--help" || first == "-h" || first == "--version" || first == "--json")
        return true;
    if (first.startsWith('-'))
        return false;
    // `fugue path.fugue` opens the window on that file. A command word always wins, so a
    // file named like one is opened as `fugue ./run`.
    if (!table().contains(canonicalGroup(first)) && (first.endsWith(".fugue", Qt::CaseInsensitive) || QFileInfo(first).isFile()))
        return false;
    return true;
}

} // namespace fugue::cli
