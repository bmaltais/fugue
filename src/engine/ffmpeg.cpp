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

Result<QByteArray> run(const QString &name, const QStringList &args, const QString &code) {
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
    process.waitForFinished(-1);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = tail(process.readAllStandardError());
        return makeError(code, detail.isEmpty() ? QStringLiteral("%1 failed.").arg(name) : detail);
    }
    return process.readAllStandardOutput();
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
