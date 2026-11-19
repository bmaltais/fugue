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
#include "cli/queue.h"
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
    QString status = folder.state().value("status").toString();
    if (status.isEmpty()) {
        for (auto it = events.crbegin(); it != events.crend(); ++it)
            if (it->value("event").toString() == "run.done")
                status = it->value("status").toString();
    }
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

QString streamEvents(const RunFolder &folder, Context &context) {
    int printed = 0;
    for (;;) {
        const QList<QJsonObject> events = folder.events();
        for (; printed < events.size(); ++printed)
            context.line(compact(events.at(printed)).toUtf8());
        for (auto it = events.crbegin(); it != events.crend(); ++it)
            if (it->value("event").toString() == "run.done")
                return it->value("status").toString();
        const QString state = folder.state().value("status").toString();
        if (isTerminal(state))
            return state; // the worker is gone without a run.done
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
    qint64 pid = 0;
    if (!worker.startDetached(&pid)) {
        queue.remove(folder->id());
        const Error error = makeError("internal", "The run worker could not be started.", worker.errorString());
        endUnrun(*folder, "failed", {error});
        return Reply::failure(error);
    }
    queue.setPid(folder->id(), pid);
    QJsonObject state = folder->state();
    if (state.value("status").toString() == "queued") { // the worker may be ahead of us
        state.insert("pid", double(pid));
        folder->writeState(state);
    }

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
    const QString id = folder->id();
    const QString status = runStatus(*folder, folder->events());
    if (isTerminal(status))
        return Reply::success(QJsonObject{{"id", id}, {"status", status}, {"cancelled", false}},
                              QStringLiteral("%1 had already ended (%2).").arg(id, status));
    const Queue queue(context.queueDir);
    QueueEntry entry;
    const bool queued = queue.find(id, &entry);
    if (queued && !entry.active) {
        // Waiting: drop its entry; the worker notices and exits.
        queue.remove(id);
        endUnrun(*folder, "cancelled", {});
        return Reply::success(QJsonObject{{"id", id}, {"status", "cancelled"}, {"cancelled", true}},
                              QStringLiteral("Cancelled %1.").arg(id));
    }
    const qint64 pid = queued && entry.pid > 0 ? entry.pid : qint64(folder->state().value("pid").toDouble());
    if (!processExists(pid) || ::kill(pid_t(pid), SIGTERM) != 0) {
        endUnrun(*folder, "failed", {makeError("internal", "The run's worker is gone.")});
        queue.remove(id);
        return Reply::failure(makeError("internal", QStringLiteral("The worker of %1 is gone; the run is marked failed.").arg(id)));
    }
    return Reply::success(QJsonObject{{"id", id}, {"status", "cancelling"}, {"cancelled", true}},
                          QStringLiteral("Cancelling %1.").arg(id));
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
        if (g_cancel.load()) {
            queue.remove(runId);
            if (!isTerminal(runStatus(*folder, folder->events())))
                endUnrun(*folder, "cancelled", {});
            return 1;
        }
        for (const QString &gone : queue.discardStale()) {
            const Result<RunFolder> other = RunFolder::open(context.runsRoot, gone);
            if (other && !isTerminal(runStatus(*other, other->events())))
                endUnrun(*other, "failed", {makeError("internal", "The run's worker is gone.")});
        }
        const QList<QueueEntry> entries = queue.entries();
        QueueEntry mine;
        if (!queue.find(runId, &mine))
            return 1; // cancelled while waiting; whoever removed the entry ended the run
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
