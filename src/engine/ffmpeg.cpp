#include "ffmpeg.h"

#include <QProcess>
#include <QStandardPaths>

namespace fugue::ffmpeg {

Result<QString> program(const QString &name) {
    const QString path = QStandardPaths::findExecutable(name);
    if (path.isEmpty())
        return makeError(QStringLiteral("internal"), QStringLiteral("%1 was not found.").arg(name),
                         QStringLiteral("Install ffmpeg (pacman -S ffmpeg); Fugue uses it for video and audio."));
    return path;
}

Result<QByteArray> run(const QString &name, const QStringList &args, const QString &code, int timeoutMs) {
    const Result<QString> path = program(name);
    if (!path)
        return path.error();
    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(*path, args);
    if (!process.waitForStarted())
        return makeError(QStringLiteral("internal"), QStringLiteral("%1 could not be started.").arg(name));
    // No timeout: a long video takes as long as it takes, and cancel is the
    // scheduler's (it ends the node).
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return makeError(code, QStringLiteral("%1 took too long.").arg(name));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = tail(process.readAllStandardError());
        return makeError(code, detail.isEmpty() ? QStringLiteral("%1 failed.").arg(name) : detail);
    }
    return process.readAllStandardOutput();
}

Result<QByteArray> runWatched(const QStringList &args, const QString &code,
                              const std::function<void(double seconds)> &progress,
                              const std::function<bool()> &cancelled) {
    const Result<QString> path = program(QStringLiteral("ffmpeg"));
    if (!path)
        return path.error();
    // Already cancelled: do not start. A short job can end inside the first poll, so the loop
    // below would never have seen the cancel (a flaky test of Upscale Video with Model).
    if (cancelled && cancelled())
        return makeError(QStringLiteral("cancelled"), QStringLiteral("The node was cancelled."));
    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(*path, QStringList{QStringLiteral("-progress"), QStringLiteral("pipe:1"), QStringLiteral("-nostats")} + args);
    if (!process.waitForStarted())
        return makeError(QStringLiteral("internal"), QStringLiteral("ffmpeg could not be started."));
    QByteArray pending, errors;
    const auto drain = [&] {
        pending += process.readAllStandardOutput();
        errors += process.readAllStandardError();
        qsizetype end;
        while ((end = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(end).trimmed();
            pending.remove(0, end + 1);
            // out_time_us is -9223372036854775807 ("N/A") until the first frame is written.
            if (line.startsWith("out_time_us=")) {
                bool ok = false;
                const qint64 us = line.mid(12).toLongLong(&ok);
                if (ok && us >= 0 && progress)
                    progress(double(us) / 1e6);
            }
        }
    };
    while (!process.waitForFinished(100)) {
        drain();
        if (cancelled && cancelled()) {
            process.kill();
            process.waitForFinished(3000);
            return makeError(QStringLiteral("cancelled"), QStringLiteral("The node was cancelled."));
        }
    }
    drain();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = tail(errors);
        return makeError(code, detail.isEmpty() ? QStringLiteral("ffmpeg failed.") : detail);
    }
    return QByteArray();
}

QStringList trimArgs(const VideoEdits &edits) {
    if (!edits.trim)
        return {};
    // Before -i, so ffmpeg seeks the input; since it then decodes, the cut is
    // exact to the frame, not to the nearest keyframe.
    return {QStringLiteral("-ss"), QString::number(edits.trim->start, 'f', 6), QStringLiteral("-t"),
            QString::number(edits.trim->end - edits.trim->start, 'f', 6)};
}

QString cropFilter(const VideoEdits &edits) {
    if (!edits.crop)
        return {};
    return QStringLiteral("crop=%1:%2:%3:%4:exact=1")
        .arg(edits.crop->width)
        .arg(edits.crop->height)
        .arg(edits.crop->x)
        .arg(edits.crop->y);
}

QString tail(const QByteArray &stderrText, int lines) {
    QStringList all = QString::fromUtf8(stderrText).trimmed().split(QLatin1Char('\n'));
    if (all.size() > lines)
        all = all.mid(all.size() - lines);
    return all.join(QLatin1Char('\n')).trimmed();
}

} // namespace fugue::ffmpeg
