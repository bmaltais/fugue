#pragma once

#include <QJsonValue>
#include <QString>

#include "cli/args.h"
#include "engine/canvas_export.h"
#include "engine/error.h"
#include "engine/node_kernel.h"
#include "engine/readiness.h"
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
    bool json = false; // --json: a notice that would go to stderr is an event on stdout instead
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
// authoring.cpp: `node new` and `node test` (6.11).
Reply nodeNew(const Command &command, Context &context);
Reply nodeTest(const Command &command, Context &context);
Reply validateWorkflow(const Command &command, Context &context);
Reply compileWorkflow(const Command &command, Context &context);
Reply workflowNew(const Command &command, Context &context);
Reply workflowFromTemplate(const Command &command, Context &context); // workflow new --template
Reply templateList(const Command &command, Context &context);
Reply workflowDescribe(const Command &command, Context &context);
Reply workflowExport(const Command &command, Context &context);
Reply runsList(const Command &command, Context &context);
Reply runsStatus(const Command &command, Context &context);
Reply runsEvents(const Command &command, Context &context);
Reply runsResult(const Command &command, Context &context);
// models.cpp: `model list` (13.1), models on disk and offered by providers.
Reply modelList(const Command &command, Context &context);
Reply modelGet(const Command &command, Context &context);
// packs.cpp: the `pack` commands (13.1). All but list need a person (13.6).
Reply packList(const Command &command, Context &context);
Reply packAdd(const Command &command, Context &context);
Reply packRemove(const Command &command, Context &context);
Reply packUpdate(const Command &command, Context &context);
Reply packTrust(const Command &command, Context &context);
// `pack secret list [<pack>]`, `set <pack> <name>` (the value from stdin) and `clear <pack> <name>` (6.4, #402).
Reply packSecret(const Command &command, Context &context);
// `template prepare` and `workflow prepare` (#379): the one list and the one question (engine/readiness.h).
Reply templatePrepare(const Command &command, Context &context);
Reply workflowPrepare(const Command &command, Context &context);

// app.cpp: the `app` commands (#392, spec 4.9): the apps of this machine, what one takes, and a run of one.
Reply appList(const Command &command, Context &context);
Reply appInfo(const Command &command, Context &context);
Reply appRun(const Command &command, Context &context);
Reply appExport(const Command &command, Context &context);
// packs.cpp: the list of what a workflow still needs (packs, trust, models), the one `prepare` shows; nothing is changed.
ReadinessPlan readinessPlanFor(const ReadinessRequest &request, Context &context);

// cache.cpp: `cache status` and `cache clear` (10.6).
Reply cacheStatus(const Command &command, Context &context);
Reply cacheClear(const Command &command, Context &context);

// settings.cpp: `settings list`, each setting in effect with where its value comes from (1.4.2).
Reply settingsList(const Command &command, Context &context);

// project.cpp: the `project` commands (1.4.2, 13.1). None needs a person: they only change the registry
// and project.json files, never delete a project's files.
Reply projectList(const Command &command, Context &context);
Reply projectShow(const Command &command, Context &context);
Reply projectNew(const Command &command, Context &context);
Reply projectAdd(const Command &command, Context &context);
Reply projectRemove(const Command &command, Context &context);
Reply projectInherits(const Command &command, Context &context);

// canvas.cpp: the `canvas` commands (13.1, 14.5): the canvas revision store.
Reply canvasNew(const Command &command, Context &context);
Reply canvasImport(const Command &command, Context &context);
Reply canvasExport(const Command &command, Context &context);
Reply canvasBatch(const Command &command, Context &context); // canvas_batch.cpp (#250)
Reply canvasAct(const Command &command, Context &context);     // canvas_act.cpp (#231 slice 10): run a canvas action, or list them
Reply canvasActions(const Command &command, Context &context);
// A model picked by its file name (or file name), as the task bar's picker does; `usage` lists the names there are. Shared by
// `canvas act --set model=` and `app run --input model=`.
Result<QJsonObject> modelByName(const PortType &type, const QString &word, Context &context);
// The picture options of `canvas export` and `canvas batch` (--format, --quality, --scale, --size, --width, --height,
// --background, --lossless); `out` is the file named, whose extension gives the format when --format is not given.
Result<canvas::PictureExport> pictureOptions(const Command &command, const QString &out);
Reply canvasLog(const Command &command, Context &context);
Reply canvasShow(const Command &command, Context &context);
Reply canvasList(const Command &command, Context &context);
Reply canvasCandidates(const Command &command, Context &context);
Reply canvasAccept(const Command &command, Context &context);
Reply canvasDrop(const Command &command, Context &context);
Reply canvasDiff(const Command &command, Context &context);
Reply canvasLayer(const Command &command, Context &context);
Reply canvasResize(const Command &command, Context &context);
Reply canvasUndo(const Command &command, Context &context);
Reply canvasRedo(const Command &command, Context &context);

// skill.cpp: `skill install` and `skill uninstall` (13.5).
Reply skillInstall(const Command &command, Context &context);
Reply skillUninstall(const Command &command, Context &context);

} // namespace fugue::cli
