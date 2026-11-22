#pragma once

// How the engine runs ffmpeg and ffprobe, which it calls as programs found on
// PATH. Internal to the media code.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

#include "engine/media.h"
#include "engine/result.h"

namespace fugue::ffmpeg {

// The absolute path of `name` ("ffmpeg" or "ffprobe") on PATH, or an internal
// error that says to install ffmpeg.
Result<QString> program(const QString &name);

// Runs a program to the end and returns its stdout. A failure carries the last
// lines of its stderr, with `code`. A `timeoutMs` of 0 or more kills the program when it runs longer
// and fails with `code` (for a short probe of a file that may be on a slow disk); -1 waits for ever.
Result<QByteArray> run(const QString &name, const QStringList &args, const QString &code, int timeoutMs = -1);

// As run(), for a long job: ffmpeg is given `-progress pipe:1`, `progress` is called with the seconds
// of output written so far as they advance, and `cancelled` is polled about every 100 ms; when it
// is true ffmpeg is killed and the result is the error `cancelled`. Call it from a worker thread.
Result<QByteArray> runWatched(const QStringList &args, const QString &code,
                              const std::function<void(double seconds)> &progress,
                              const std::function<bool()> &cancelled);

// The arguments that go before "-i <file>" to apply a pending trim, and the
// "-vf" filter for a pending crop (empty when there is none).
QStringList trimArgs(const VideoEdits &edits);
QString cropFilter(const VideoEdits &edits);

// The last `lines` lines of a program's stderr, for an error message.
QString tail(const QByteArray &stderrText, int lines = 20);

} // namespace fugue::ffmpeg
