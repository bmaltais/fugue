#pragma once

// How the engine runs ffmpeg and ffprobe, which it calls as programs found on
// PATH. Internal to the media code.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "engine/media.h"
#include "engine/result.h"

namespace fugue::ffmpeg {

// The absolute path of `name` ("ffmpeg" or "ffprobe") on PATH, or an internal
// error that says to install ffmpeg.
Result<QString> program(const QString &name);

// Runs a program to the end and returns its stdout. A failure carries the last
// lines of its stderr, with `code`.
Result<QByteArray> run(const QString &name, const QStringList &args, const QString &code);

// The arguments that go before "-i <file>" to apply a pending trim, and the
// "-vf" filter for a pending crop (empty when there is none).
QStringList trimArgs(const VideoEdits &edits);
QString cropFilter(const VideoEdits &edits);

// The last `lines` lines of a program's stderr, for an error message.
QString tail(const QByteArray &stderrText, int lines = 20);

} // namespace fugue::ffmpeg
