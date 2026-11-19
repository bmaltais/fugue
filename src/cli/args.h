#pragma once

// The command table of spec 13.1 and argument parsing against it. Each command is
// in the table once, with the arguments it takes and the handler that runs it
// (commands.h), so there is no second list of commands to keep in step.

#include <QMap>
#include <QString>
#include <QStringList>

#include "engine/result.h"

namespace fugue::cli {

struct Command;
struct Context;
struct Reply;

// What runs a command (commands.h).
using Handler = Reply (*)(const Command &command, Context &context);

struct Command {
    // The canonical name: "run", "validate", "compile", "workflow.new",
    // "runs.status", "node.list", ... Aliases are resolved: `nodes list` is
    // "node.list" and `workflow validate` is "validate".
    QString name;
    QStringList positionals;
    // Option values by name without dashes; a flag has one empty entry.
    QMap<QString, QStringList> options;
    bool json = false;
    // From the table. Null for a command that belongs to a later milestone: it
    // parses, but there is nothing to run yet.
    Handler handler = nullptr;

    bool has(const QString &option) const { return options.contains(option); }
    QString value(const QString &option) const { return options.value(option).value(0); }
};

// `args` is argv without the program name. `--json` may stand anywhere.
// Failures are `usage` errors.
Result<Command> parseArguments(const QStringList &args);

// True when argv asks for the command line rather than the window: the first
// argument is a word (not a Qt option such as -platform) or --help, --version
// or --json. main() calls it before it creates any application object.
bool wantsCli(int argc, char **argv);

} // namespace fugue::cli
