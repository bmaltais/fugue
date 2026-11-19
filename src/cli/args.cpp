#include "cli/args.h"

#include <QJsonArray>

namespace fugue::cli {

namespace {

struct Spec {
    int minPositionals = 0;
    int maxPositionals = 0;       // -1: no limit
    QStringList valueOptions;     // --name <value> or --name=<value>
    QStringList flagOptions;      // --name
    bool later = false;           // a later milestone: parsed, not run
};

struct Group {
    QString command; // canonical name when the group has no subcommands
    QMap<QString, Spec> subs;
};

const Spec runSpec{1, 1, {"to", "provider", "set"}, {"wait", "first"}, false};
const Spec fileSpec{1, 1, {}, {}, false};

// The commands of 13.1 by the first word. A group with an empty-named entry in
// `subs` takes no subcommand.
const QMap<QString, QMap<QString, Spec>> &table() {
    static const QMap<QString, QMap<QString, Spec>> commands{
        {"run", {{"", runSpec}}},
        {"validate", {{"", fileSpec}}},
        {"compile", {{"", fileSpec}}},
        {"workflow",
         {{"new", fileSpec},
          {"describe", fileSpec},
          {"export", {1, 1, {}, {"share", "no-prompts", "no-layout"}, false}},
          {"run", runSpec},
          {"validate", fileSpec},
          {"compile", fileSpec}}},
        {"runs",
         {{"list", {0, 0, {}, {}, false}},
          {"status", {1, 1, {}, {}, false}},
          {"events", {1, 1, {}, {"follow"}, false}},
          {"result", {1, 1, {}, {}, false}},
          {"cancel", {1, 1, {}, {}, false}}}},
        {"node",
         {{"list", {0, 0, {"type"}, {}, false}},
          {"describe", {1, -1, {}, {}, false}},
          {"new", {1, 1, {}, {}, true}},
          {"test", {1, 1, {}, {}, true}}}},
        {"pack",
         {{"list", {0, 0, {}, {}, true}},
          {"add", {1, 1, {}, {}, true}},
          {"remove", {1, 1, {}, {}, true}},
          {"update", {0, 1, {}, {}, true}},
          {"trust", {1, 1, {}, {}, true}}}},
        {"provider", {{"list", {0, 0, {}, {}, true}}}},
        {"model", {{"list", {0, 0, {"type"}, {}, true}}}},
        {"cache",
         {{"status", {0, 0, {}, {}, true}}, {"clear", {0, 0, {"workflow", "node"}, {}, true}}}},
        {"command", {{"list", {0, 0, {}, {}, true}}}},
        {"skill", {{"install", {0, 0, {}, {}, true}}}},
    };
    return commands;
}

// Spec 13.1: the plural is an alias for these four only.
QString canonicalGroup(const QString &word) {
    if (word == "nodes" || word == "packs" || word == "providers" || word == "models")
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

    QString sub;
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
    command.later = spec.later;

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
    return !first.startsWith('-') || first == "--help" || first == "-h" || first == "--version" || first == "--json";
}

} // namespace fugue::cli
