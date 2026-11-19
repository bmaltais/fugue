#pragma once

// Argument parsing for the command table of spec 13.1. It only turns argv into
// a Command; it knows nothing about running one.

#include <QMap>
#include <QString>
#include <QStringList>

#include "engine/result.h"

namespace fugue::cli {

struct Command {
    // The canonical name: "run", "validate", "compile", "workflow.new",
    // "runs.status", "node.list", ... Aliases are resolved: `nodes list` is
    // "node.list" and `workflow validate` is "validate".
    QString name;
    QStringList positionals;
    // Option values by name without dashes; a flag has one empty entry.
    QMap<QString, QStringList> options;
    bool json = false;
    // True for commands that belong to later milestones: they parse, but
    // there is nothing to run yet.
    bool later = false;

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
