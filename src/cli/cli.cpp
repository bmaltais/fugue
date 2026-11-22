#include "cli/cli.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

#include "cli/args.h"
#include "cli/commands.h"
#include "engine/queue.h"
#include "engine/json.h"
#include "engine/run_folder.h"

namespace fugue::cli {

namespace {

void writeOut(const QByteArray &bytes) {
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

void writeLine(const QByteArray &line) { writeOut(line + '\n'); }

void logLine(const QString &text) {
    const QByteArray bytes = text.toUtf8() + '\n';
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stderr);
    std::fflush(stderr);
}

// One error as a person reads it (stderr, or stdout text without --json).
QString humanError(const Error &error) {
    QString text = QStringLiteral("%1[%2]: %3")
                       .arg(error.severity == Error::Severity::Warning ? "warning" : "error", error.code, error.message);
    // The same information as the JSON form (13.2): where, and what would be valid.
    QStringList where;
    if (!error.node.isEmpty())
        where << QStringLiteral("node %1").arg(error.node);
    if (!error.input.isEmpty())
        where << QStringLiteral("input %1").arg(error.input);
    if (!error.output.isEmpty())
        where << QStringLiteral("output %1").arg(error.output);
    if (!where.isEmpty())
        text += QStringLiteral(" (%1)").arg(where.join(", "));
    if (!error.valid.isEmpty()) {
        QStringList valid;
        for (const QJsonValue &option : error.valid)
            valid << option.toVariant().toString();
        text += QStringLiteral("\n  valid: %1").arg(valid.join(", "));
    }
    if (!error.hint.isEmpty())
        text += QStringLiteral("\n  hint: %1").arg(error.hint);
    return text;
}

const char *helpText =
    "usage: fugue <command> [--json]\n"
    "  run <file> [--wait] [--first] [--to <node>] [--provider <name>] [--set <node>.<input>=<value>]\n"
    "  validate <file> | compile <file>\n"
    "  workflow new <file> [--template <id>] | workflow describe|export|run|validate|compile <file>\n"
    "  runs list | runs status|events|result|cancel <run-id>\n"
    "  node list [--type <port type>] | node describe <type id>[@<major>]...\n"
    "  pack list (origin: own or inherited from Default, read-only there) | pack add <folder|git-url> [--ref <tag|commit>] | pack update [id] [--ref ...] | pack remove|trust <id>\n"
    "  model list [--type <resource type>] [--hash]\n"
    "  template list (origin: own, bundled or inherited from Default)\n"
    "  settings list\n"
    "  project list | project show [name|path] | project new <name> [--in <dir>] [--inherits a,b] | project add <dir>\n"
    "  project remove <name|path> | project inherits <name|path> <items|none>\n"
    "  canvas new [--name <text>] [--size <w>x<h>] | canvas import <file.ora> [--name <text>] | canvas export <id[@rev]> <file.ora>\n"
    "  canvas log <id[@rev]> [--limit <n>] | canvas show <id[@rev]>   (the canvas revision store, spec 14.5)\n"
    "  canvas candidates <id> [--all] | canvas accept|drop <id[@rev]> | canvas diff <id@rev> [<id@rev>]\n"
    "  skill install|uninstall [--target agents|claude|all] [--dry-run] [--force]\n"
    "  cache status | cache clear [--kind results|thumbnails|runs|all] [--workflow <file>] [--node <id>]\n"
    "      [--older-than <age, 30d>] [--dry-run]\n"
    "  provider, command (later releases)\n"
    "With no command, fugue opens the window.\n"
    "  --home <dir> (anywhere), or FUGUE_HOME: keep everything fugue writes in one folder (spec 1.4.1)\n"
    "  --project <name|path> (anywhere), or FUGUE_PROJECT: work in a project (spec 1.4.2); not with --home\n";

} // namespace

int run(int argc, char **argv, const Hooks &hooks) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("fugue");
    QCoreApplication::setApplicationVersion("0.1.0");

    const QStringList args = app.arguments().mid(1);
    if (args.value(0) == "--help" || args.value(0) == "-h") {
        writeOut(helpText);
        return 0;
    }
    if (args.value(0) == "--version") {
        writeLine(QCoreApplication::applicationVersion().toUtf8());
        return 0;
    }

    const bool json = args.contains("--json");
    auto fail = [json](const Errors &errors) {
        if (json) {
            writeLine(compactJson(errorEnvelope(errors)));
        } else {
            for (const Error &error : errors)
                logLine(humanError(error));
        }
        return 1;
    };

    const Result<Command> command = parseArguments(args);
    if (!command)
        return fail(command.errors());

    Context context;
    context.runsRoot = RunFolder::defaultRoot();
    context.queueDir = Queue::defaultDir();
    context.lockPath = Queue::defaultLockPath();
    if (hooks.addNodeTypes)
        hooks.addNodeTypes(context.registry);
    if (hooks.addKernels)
        hooks.addKernels(context.kernels);
    context.line = writeLine;
    context.log = logLine;

    const Reply reply = execute(*command, context);
    if (reply.streamed)
        return reply.exitCode;
    if (reply.failed())
        return fail(reply.errors);
    // Warnings that did not fail the command are diagnostics.
    for (const Error &warning : reply.errors)
        logLine(humanError(warning));
    if (json) {
        // 13.2's envelope plus an additive `warnings`, only when there are some.
        QJsonObject envelope = okEnvelope(reply.data);
        if (!reply.errors.isEmpty())
            envelope.insert("warnings", toJson(reply.errors));
        writeLine(compactJson(envelope));
    } else if (!reply.text.isEmpty()) {
        writeLine(reply.text.toUtf8());
    }
    return 0;
}

} // namespace fugue::cli
