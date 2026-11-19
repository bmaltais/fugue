// Video and audio handles: probing with ffprobe and applying pending edits
// with ffmpeg.

#include "media.h"

namespace fugue {

Result<MediaHandle> MediaHandle::probeWithFfprobe(MediaType, const QString &, FileHashes &) {
    return makeError(QStringLiteral("internal"), QStringLiteral("Not built yet."));
}

Result<MediaHandle> MediaHandle::withEditsApplied(const QString &, const QString &, FileHashes &) const {
    return makeError(QStringLiteral("internal"), QStringLiteral("Not built yet."));
}

} // namespace fugue
