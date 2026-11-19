#pragma once

// Starting, queueing, running and cancelling runs across processes (spec 9.8).
//
// `fugue run` compiles, creates the run folder, writes the queue entry and starts a
// detached worker (`fugue __worker <run id>`, the same binary) that waits its turn
// and then runs the graph with runGraph(). Everything the worker needs is in the run
// folder: compiled.json and run.json (the workflow path).

#include "cli/commands.h"
#include "engine/run_folder.h"

namespace fugue::cli {

// `fugue run`: returns {"run", "status": "queued"} at once, or with --wait streams
// events until run.done and exits 1 unless the run ended ok.
Reply startRun(const Command &command, Context &context);

// `fugue runs cancel`: removes a waiting run's entry, or signals the worker.
Reply cancelRun(const Command &command, Context &context);

// The hidden `__worker` command. Returns the process exit code.
int runWorker(const QString &runId, Context &context);

// Ends a run that never ran (or whose worker is gone): run.json and run.done.
void endUnrun(const RunFolder &folder, const QString &status, const Errors &errors);

// Writes events as JSON lines as they arrive until run.done; returns its status.
QString streamEvents(const RunFolder &folder, Context &context);

// The run's status: run.json's, else what the events say, else "unknown".
QString runStatus(const RunFolder &folder, const QList<QJsonObject> &events);
bool isTerminal(const QString &status);

} // namespace fugue::cli
