#pragma once

// A test-only hook for the README tours (tours/, docs/recording-screenshots.md): a tour asks where an item is by objectName
// instead of clicking fixed pixels. Off unless FUGUE_TOUR_PROBE names a folder; then, every 150 ms, a file `ask` there
// (one query per line; a line starting with "#" is ignored) is answered in `reply`. A query is objectNames joined by "/" (each looked for below the one before),
// a name with "*" in it is a wildcard pattern. The reply has, per query, a line "? query" and then one line per visible match:
// "objectName x y width height" in window coordinates, ending with "." (written whole and renamed in place, so it is
// never read half-written). Nothing is written to the window or the app's data.

#include <QObject>

class QQuickWindow;

namespace fugue {

// Starts the hook on `window` when FUGUE_TOUR_PROBE is set (and does nothing otherwise).
void startTourProbe(QQuickWindow *window, QObject *parent);

} // namespace fugue
