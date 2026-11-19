#include "cli/runs.h"

#include <QCoreApplication>
#include <QDateTime>
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

namespace fugue::cli {

namespace {

std::atomic<bool> g_cancel{false};

void onSignal(int) { g_cancel.store(true); }

QString compact(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

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

QString runStatus(const RunFolder &folder, const QList<QJsonObject> &events) {
    // run.done is the last word: it is appended once, by whoever ended the run, while
    // run.json is rewritten as the run goes.
    for (auto it = events.crbegin(); it != events.crend(); ++it)
        if (it->value("event").toString() == "run.done")
            return it->value("status").toString();
    QString status = folder.state().value("status").toString();
    if (status.isEmpty() && !events.isEmpty())
        status = events.last().value("event").toString() == "run.queued" ? "queued" : "running";
    return status.isEmpty() ? QStringLiteral("unknown") : status;
}

bool isTerminal(const QString &status) {
    return status == "ok" || status == "failed" || status == "cancelled";
}

void endUnrun(const RunFolder &folder, const QString &status, const Errors &errors) {
    QJsonObject state = folder.state();
    state.insert("status", status);
    state.insert("ended", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    folder.writeState(state);
    QJsonArray list;
    for (const Error &error : errors)
        list.append(error.toJson());
    folder.appendEvent(QJsonObject{{"event", "run.done"}, {"status", status}, {"errors", list}});
}

namespace {

Error workerGone() {
    return makeError("internal", "The run's worker is gone.", "Start the run again with `fugue run`.");
}

} // namespace

void endDeadRuns(const Context &context, Lock &lock) {
    for (const QString &gone : Queue(context.queueDir).discardDead(lock)) {
        const Result<RunFolder> folder = RunFolder::open(context.runsRoot, gone);
        if (folder && !isTerminal(runStatus(*folder, folder->events())))
            endUnrun(*folder, "failed", {workerGone()});
    }
}

QString streamEvents(const RunFolder &folder, Context &context) {
    int printed = 0;
    int settling = 0;
    int orphaned = 0;
    for (int pass = 0;; ++pass) {
        const QList<QJsonObject> events = folder.events();
        for (; printed < events.size(); ++printed)
            context.line(compact(events.at(printed)).toUtf8());
        for (auto it = events.crbegin(); it != events.crend(); ++it)
            if (it->value("event").toString() == "run.done")
                return it->value("status").toString();
        const QString state = folder.state().value("status").toString();
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
            const bool orphan = !isTerminal(runStatus(folder, folder.events()))
                                && !Queue(context.queueDir).find(folder.id()) && !isRunWorker(pid, folder.id());
            orphaned = orphan ? orphaned + 1 : 0;
            if (orphaned >= 2)
                endUnrun(folder, "failed", {workerGone()});
        }
        QThread::msleep(50);
    }
}

Reply startRun(const Command &command, Context &context) {
    const QString file = command.positionals.at(0);
    Errors errors;
    std::optional<Document> document = Document::loadFile(file, errors);
    if (!document)
        return Reply::failure(errors);
    const Errors setErrors = applyOverrides(*document, command.options.value("set"));
    if (!setErrors.isEmpty())
        return Reply::failure(setErrors);
    const Compilation compilation = compileDocument(*document, context.registry, {command.value("to"), command.value("provider")});
    if (!compilation.ok())
        return Reply::failure(compilation.errors);

    const Result<RunFolder> folder = RunFolder::create(context.runsRoot);
    if (!folder)
        return Reply::failure(folder.errors());
    const QString workflow = QFileInfo(file).absoluteFilePath();
    folder->writeState(QJsonObject{{"status", "queued"}, {"workflow", QJsonObject{{"path", workflow}}}});
    folder->writeCompiled(compilation.graph->toJson());
    folder->appendEvent(QJsonObject{{"event", "run.queued"}});

    const Queue queue(context.queueDir);
    if (!queue.enqueue(folder->id(), workflow, command.has("first"))) {
        endUnrun(*folder, "failed", {});
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
        endUnrun(*folder, "failed", {error});
        return Reply::failure(error);
    }
    // The worker records its own pid, in its queue entry and later in run.json; both
    // belong to it from here on (runs.h). Recording it from here would name a process
    // that may not have its command line yet (isRunWorker).

    if (command.has("wait")) {
        Reply reply;
        reply.streamed = true;
        reply.exitCode = streamEvents(*folder, context) == "ok" ? 0 : 1;
        return reply;
    }
    return Reply::success(QJsonObject{{"run", folder->id()}, {"status", "queued"}}, folder->id());
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
    const auto ended = [&](const QString &status) {
        return Reply::success(QJsonObject{{"id", id}, {"status", status}, {"cancelled", false}},
                              QStringLiteral("%1 had already ended (%2).").arg(id, status));
    };
    QString status = runStatus(folder, folder.events());
    if (isTerminal(status))
        return ended(status);
    const Queue queue(context.queueDir);
    std::optional<QueueEntry> entry = seen;
    if (entry && !entry->active) {
        // Waiting: take its entry out. Getting the file is what makes the run ours to end;
        // its worker finds the entry gone and exits.
        if (queue.remove(*entry)) {
            endUnrun(folder, "cancelled", {});
            return Reply::success(QJsonObject{{"id", id}, {"status", "cancelled"}, {"cancelled", true}},
                                  QStringLiteral("Cancelled %1.").arg(id));
        }
        // The file is gone: its worker activated it meanwhile, or someone else removed it.
        QueueEntry now;
        entry = queue.find(id, &now) ? std::optional<QueueEntry>(now) : std::nullopt;
    }
    // Active (or about to be): the worker ends the run when it is told to. Only a process
    // that really is this run's worker is signalled; a pid may have been reused.
    const qint64 pid = entry && entry->pid > 0 ? entry->pid : qint64(folder.state().value("pid").toDouble());
    if (isRunWorker(pid, id) && ::kill(pid_t(pid), SIGTERM) == 0)
        return Reply::success(QJsonObject{{"id", id}, {"status", "cancelling"}, {"cancelled", true}},
                              QStringLiteral("Cancelling %1.").arg(id));
    // No worker to signal. If it died, its entry is dead and the run is ended as failed.
    {
        Lock lock(context.lockPath);
        endDeadRuns(context, lock);
    }
    status = runStatus(folder, folder.events());
    if (isTerminal(status) && status != QLatin1String("failed"))
        return ended(status); // it ended by itself meanwhile
    if (!isTerminal(status)) {
        if (queue.find(id))
            return Reply::failure(makeError("internal", QStringLiteral("The worker of %1 could not be signalled.").arg(id),
                                            "Try again in a moment."));
        endUnrun(folder, "failed", {workerGone()}); // no entry and no worker: nothing will end it
    }
    return Reply::failure(makeError("internal", QStringLiteral("The worker of %1 is gone; the run is marked failed.").arg(id),
                                    "Start the run again with `fugue run`."));
}

int runWorker(const QString &runId, Context &context) {
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
                endUnrun(*folder, "cancelled", {});
            return 1;
        }
        endDeadRuns(context, lock);
        const QList<QueueEntry> entries = queue.entries();
        if (!entries.isEmpty() && entries.first().run == runId && lock.tryAcquire() && queue.activate(runId))
            break;
        QThread::msleep(50);
    }

    Process process(context);
    collectGarbageIfDue(context, process);

    Errors errors;
    const std::optional<CompiledGraph> graph = CompiledGraph::fromJson(folder->compiled(), errors);
    if (!graph) {
        endUnrun(*folder, "failed", errors);
        queue.remove(runId);
        return 1;
    }
    RunOptions options;
    options.workflow = folder->state().value("workflow").toObject().value("path").toString();
    const RunOutcome outcome = runGraph(*graph, *folder, process.environment, options, &g_cancel);
    queue.remove(runId);
    return outcome.status == "ok" ? 0 : 1;
}

} // namespace fugue::cli
