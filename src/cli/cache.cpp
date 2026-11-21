// `fugue cache status` and `fugue cache clear` (spec 13.1, 10.6, #141): what Fugue keeps under
// ~/.cache/fugue, and clearing it. The work is the cache manager's (engine/cache_manager.h);
// this reads the options and prints.

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <limits>

#include "cli/commands.h"
#include "engine/cache_manager.h"
#include "engine/model_download.h"

namespace fugue::cli {

namespace {

CacheFolders folders(const Context &context) {
    CacheFolders result = CacheFolders::standard();
    if (!context.runsRoot.isEmpty())
        result.runs = context.runsRoot;
    if (!context.queueDir.isEmpty())
        result.queue = context.queueDir;
    if (!context.lockPath.isEmpty())
        result.lock = context.lockPath;
    return result;
}

// "30d", "12h", "90m", "2w", or a bare number of days.
std::optional<qint64> parseAge(const QString &text) {
    static const QRegularExpression pattern(QStringLiteral("^(\\d+)([smhdw]?)$"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch())
        return std::nullopt;
    const qint64 count = match.captured(1).toLongLong();
    const QString unit = match.captured(2);
    const qint64 seconds = unit == "s" ? 1 : unit == "m" ? 60 : unit == "h" ? 3600 : unit == "w" ? 7 * 86400 : 86400;
    if (count <= 0 || count > (std::numeric_limits<qint64>::max() / 4) / seconds)
        return std::nullopt;
    return count * seconds;
}

QString day(const QDateTime &time) {
    return time.isValid() ? time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd")) : QStringLiteral("-");
}

QString kindLine(const KindUsage &usage) {
    QString line = QStringLiteral("%1  %2").arg(cacheKindName(usage.kind), -10).arg(sizeText(usage.bytes), 9);
    switch (usage.kind) {
    case CacheKind::Results:
        line += QStringLiteral("  %1 results in %2 files").arg(usage.items).arg(usage.files);
        line += usage.limit > 0 ? QStringLiteral(", limit %1").arg(sizeText(usage.limit)) : QStringLiteral(", no limit");
        break;
    case CacheKind::Thumbnails:
        line += QStringLiteral("  %1 files").arg(usage.files);
        line += usage.limit > 0 ? QStringLiteral(", limit %1").arg(sizeText(usage.limit)) : QStringLiteral(", no limit");
        break;
    case CacheKind::Runs:
        line += QStringLiteral("  %1 runs, %2 files, the newest %3 kept").arg(usage.items).arg(usage.files).arg(usage.limit);
        break;
    }
    if (usage.items > 0)
        line += QStringLiteral(", oldest %1").arg(day(usage.oldest));
    if (usage.protectedItems > 0)
        line += QStringLiteral(", %1 %2")
                    .arg(usage.protectedItems)
                    .arg(usage.kind == CacheKind::Results ? QStringLiteral("frozen") : QStringLiteral("going or queued"));
    return line;
}

} // namespace

Reply cacheStatus(const Command &, Context &context) {
    const CacheStatus status = CacheManager(folders(context)).status(CachePolicy::current());
    QStringList lines;
    for (const KindUsage &usage : status.kinds)
        lines << kindLine(usage);
    lines << QStringLiteral("total       %1").arg(sizeText(status.totalBytes()), 9);
    if (!status.workflows.isEmpty()) {
        lines << QString() << QStringLiteral("Results by workflow (one used by several counts for each):");
        for (const WorkflowUsage &workflow : status.workflows) {
            lines << QStringLiteral("  %1  %2, %3 results")
                         .arg(workflow.workflow.isEmpty() ? QStringLiteral("(no file)") : workflow.workflow,
                              sizeText(workflow.bytes))
                         .arg(workflow.entries);
            for (const NodeUsage &node : workflow.nodes)
                lines << QStringLiteral("    %1  %2, %3 results").arg(node.node, sizeText(node.bytes)).arg(node.entries);
        }
    }
    return Reply::success(status.toJson(), lines.join('\n'));
}

Reply cacheClear(const Command &command, Context &context) {
    ClearRequest request;
    for (const QString &name : command.options.value("kind")) {
        if (name == QLatin1String("all")) {
            request.kinds = allCacheKinds();
            continue;
        }
        const std::optional<CacheKind> kind = parseCacheKind(name);
        if (!kind) {
            Error error = makeError("usage", QStringLiteral("\"%1\" is not a kind of cache.").arg(name),
                                    "Use results, thumbnails, runs or all; with no --kind, results.");
            error.valid = QJsonArray{"results", "thumbnails", "runs", "all"};
            return Reply::failure(error);
        }
        if (!request.kinds.contains(*kind))
            request.kinds.append(*kind);
    }
    if (command.has("workflow")) {
        // The result cache and run.json hold the workflow's absolute path.
        const QFileInfo info(command.value("workflow"));
        request.workflow = info.absoluteFilePath();
    }
    request.node = command.value("node");
    if (command.has("older-than")) {
        const std::optional<qint64> age = parseAge(command.value("older-than"));
        if (!age)
            return Reply::failure(makeError("usage",
                                            QStringLiteral("\"%1\" is not an age.").arg(command.value("older-than")),
                                            "Use a number with s, m, h, d or w, such as 30d or 12h."));
        request.olderThanSeconds = *age;
    }
    request.dryRun = command.has("dry-run");

    const Result<ClearReport> done = CacheManager(folders(context)).clear(request);
    if (!done)
        return Reply::failure(done.errors());
    const ClearReport &report = done.value();

    Errors warnings;
    QStringList lines;
    for (const KindOutcome &outcome : report.kinds) {
        if (!outcome.skipped.isEmpty()) {
            Error warning = makeError("cache_busy",
                                      QStringLiteral("The %1 were left alone: %2.").arg(cacheKindName(outcome.kind), outcome.skipped),
                                      "Clear them when the run has ended.");
            warning.severity = Error::Severity::Warning;
            warnings << warning;
            continue;
        }
        QString line = QStringLiteral("%1 %2: %3 removed, %4")
                           .arg(report.dryRun ? QStringLiteral("Would clear") : QStringLiteral("Cleared"),
                                cacheKindName(outcome.kind))
                           .arg(outcome.removed)
                           .arg(sizeText(outcome.bytes));
        if (outcome.kept > 0)
            line += QStringLiteral(", %1 kept (%2)")
                        .arg(outcome.kept)
                        .arg(outcome.kind == CacheKind::Results ? QStringLiteral("frozen") : QStringLiteral("going or queued"));
        lines << line;
    }
    Reply reply = Reply::success(report.toJson(), lines.join('\n'));
    reply.errors = warnings;
    return reply;
}

} // namespace fugue::cli
