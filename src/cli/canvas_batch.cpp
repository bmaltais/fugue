// `fugue canvas batch <action> <in-folder> <out-folder>` (#250): a canvas action over a folder of pictures. The loop, the
// naming and the export are engine/canvas_batch.h's; this reads the arguments, starts each picture's run the way `fugue
// run` does (a detached worker that waits for its turn on the one GPU) and waits for it, and prints one line per picture.

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

#include <atomic>
#include <csignal>

#include "cli/commands.h"
#include "cli/canvas_run.h"
#include "cli/compile_step.h"
#include "cli/runs.h"
#include "engine/canvas_batch.h"
#include "engine/json.h"
#include "engine/pack_manager.h"
#include "engine/queue.h"
#include "engine/run_folder.h"

namespace fugue::cli {

namespace {

std::atomic<bool> g_stop{false};

// The first signal asks to stop; a second one means the wait is too long for the person, and ends the process as the signal would.
void onSignal(int number) {
    if (g_stop.exchange(true)) {
        // A pack's setup runs in a group of its own, which the terminal's signal does not reach: end it with us.
        if (const qint64 group = runningSetupGroup().load(); group > 1)
            ::kill(pid_t(-group), SIGKILL);
        ::signal(number, SIG_DFL);
        ::raise(number);
    }
}

Error usageProblem(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("usage"), message, hint); }

// The first error of a run.done event, in words.
QString runMessage(const QJsonObject &done) {
    for (const QJsonValue &value : done.value("errors").toArray()) {
        const Error error = Error::fromJson(value.toObject());
        if (error.severity == Error::Severity::Error)
            return error.message;
    }
    return QStringLiteral("The action did not finish.");
}

QString humanLine(const QJsonObject &event) {
    const QString kind = event.value("event").toString();
    if (kind == QLatin1String("batch.started"))
        return QStringLiteral("%1 pictures.").arg(event.value("total").toInt());
    if (kind == QLatin1String("item.started"))
        return QStringLiteral("[%1/%2] %3 ...").arg(event.value("index").toInt() + 1).arg(event.value("total").toInt()).arg(QFileInfo(event.value("file").toString()).fileName());
    if (kind == QLatin1String("item.done")) {
        const QString status = event.value("status").toString();
        return QStringLiteral("[%1/%2] %3: %4").arg(event.value("index").toInt() + 1).arg(event.value("total").toInt())
            .arg(QFileInfo(event.value("file").toString()).fileName(),
                 status == QLatin1String("ok") ? QStringLiteral("ok, wrote %1").arg(event.value("output").toString())
                                               : QStringLiteral("%1: %2").arg(status, event.value("error").toString()));
    }
    return QStringLiteral("Done: %1 ok, %2 failed, %3 cancelled.").arg(event.value("ok").toInt()).arg(event.value("failed").toInt()).arg(event.value("cancelled").toInt());
}

} // namespace

void installStopHandlers() {
    struct sigaction handler {};
    handler.sa_handler = onSignal;
    sigemptyset(&handler.sa_mask);
    sigaction(SIGINT, &handler, nullptr);
    sigaction(SIGTERM, &handler, nullptr);
}

bool stopRequested() { return g_stop.load(); }

Result<RunFolder> startBoundRun(const Document &bound, const QString &workflow, Context &context, const QJsonObject &allowRemote,
                                const CompileSettings &settings) {
    const Compilation compilation = compileDocument(bound, context.registry, settings);
    if (!compilation.ok())
        return compilation.errors();
    if (const Errors refused = remoteConsentProblems(*compilation, context, workflow, allowRemote); !refused.isEmpty())
        return refused;
    return startCompiledRun(compilation, bound.toJson(), workflow, false, context, allowRemote);
}

canvas::BatchRunResult waitForRun(const RunFolder &folder, Context &context, const std::function<void(const QJsonObject &)> &onEvent) {
    bool cancelling = false;
    int orphaned = 0;
    qsizetype seen = 0;
    for (int pass = 0;; ++pass) {
        const QList<QJsonObject> events = folder.events();
        if (onEvent) {
            for (; seen < events.size(); ++seen)
                onEvent(events.at(seen));
        }
        for (auto it = events.crbegin(); it != events.crend(); ++it)
            if (it->value("event").toString() == "run.done") {
                const RunStatus status = parseRunStatus(it->value("status").toString());
                if (status == RunStatus::Ok)
                    return {QStringLiteral("ok"), {}, {}};
                if (status == RunStatus::Cancelled)
                    return {QStringLiteral("cancelled"), {}, QStringLiteral("Cancelled.")};
                const QString code = Error::fromJson(it->value("errors").toArray().at(0).toObject()).code;
                return {QStringLiteral("failed"), code, runMessage(*it)};
            }
        if (g_stop.load() && !cancelling) {
            cancelling = true;
            QueueEntry entry;
            const Queue queue(context.queueDir);
            cancelRun(folder, queue.find(folder.id(), &entry) ? std::optional<QueueEntry>(entry) : std::nullopt, context);
        }
        // A worker that died leaves no run.done (9.8): the same look as `fugue run --wait` takes.
        if (pass % 10 == 0) {
            Lock lock(context.lockPath);
            endDeadRuns(context, lock);
            const qint64 pid = qint64(folder.state().value("pid").toDouble());
            const bool orphan = !isTerminal(folder.status()) && !Queue(context.queueDir).find(folder.id()) && !isRunWorker(pid, folder.id());
            orphaned = orphan ? orphaned + 1 : 0;
            if (orphaned >= 2)
                folder.finish(RunStatus::Failed, {makeError("internal", "The run's worker is gone.")}, utcNow());
        }
        if (isTerminal(folder.status()) && pass > 40 && folder.events().isEmpty())
            return {QStringLiteral("failed"), QStringLiteral("internal"), QStringLiteral("The run ended without a result.")};
        QThread::msleep(50);
    }
}

namespace {

canvas::BatchRunResult runOne(const Document &bound, const QString &workflow, Context &context) {
    const Result<RunFolder> folder = startBoundRun(bound, workflow, context);
    if (!folder)
        return {QStringLiteral("failed"), folder.errors().first().code, folder.errors().first().message};
    return waitForRun(*folder, context);
}

} // namespace

Reply canvasBatch(const Command &command, Context &context) {
    canvas::BatchSpec spec;
    const Result<canvas::BatchAction> action =
        canvas::loadBatchAction(command.positionals.at(0), {QStringLiteral(":/fugue/actions")}, context.registry);
    if (!action)
        return Reply::failure(action.errors());
    spec.action = action.value();
    spec.inFolder = command.positionals.at(1);
    spec.outFolder = command.positionals.at(2);
    if (command.has("prompt"))
        spec.prompt = command.value("prompt");
    if (command.has("seed")) {
        bool ok = false;
        const qint64 seed = command.value("seed").toLongLong(&ok);
        if (!ok || seed < 0)
            return Reply::failure(usageProblem(QStringLiteral("--seed is a whole number of 0 or more."), command.value("seed")));
        spec.seed = seed;
    }
    spec.keepCanvases = command.has("keep");
    const Result<canvas::PictureExport> picture = pictureOptions(command, command.has("format") ? QStringLiteral("picture") : QStringLiteral("picture.png"));
    if (!picture)
        return Reply::failure(picture.errors());
    spec.picture = picture.value();

    installStopHandlers();

    bool streaming = false;
    canvas::BatchHooks hooks;
    hooks.cancelled = [] { return g_stop.load(); };
    hooks.event = [&](const QJsonObject &event) {
        streaming = true;
        context.line(command.json ? compactJson(event) : humanLine(event).toUtf8());
    };
    const Result<canvas::BatchReport> report = canvas::runBatch(
        spec, [&](const Document &bound, const QString &workflow) { return runOne(bound, workflow, context); }, hooks);
    if (!report)
        return Reply::failure(report.errors());
    Reply reply;
    reply.streamed = streaming;
    reply.exitCode = report->failed() + report->cancelled() > 0 ? 1 : 0;
    if (command.json)
        context.line(compactJson(QJsonObject{{"event", "batch.report"}, {"report", report->toJson()}}));
    return reply;
}

} // namespace fugue::cli
