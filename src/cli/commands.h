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

Reply execute(const Command &command, Context &context);

// The node types in the folder $FUGUE_NODE_MANIFESTS names (every node.json
// under it). This is the hook that lets tests, and a developer, seed the
// registry before built-in node types exist; a missing folder is an empty registry.
void loadManifestFolder(Registry &registry, const QString &folder, void (*log)(const QString &));

} // namespace fugue::cli
