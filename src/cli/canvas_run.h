#pragma once

// Starting a bound canvas workflow as a run and waiting for it, as `fugue run --wait` does: shared by `fugue canvas batch`
// (cli/canvas_batch.cpp) and `fugue canvas act` (cli/canvas_act.cpp). The run is the queue's: a detached worker that waits for
// its turn on the one GPU.

#include <QJsonObject>

#include <functional>

#include "cli/commands.h"
#include "engine/canvas_batch.h"
#include "engine/run_folder.h"

namespace fugue::cli {

// SIGINT and SIGTERM set a flag instead of ending the process: a wait then cancels its run (waitForRun) and a loop checks stopRequested().
void installStopHandlers();
bool stopRequested();

// Compiles `bound` and starts its run (the run folder, the queue entry, the worker). `workflow` is the file it is recorded under.
Result<RunFolder> startBoundRun(const Document &bound, const QString &workflow, Context &context);

// Waits until the run ends: ok, failed (the first error's code and message) or cancelled. A stop request cancels it. `onEvent`, when
// given, gets each event of the run as it is written (node.running, node.progress, node.done, ...).
canvas::BatchRunResult waitForRun(const RunFolder &folder, Context &context, const std::function<void(const QJsonObject &)> &onEvent = {});

} // namespace fugue::cli
