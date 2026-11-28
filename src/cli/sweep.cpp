// `fugue run <file> --sweep <node>.<input>=v1,v2 [--sweep ...] --to <node> --wait` (#552, spec 13.1): the plan and the sheet's
// workflow are engine/sweep.h's; this starts each cell's run the way `fugue run` does (startBoundRun, one after the other: they share
// the one GPU) and waits for it, then runs the sheet. A cell that fails is a labelled blank tile, and Ctrl+C cancels the rest.

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include "cli/canvas_run.h"
#include "cli/runs.h"
#include "engine/paths.h"
#include "engine/sweep.h"
#include "engine/vips.h"

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

// The first picture node `node` made in the run: a Save node's file, else what a Preview shows (as `fugue app run` reads it).
QString pictureOf(const RunFolder &folder, const QString &node) {
    QStringList paths;
    for (const QJsonObject &event : folder.events()) {
        const QString kind = event.value("event").toString();
        if ((kind != QLatin1String("node.done") && kind != QLatin1String("node.cached")) || event.value("node").toString() != node)
            continue;
        const qsizetype before = paths.size();
        collectMediaPaths(event.value("outputs"), paths);
        if (paths.size() == before)
            collectMediaPaths(event.value("shown"), paths);
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

// A plain tile the size of the first picture that was made, kept in the inputs folder (a run may always read it), for a cell
// that did not make one.
QString blankTile(const QString &sizedLike) {
    int width = 256, height = 256;
    try {
        if (initVips() && !sizedLike.isEmpty()) {
            const vips::VImage picture = vips::VImage::new_from_file(QFile::encodeName(sizedLike).constData());
            width = picture.width();
            height = picture.height();
        }
        const QString path = QDir(inputsDir()).filePath(QStringLiteral("sweep-blank-%1x%2.png").arg(width).arg(height));
        if (QFileInfo::exists(path))
            return path;
        QDir().mkpath(inputsDir());
        vips::VImage tile = vips::VImage::black(width, height).new_from_image(std::vector<double>{60, 60, 60}).copy(
            vips::VImage::option()->set("interpretation", VIPS_INTERPRETATION_sRGB));
        tile.write_to_file(QFile::encodeName(path).constData());
        return path;
    } catch (const vips::VError &) {
        return {};
    }
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
    QString firstPicture;
    for (qsizetype i = 0; i < plan->cells.size(); ++i) {
        const SweepCell &cell = plan->cells.at(i);
        context.log(QStringLiteral("[%1/%2] %3 ...").arg(i + 1).arg(plan->cells.size()).arg(cell.label));
        const CellOutcome outcome = runCell(cell.bound, file, to, settings, *allowRemote, context);
        context.log(QStringLiteral("[%1/%2] %3: %4").arg(i + 1).arg(plan->cells.size()).arg(cell.label,
                    outcome.status == QLatin1String("ok") ? QStringLiteral("ok") : QStringLiteral("%1: %2").arg(outcome.status, outcome.message)));
        outcomes << outcome;
        files << outcome.file;
        if (firstPicture.isEmpty())
            firstPicture = outcome.file;
    }
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
    if (stopRequested())
        return Reply::failure(makeError(QStringLiteral("cancelled"), QStringLiteral("The sweep was cancelled.")));
    if (firstPicture.isEmpty()) {
        const CellOutcome &first = outcomes.first();
        return Reply::failure(makeError(first.code.isEmpty() ? QStringLiteral("node_failed") : first.code,
                                        QStringLiteral("No cell of the sweep made a picture: %1").arg(first.message),
                                        "Run the workflow once with `fugue run --wait` to see the error."));
    }

    // A cell without a picture is a blank tile saying so.
    const QString blank = blankTile(firstPicture);
    if (blank.isEmpty())
        return Reply::failure(makeError(QStringLiteral("internal"), QStringLiteral("The blank tile for a failed cell could not be made.")));
    int failed = 0;
    for (qsizetype i = 0; i < files.size(); ++i) {
        if (!files.at(i).isEmpty())
            continue;
        ++failed;
        files[i] = blank;
        labels.cells[i] = labels.cells.at(i).isEmpty() ? QStringLiteral("failed") : labels.cells.at(i) + QStringLiteral(" (failed)");
    }
    const Result<Document> sheet = composeDocument(files, labels, context.registry);
    if (!sheet)
        return Reply::failure(sheet.errors());
    context.log(QStringLiteral("Laying out the sheet ..."));
    const Result<RunFolder> folder = startBoundRun(*sheet, file, context, {});
    if (!folder)
        return Reply::failure(folder.errors());
    const canvas::BatchRunResult laid = waitForRun(*folder, context);
    if (laid.status != QLatin1String("ok"))
        return Reply::failure(makeError(laid.status == QLatin1String("cancelled") ? QStringLiteral("cancelled")
                                        : laid.code.isEmpty() ? QStringLiteral("node_failed") : laid.code, laid.message));
    const QString grid = pictureOf(*folder, QStringLiteral("sheet"));
    Reply reply = Reply::success(QJsonObject{{"grid", grid}, {"run", folder->id()}, {"columns", labels.columns}, {"failed", failed},
                                             {"cells", cellRows()}}, grid);
    if (failed > 0) {
        // A warning: the sheet was made, and each cell's own status is in the data.
        Error warning = makeError(QStringLiteral("node_failed"), QStringLiteral("%1 of %2 cells failed; the sheet shows them blank.").arg(failed).arg(files.size()));
        warning.severity = Error::Severity::Warning;
        reply.errors = {warning};
    }
    return reply;
}

} // namespace fugue::cli
