#pragma once

// Fugue's own folders (spec 1.4). XDG variables are honored; the defaults are shown.

#include <QString>

namespace fugue {

QString configDir(); // $XDG_CONFIG_HOME/fugue  (~/.config/fugue)
QString stateDir();  // $XDG_STATE_HOME/fugue   (~/.local/state/fugue)
QString cacheDir();  // $XDG_CACHE_HOME/fugue   (~/.cache/fugue)
// The default output folder of Save nodes: Fugue under the user's pictures folder
// ($XDG_PICTURES_DIR, else the desktop's setting, else ~/Pictures): ~/Pictures/Fugue.
QString outputDir();

// "~" and "~/..." as the home folder (the file picker's paths and hand-written workflows
// both use them); any other text as it is. The scheduler applies it to every literal of a
// `file` or `folder` input before the node sees it or its key is made (10.1).
QString expandHome(const QString &path);

} // namespace fugue
