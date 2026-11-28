// `fugue run <file> --sweep <node>.<input>=v1,v2 [--sweep ...] --to <node> --wait` (#552, spec 13.1): the plan and the sheet's
// workflow are engine/sweep.h's; this starts each cell's run the way `fugue run` does (startBoundRun, one after the other: they share
// the one GPU) and waits for it, then runs the sheet. A cell that fails is a labelled blank tile, and Ctrl+C cancels the rest.

#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>

#include "cli/canvas_run.h"
#include "cli/runs.h"
#include "engine/media.h"
#include "engine/json.h"
#include "engine/sweep.h"

namespace fugue::cli {

namespace {

struct CellOutcome {
    QString status; // "ok", "failed" or "cancelled"
    QString code;
    QString message;
    QString run;
    QString file; // the picture, for "ok"
};

Error usageProblem(const QString &message, const QString &hint = {}) { return makeError(QStringLiteral("usage"), message, hint); }

// The first picture node `node` made in the run (a handle of type image, not any file: a model loader has a path too): a Save node's
// file, else what a Preview shows (as `fugue app run` reads it).
QString pictureOf(const RunFolder &folder, const QString &node) {
    QStringList paths;
    for (const QJsonObject &event : folder.events()) {
        const QString kind = event.value("event").toString();
        if ((kind != QLatin1String("node.done") && kind != QLatin1String("node.cached")) || event.value("node").toString() != node)
            continue;
        const qsizetype before = paths.size();
        collectMediaPaths(event.value("outputs"), paths, QStringLiteral("image"));
        if (paths.size() == before)
            collectMediaPaths(event.value("shown"), paths, QStringLiteral("image"));
    }
    return paths.value(0);
}

CellOutcome runCell(const Document &bound, const QString &workflow, const QString &to, const CompileSettings &settings,
                    const QJsonObject &allowRemote, Context &context) {
    CellOutcome outcome;
    if (stopRequested())
        return {QStringLiteral("cancelled"), {}, QStringLiteral("Cancelled."), {}, {}};
    const Result<RunFolder> folder = startBoundRun(bound, workflow, context, allowRemote, settings);
    if (!folder)
        return {QStringLiteral("failed"), folder.errors().first().code, folder.errors().first().message, {}, {}};
    const canvas::BatchRunResult result = waitForRun(*folder, context);
    outcome.run = folder->id();
    outcome.status = result.status;
    outcome.code = result.code;
    outcome.message = result.message;
    if (result.status == QLatin1String("ok")) {
        outcome.file = pictureOf(*folder, to);
        if (outcome.file.isEmpty()) {
            outcome.status = QStringLiteral("failed");
            outcome.code = QStringLiteral("node_bad_output");
            outcome.message = QStringLiteral("Node %1 made no picture.").arg(to);
        }
    }
    return outcome;
}

} // namespace

Reply sweepRun(const Command &command, Context &context) {
    const QString file = QFileInfo(command.positionals.at(0)).absoluteFilePath();
    if (!command.has("wait") || !command.has("to"))
        return Reply::failure(usageProblem(QStringLiteral("--sweep needs --wait and --to <node>."),
                                           "--to names the node whose picture is each cell, for example --to save."));
    for (const char *option : {"first", "use-on-disk", "no-use-on-disk"})
        if (command.has(option))
            return Reply::failure(usageProblem(QStringLiteral("--%1 cannot be used with --sweep.").arg(option)));
    QList<SweepAxis> axes;
    for (const QString &text : command.options.value("sweep")) {
        const Result<SweepAxis> axis = parseSweepAxis(text);
        if (!axis)
            return Reply::failure(axis.errors());
        axes << axis.value();
    }
    Result<Document> document = Document::loadFile(file);
    if (!document)
        return Reply::failure(document.errors());
    if (const Errors set = applySetOverrides(*document, command.options.value("set")); !set.isEmpty())
        return Reply::failure(set);
    const QString to = command.value("to");
    const Result<SweepPlan> plan = planSweep(*document, axes, to, context.registry);
    if (!plan)
        return Reply::failure(plan.errors());
    const Result<QJsonObject> allowRemote = allowRemoteFingerprints(command.options.value("allow-remote"), context);
    if (!allowRemote)
        return Reply::failure(allowRemote.errors());

    installStopHandlers();
    const CompileSettings settings{to, command.value("provider")};
    QList<CellOutcome> outcomes;
    QStringList files;
    SweepLabels labels = plan->labels;
    const auto cellRows = [&] {
        QJsonArray rows;
        for (qsizetype i = 0; i < outcomes.size(); ++i) {
            const CellOutcome &o = outcomes.at(i);
            QJsonObject row{{"label", plan->cells.at(i).label}, {"status", o.status}, {"run", o.run}};
            if (o.status == QLatin1String("ok")) {
                row.insert("file", o.file);
            } else {
                row.insert("code", o.code);
                row.insert("error", o.message);
            }
            rows.append(row);
        }
        return rows;
    };
    // The sheet cannot be made: the error, and still which cell made which file, so what was spent is not lost.
    const auto sizeFailure = [&](const Errors &errors) {
        Reply reply;
        reply.streamed = true;
        reply.exitCode = 1;
        if (command.json) {
            QJsonObject envelope = errorEnvelope(errors);
            envelope.insert("cells", cellRows());
            context.line(compactJson(envelope));
        } else {
            for (const Error &error : errors)
                context.log(error.message + QLatin1Char(' ') + error.hint);
            for (qsizetype i = 0; i < outcomes.size(); ++i)
                if (!outcomes.at(i).file.isEmpty())
                    context.line((outcomes.at(i).file + QLatin1Char('\t') + plan->cells.at(i).label).toUtf8());
        }
        return reply;
    };
    QString firstPicture;
    QSize largest; // of the pictures made so far (null: none read yet)
    for (qsizetype i = 0; i < plan->cells.size(); ++i) {
        const SweepCell &cell = plan->cells.at(i);
        context.log(QStringLiteral("[%1/%2] %3 ...").arg(i + 1).arg(plan->cells.size()).arg(cell.label));
        const CellOutcome outcome = runCell(cell.bound, file, to, settings, *allowRemote, context);
        context.log(QStringLiteral("[%1/%2] %3: %4").arg(i + 1).arg(plan->cells.size()).arg(cell.label,
                    outcome.status == QLatin1String("ok") ? QStringLiteral("ok") : QStringLiteral("%1: %2").arg(outcome.status, outcome.message)));
        outcomes << outcome;
        files << outcome.file;
        if (outcome.file.isEmpty())
            continue;
        if (firstPicture.isEmpty())
            firstPicture = outcome.file;
        // The sheet is as big as the largest picture so far: say now, not after the last cell, that it could not be made.
        const std::optional<QSize> size = MediaHandle::imageHeaderSize(outcome.file);
        if (!size) {
            context.log(QStringLiteral("Warning: the size of %1 could not be read, so it is not counted in the sheet's size check.").arg(outcome.file));
            continue;
        }
        largest = QSize(std::max(largest.width(), size->width()), std::max(largest.height(), size->height()));
        if (const Errors tooBig = checkSheetSize(labels, largest); !tooBig.isEmpty())
            return sizeFailure(tooBig);
    }
    if (stopRequested())
        return Reply::failure(makeError(QStringLiteral("cancelled"), QStringLiteral("The sweep was cancelled.")));
    if (firstPicture.isEmpty()) {
        const CellOutcome &first = outcomes.first();
        return Reply::failure(makeError(first.code.isEmpty() ? QStringLiteral("node_failed") : first.code,
                                        QStringLiteral("No cell of the sweep made a picture: %1").arg(first.message),
                                        "Run the workflow once with `fugue run --wait` to see the error."));
    }

    // A cell without a picture stays an empty entry (composeDocument makes a plain tile of it) and says so in its label.
    int failed = 0;
    for (qsizetype i = 0; i < files.size(); ++i) {
        if (!files.at(i).isEmpty())
            continue;
        ++failed;
        labels.cells[i] = labels.cells.at(i).isEmpty() ? QStringLiteral("failed") : labels.cells.at(i) + QStringLiteral(" (failed)");
    }
    const Result<Document> sheet = composeDocument(files, labels, context.registry, {{}, largest.isNull() ? QSize(256, 256) : largest});
    if (!sheet)
        return Reply::failure(sheet.errors());
    context.log(QStringLiteral("Laying out the sheet ..."));
    // The sheet's run reads the cells' pictures, wherever the output folder is; nothing else beyond the usual roots. It is recorded under the
    // swept file like the cells (its roots and its place in History), and its own workflow.json is the sheet's graph.
    QStringList pictures = files;
    pictures.removeAll(QString());
    const Result<RunFolder> folder = startBoundRun(*sheet, file, context, {}, {}, pictures);
    if (!folder)
        return Reply::failure(folder.errors());
    const canvas::BatchRunResult laid = waitForRun(*folder, context);
    if (laid.status != QLatin1String("ok"))
        return Reply::failure(makeError(laid.status == QLatin1String("cancelled") ? QStringLiteral("cancelled")
                                        : laid.code.isEmpty() ? QStringLiteral("node_failed") : laid.code, laid.message));
    const QString grid = pictureOf(*folder, QStringLiteral("sheet"));
    QJsonObject data{{"grid", grid}, {"run", folder->id()}, {"columns", labels.columns}, {"failed", failed}, {"cells", cellRows()}};
    // Streamed, as `fugue canvas batch`: the sheet is made and printed, and the exit says whether every cell made its picture.
    Reply reply;
    reply.streamed = true;
    reply.exitCode = failed > 0 ? 1 : 0;
    Errors warnings;
    if (failed > 0) {
        Error warning = makeError(QStringLiteral("node_failed"), QStringLiteral("%1 of %2 cells failed; the sheet shows them blank.").arg(failed).arg(files.size()));
        warning.severity = Error::Severity::Warning;
        warnings << warning;
    }
    if (command.json) {
        QJsonObject envelope = okEnvelope(data);
        if (!warnings.isEmpty())
            envelope.insert("warnings", toJson(warnings));
        context.line(compactJson(envelope));
    } else {
        for (const Error &warning : warnings)
            context.log(warning.message);
        context.line(grid.toUtf8());
    }
    return reply;
}

} // namespace fugue::cli
