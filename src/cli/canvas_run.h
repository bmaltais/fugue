#pragma once

// Starting a bound canvas workflow as a run and waiting for it, as `fugue run --wait` does: shared by `fugue canvas batch`
// (cli/canvas_batch.cpp) and `fugue canvas act` (cli/canvas_act.cpp). The run is the queue's: a detached worker that waits for
// its turn on the one GPU.

#include <QJsonObject>

#include <functional>

#include "cli/commands.h"
#include "cli/compile_step.h"
#include "engine/canvas_batch.h"
#include "engine/run_folder.h"

namespace fugue::cli {

// SIGINT and SIGTERM set a flag instead of ending the process: a wait then cancels its run (waitForRun) and a loop checks stopRequested().
void installStopHandlers();
bool stopRequested();

// Compiles `bound` and starts its run (the run folder, the queue entry, the worker). `workflow` is the file it is recorded under.
// A run that would send data off this machine is refused with consent_needed unless allowed (engine/remote_consent.h, #404);
// `allowRemote` is allowRemoteFingerprints() of `--allow-remote`; `settings` are the compile's `--to` and `--provider`; `alsoRead` are files the run may read besides the file roots.
Result<RunFolder> startBoundRun(const Document &bound, const QString &workflow, Context &context, const QJsonObject &allowRemote = {},
                                const CompileSettings &settings = {}, const QStringList &alsoRead = {});

// Waits until the run ends: ok, failed (the first error's code and message) or cancelled. A stop request cancels it. `onEvent`, when
// given, gets each event of the run as it is written (node.running, node.progress, node.done, ...).
canvas::BatchRunResult waitForRun(const RunFolder &folder, Context &context, const std::function<void(const QJsonObject &)> &onEvent = {});

} // namespace fugue::cli
