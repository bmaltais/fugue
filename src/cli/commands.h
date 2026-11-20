#pragma once

#include <QJsonValue>
#include <QString>

#include "cli/args.h"
#include "engine/error.h"
#include "engine/node_kernel.h"
#include "engine/registry.h"

namespace fugue::cli {

// What a command produces. A failure has errors; a success has `data` (the
// "data" of the 13.2 envelope) and `text` (the same for a person).
struct Reply {
    Errors errors;
    QJsonValue data;
    QString text;
    // The command already wrote its own lines to stdout (a stream), so no
    // envelope follows; `exitCode` then decides the result.
    bool streamed = false;
    int exitCode = 0;

    bool failed() const { return streamed ? exitCode != 0 : hasErrors(errors); }
    static Reply failure(const Errors &errors) { Reply reply; reply.errors = errors; return reply; }
    static Reply failure(const Error &error) { return failure(Errors{error}); }
    static Reply success(const QJsonValue &data, const QString &text = {}) {
        Reply reply;
        reply.data = data;
        reply.text = text;
        return reply;
    }
};

// Where commands write. Lines of a stream go to `line`; the one-shot answer is
// printed by cli.cpp.
struct Context {
    Registry registry;
    Kernels kernels;
    QString runsRoot;
    QString queueDir;
    QString lockPath;
    void (*line)(const QByteArray &) = nullptr; // one stdout line, flushed
    void (*log)(const QString &) = nullptr;     // one stderr diagnostic
};

// Runs the command with its handler from the table; a command of a later milestone
// (no handler) is a usage error, "not available in this version".
Reply execute(const Command &command, Context &context);

// The handlers the command table (args.cpp) names. Those of `run`, `runs cancel` and
// the worker are in runs.h.
Reply nodeList(const Command &command, Context &context);
Reply nodeDescribe(const Command &command, Context &context);
Reply validateWorkflow(const Command &command, Context &context);
Reply compileWorkflow(const Command &command, Context &context);
Reply workflowNew(const Command &command, Context &context);
Reply workflowFromTemplate(const Command &command, Context &context); // workflow new --template
Reply templateList(const Command &command, Context &context);
Reply workflowDescribe(const Command &command, Context &context);
Reply runsList(const Command &command, Context &context);
Reply runsStatus(const Command &command, Context &context);
Reply runsEvents(const Command &command, Context &context);
Reply runsResult(const Command &command, Context &context);
// models.cpp: `model list` (13.1), models on disk and offered by providers.
Reply modelList(const Command &command, Context &context);
// packs.cpp: the `pack` commands (13.1). All but list need a person (13.6).
Reply packList(const Command &command, Context &context);
Reply packAdd(const Command &command, Context &context);
Reply packRemove(const Command &command, Context &context);
Reply packUpdate(const Command &command, Context &context);
Reply packTrust(const Command &command, Context &context);

} // namespace fugue::cli
