#pragma once

// Starting, queueing, running and cancelling runs across processes (spec 9.8).
//
// `fugue run` compiles, creates the run folder, writes the queue entry and starts a
// detached worker (`fugue __worker <run id>`, the same binary) that waits its turn
// and then runs the graph with runGraph(). Everything the worker needs is in the run
// folder: compiled.json and run.json (the workflow path).
//
// Who writes a run's run.json and its run.done, so that no two processes ever do:
//   - `fugue run` writes the first run.json (status queued) before it starts the worker,
//     and nothing after, unless the worker could not be started.
//   - While the run waits, nobody writes. Whoever takes its entry out of the queue ends it:
//     a cancel, or a process that found the entry's worker dead (Queue::remove(seen) lets
//     exactly one process have it).
//   - The worker owns the run from the moment it activates its entry: the scheduler
//     writes run.json (with the worker's pid) and run.done.
//   - If the worker died while active, the process that takes the engine lock and
//     removes the dead entry ends the run as failed.
// The worker's pid while it waits is in its queue entry only.

#include <optional>

#include "cli/commands.h"
#include "engine/queue.h"
#include "engine/run_folder.h"

namespace fugue::cli {

// `fugue run`: returns {"run", "status": "queued"} at once, or with --wait streams
// events until run.done and exits 1 unless the run ended ok.
Reply startRun(const Command &command, Context &context);

// `fugue runs cancel`: removes a waiting run's entry, or signals the worker.
Reply cancelRun(const Command &command, Context &context);
// The same, from what the queue held when it was read: `seen` is the run's entry then, if
// it had one. The queue may have moved on since, so a waiting entry is removed by its
// exact file; when that file is gone the worker has activated it, and is signalled.
Reply cancelRun(const RunFolder &folder, const std::optional<QueueEntry> &seen, const Context &context);

// The hidden `__worker` command. Returns the process exit code.
int runWorker(const QString &runId, Context &context);

// Ends a run that never ran (or whose worker is gone): run.json and run.done.
void endUnrun(const RunFolder &folder, const QString &status, const Errors &errors);

// Ends, as failed, every run whose worker died (9.8): the dead entries leave the queue
// (Queue::discardDead) and each of their runs gets its status, `ended` and run.done.
// `lock` is the engine lock, held or not; it is left as it was.
void endDeadRuns(const Context &context, Lock &lock);

// Writes events as JSON lines as they arrive until run.done; returns its status.
QString streamEvents(const RunFolder &folder, Context &context);

// The run's status: run.json's, else what the events say, else "unknown".
QString runStatus(const RunFolder &folder, const QList<QJsonObject> &events);
bool isTerminal(const QString &status);

} // namespace fugue::cli
