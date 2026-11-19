#pragma once

// Fugue's own folders (spec 1.4). XDG variables are honored; the defaults are shown.

#include <QString>

namespace fugue {

QString configDir(); // $XDG_CONFIG_HOME/fugue  (~/.config/fugue)
QString stateDir();  // $XDG_STATE_HOME/fugue   (~/.local/state/fugue)
QString cacheDir();  // $XDG_CACHE_HOME/fugue   (~/.cache/fugue)

} // namespace fugue
