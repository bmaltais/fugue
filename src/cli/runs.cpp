#include "cli/runs.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QThread>

#include <atomic>
#include <csignal>
#include <unistd.h>

#include "cli/compile_step.h"
#include "cli/process.h"
#include "engine/queue.h"
#include "engine/document.h"
#include "engine/json.h"

namespace fugue::cli {

namespace {

std::atomic<bool> g_cancel{false};

void onSignal(int) { g_cancel.store(true); }

// --set <node>.<input>=<value>: the value is JSON when it parses as one (a number, true,
// "text"), else plain text. The input then holds that literal, replacing a link.
Errors applyOverrides(Document &document, const QStringList &overrides) {
    Errors errors;
    for (const QString &text : overrides) {
        const int dot = text.indexOf('.');
        const int equals = text.indexOf('=');
        const QString node = text.left(dot);
        const QString input = text.mid(dot + 1, equals - dot - 1);
        QString raw = text.mid(equals + 1);
        const auto found = document.graph.nodes.find(node);
        if (found == document.graph.nodes.end()) {
            Error error = makeError("usage", QStringLiteral("--set names node \"%1\", which the workflow does not have.").arg(node),
                                    "Use the node's id as `fugue workflow describe` shows it.");
            for (auto it = document.graph.nodes.cbegin(); it != document.graph.nodes.cend(); ++it)
                error.valid.append(it.key());
            errors << error;
            continue;
        }
        QJsonValue value = raw;
        const QJsonDocument parsed = QJsonDocument::fromJson("[" + raw.toUtf8() + "]");
        if (parsed.isArray() && parsed.array().size() == 1)
            value = parsed.array().at(0);
        found->inputs.insert(input, InputValue::fromJson(value));
    }
    return errors;
}

} // namespace

namespace {

Error workerGone() {
    return makeError("internal", "The run's worker is gone.", "Start the run again with `fugue run`.");
}

} // namespace

void endDeadRuns(const Context &context, Lock &lock) {
    for (const QString &gone : Queue(context.queueDir).discardDead(lock)) {
        const Result<RunFolder> folder = RunFolder::open(context.runsRoot, gone);
        if (folder && !isTerminal(folder->status()))
            folder->finish(RunStatus::Failed, {workerGone()}, utcNow());
    }
}

RunStatus streamEvents(const RunFolder &folder, Context &context) {
    int printed = 0;
    int settling = 0;
    int orphaned = 0;
    for (int pass = 0;; ++pass) {
        const QList<QJsonObject> events = folder.events();
        for (; printed < events.size(); ++printed)
            context.line(compactJson(events.at(printed)));
        for (auto it = events.crbegin(); it != events.crend(); ++it)
            if (it->value("event").toString() == "run.done")
                return parseRunStatus(it->value("status").toString());
        const RunStatus state = parseRunStatus(folder.state().value("status").toString());
        // run.json is written just before run.done, so the event is given a moment to
        // arrive; after that its writer is gone and the state is the answer.
        if (isTerminal(state) && ++settling > 20)
            return state;
        // A worker that died leaves no run.done (9.8): twice a second, look for runs
        // whose worker is gone and end them, this one included.
        if (pass % 10 == 0) {
            Lock lock(context.lockPath);
            endDeadRuns(context, lock);
            // Not ended, not in the queue and no worker: whoever took its entry ends it at
            // once, so seeing this twice means nobody will (a starter or canceller that
            // was killed half way).
            const qint64 pid = qint64(folder.state().value("pid").toDouble());
            const bool orphan = !isTerminal(folder.status())
                                && !Queue(context.queueDir).find(folder.id()) && !isRunWorker(pid, folder.id());
            orphaned = orphan ? orphaned + 1 : 0;
            if (orphaned >= 2)
                folder.finish(RunStatus::Failed, {workerGone()}, utcNow());
        }
        QThread::msleep(50);
    }
}

namespace {

// What `fugue run <file>` is to run: the file compiled, or the file itself when it
// already is a compiled graph (4.8). `document` is the workflow as compiled (after
// `--set`), empty for a compiled graph.
struct Plan {
    Compilation compilation;
    QJsonObject document;
};

Plan graphToRun(const Command &command, Context &context) {
    const QString file = command.positionals.at(0);
    const Result<QJsonObject> json = readJsonObject(file);
    if (json && json->contains("fugue-compiled")) {
        // Nothing is compiled, so what changes a compile has nothing to act on.
        for (const char *option : {"set", "to", "provider"})
            if (command.has(option))
                return {makeError("usage", QStringLiteral("--%1 cannot be used with a compiled graph.").arg(option),
                                  "Run the workflow file itself with this option."), {}};
        const Errors providerProblems = registerProviders(context.registry);
        if (hasErrors(providerProblems))
            return {Compilation(providerProblems), {}};
        const Result<CompiledGraph> graph = CompiledGraph::fromJson(*json);
        const Errors problems = providerProblems + graph.problems();
        return {graph ? Compilation(graph.value(), problems) : Compilation(problems), {}};
    }
    // Not a compiled graph: a workflow document, whose loader reports what is wrong
    // with the file in a workflow's terms.
    Result<Document> document = Document::loadFile(file);
    if (!document)
        return {Compilation(document.errors()), {}};
    const Errors setErrors = applyOverrides(*document, command.options.value("set"));
    if (!setErrors.isEmpty())
        return {Compilation(setErrors), {}};
    return {compileDocument(*document, context.registry, {command.value("to"), command.value("provider")}),
            document->toJson()};
}

} // namespace

Reply startRun(const Command &command, Context &context) {
    const QString file = command.positionals.at(0);
    const Plan plan = graphToRun(command, context);
    const Compilation &compilation = plan.compilation;
    if (!compilation.ok())
        return Reply::failure(compilation.errors());

    const Result<RunFolder> folder = RunFolder::create(context.runsRoot);
    if (!folder)
        return Reply::failure(folder.errors());
    const QString workflow = QFileInfo(file).absoluteFilePath();
    // The worker reads both back. Without workflow.json the run would save its images
    // without their workflow (11.7), so a failed write ends the run before it starts.
    Errors written;
    if (!folder->writeCompiled(compilation->toJson()))
        written.append(makeError("permission_denied", "The compiled graph could not be written to " + folder->path() + "."));
    if (!plan.document.isEmpty())
        written += folder->writeWorkflow(plan.document);
    // The worker is not started here, so "the current directory" of 13.6 is recorded now.
    written += folder->writeRoots(FileRoots::defaults(QDir::currentPath(), workflow));
    if (hasErrors(written)) {
        folder->finish(RunStatus::Failed, written, utcNow());
        return Reply::failure(written);
    }
    folder->queued(workflow);

    const Queue queue(context.queueDir);
    if (!queue.enqueue(folder->id(), workflow, command.has("first"))) {
        folder->finish(RunStatus::Failed, {}, utcNow());
        return Reply::failure(makeError("internal", "The run queue could not be written.", "Check " + context.queueDir));
    }
    QProcess worker;
    worker.setProgram(QCoreApplication::applicationFilePath());
    worker.setArguments({"__worker", folder->id()});
    worker.setStandardInputFile(QProcess::nullDevice());
    worker.setStandardOutputFile(QProcess::nullDevice());
    worker.setStandardErrorFile(QProcess::nullDevice());
    if (!worker.startDetached()) {
        queue.remove(folder->id());
        const Error error = makeError("internal", "The run worker could not be started.", worker.errorString());
        folder->finish(RunStatus::Failed, {error}, utcNow());
        return Reply::failure(error);
    }
    // The worker records its own pid, in its queue entry and later in run.json; both
    // belong to it from here on (runs.h). Recording it from here would name a process
    // that may not have its command line yet (isRunWorker).

    if (command.has("wait")) {
        Reply reply;
        reply.streamed = true;
        reply.exitCode = streamEvents(*folder, context) == RunStatus::Ok ? 0 : 1;
        return reply;
    }
    return Reply::success(QJsonObject{{"run", folder->id()}, {"status", runStatusName(RunStatus::Queued)}}, folder->id());
}

Reply cancelRun(const Command &command, Context &context) {
    const Result<RunFolder> folder = RunFolder::open(context.runsRoot, command.positionals.at(0));
    if (!folder)
        return Reply::failure(folder.errors());
    QueueEntry entry;
    if (Queue(context.queueDir).find(folder->id(), &entry))
        return cancelRun(*folder, entry, context);
    return cancelRun(*folder, std::nullopt, context);
}

Reply cancelRun(const RunFolder &folder, const std::optional<QueueEntry> &seen, const Context &context) {
    const QString id = folder.id();
    const auto ended = [&](RunStatus status) {
        return Reply::success(QJsonObject{{"id", id}, {"status", runStatusName(status)}, {"cancelled", false}},
                              QStringLiteral("%1 had already ended (%2).").arg(id, runStatusName(status)));
    };
    RunStatus status = folder.status();
    if (isTerminal(status))
        return ended(status);
    const Queue queue(context.queueDir);
    std::optional<QueueEntry> entry = seen;
    if (entry && !entry->active) {
        // Waiting: take its entry out. Getting the file is what makes the run ours to end;
        // its worker finds the entry gone and exits.
        if (queue.remove(*entry)) {
            folder.finish(RunStatus::Cancelled, {}, utcNow());
            return Reply::success(QJsonObject{{"id", id}, {"status", runStatusName(RunStatus::Cancelled)}, {"cancelled", true}},
                                  QStringLiteral("Cancelled %1.").arg(id));
        }
        // The file is gone: its worker activated it meanwhile, or someone else removed it.
        QueueEntry now;
        entry = queue.find(id, &now) ? std::optional<QueueEntry>(now) : std::nullopt;
    }
    // The window's own run: no signal reaches it, so it is asked through the run folder,
    // while that window lives (its pid and start time); a gone one's run is ended below.
    if (entry && entry->inProcess && entry->pid > 0 && processStartTime(entry->pid) == entry->processStart
        && folder.requestCancel())
        return Reply::success(QJsonObject{{"id", id}, {"status", "cancelling"}, {"cancelled", true}},
                              QStringLiteral("Cancelling %1.").arg(id));
    // Active (or about to be): the worker ends the run when it is told to. Only a process
    // that really is this run's worker is signalled; a pid may have been reused.
    const qint64 pid = entry && entry->pid > 0 ? entry->pid : qint64(folder.state().value("pid").toDouble());
    if (signalRunWorker(pid, id, SIGTERM))
        // Not a run status: the worker has been told, and the run is cancelled once it ends.
        return Reply::success(QJsonObject{{"id", id}, {"status", "cancelling"}, {"cancelled", true}},
                              QStringLiteral("Cancelling %1.").arg(id));
    // No worker to signal. If it died, its entry is dead and the run is ended as failed.
    {
        Lock lock(context.lockPath);
        endDeadRuns(context, lock);
    }
    status = folder.status();
    if (isTerminal(status) && status != RunStatus::Failed)
        return ended(status); // it ended by itself meanwhile
    if (!isTerminal(status)) {
        if (queue.find(id))
            return Reply::failure(makeError("internal", QStringLiteral("The worker of %1 could not be signalled.").arg(id),
                                            "Try again in a moment."));
        folder.finish(RunStatus::Failed, {workerGone()}, utcNow()); // no entry and no worker: nothing will end it
    }
    return Reply::failure(makeError("internal", QStringLiteral("The worker of %1 is gone; the run is marked failed.").arg(id),
                                    "Start the run again with `fugue run`."));
}

namespace {

int work(const QString &runId, Context &context) {
    const Result<RunFolder> folder = RunFolder::open(context.runsRoot, runId);
    if (!folder)
        return 1;
    struct sigaction action {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);

    const Queue queue(context.queueDir);
    Lock lock(context.lockPath);
    queue.setPid(runId, qint64(::getpid()));

    // Wait for my turn: my entry first in the queue, and the lock.
    for (;;) {
        QueueEntry mine;
        if (!queue.find(runId, &mine))
            return 1; // cancelled while waiting; whoever removed the entry ends the run
        if (g_cancel.load()) {
            // Told to stop while waiting. The run is mine to end only if I am the one
            // who takes its entry out.
            if (queue.remove(mine))
                folder->finish(RunStatus::Cancelled, {}, utcNow());
            return 1;
        }
        endDeadRuns(context, lock);
        const QList<QueueEntry> entries = queue.entries();
        if (!entries.isEmpty() && entries.first().run == runId && lock.tryAcquire() && queue.activate(runId))
            break;
        QThread::msleep(50);
    }

    // providers.json is read now, when the run's turn has come: it may have changed, or
    // broken, since `fugue run` compiled. A file that cannot be used fails the run with
    // the reason; an instance that cannot be made is a warning of the run.
    const Errors providerProblems = registerProviders(context.registry);
    const Result<CompiledGraph> graph = hasErrors(providerProblems) ? Result<CompiledGraph>(providerProblems)
                                                                    : CompiledGraph::fromJson(folder->compiled());
    if (!graph) {
        folder->finish(RunStatus::Failed, graph.errors(), utcNow());
        queue.remove(runId);
        return 1;
    }
    Process process(context);
    collectGarbageIfDue(context, process);
    RunOptions options;
    options.warnings = providerProblems;
    options.workflow = folder->state().value("workflow").toObject().value("path").toString();
    options.document = folder->workflow();
    options.roots = folder->roots();
    const RunOutcome outcome = runGraph(*graph, *folder, process.environment, options, &g_cancel);
    queue.remove(runId);
    return outcome.status == RunStatus::Ok ? 0 : 1;
}

} // namespace

Reply runWorker(const Command &command, Context &context) {
    Reply reply;
    reply.streamed = true;
    reply.exitCode = work(command.positionals.at(0), context);
    return reply;
}

} // namespace fugue::cli
