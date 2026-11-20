#pragma once

// Fugue's own folders (spec 1.4). XDG variables are honored; the defaults are shown.

#include <QString>

namespace fugue {

QString configDir(); // $XDG_CONFIG_HOME/fugue  (~/.config/fugue)
QString stateDir();  // $XDG_STATE_HOME/fugue   (~/.local/state/fugue)
QString dataDir();   // $XDG_DATA_HOME/fugue    (~/.local/share/fugue)
QString cacheDir();  // $XDG_CACHE_HOME/fugue   (~/.cache/fugue)
// The default output folder of Save nodes: Fugue under the user's pictures folder
// ($XDG_PICTURES_DIR, else the desktop's setting, else ~/Pictures): ~/Pictures/Fugue, unless
// settings.json's output_folder names another (engine/settings.h).
QString outputDir();
// The output folder when settings.json names none.
QString defaultOutputDir();

// Where pasted and dropped images are kept (spec 1.4, 14.1): one folder for every workflow,
// outputDir()/inputs (~/Pictures/Fugue/inputs, so it follows the output folder
// wherever the pictures folder is, and is visible to the user), unless settings.json's
// inputs_folder names another. Files in it are named by content hash, so the same image is one file.
QString inputsDir();
// The `inputs` folder beside a saved workflow file, where a save copies the images it uses
// so the workflow stays portable.
QString inputsDirBeside(const QString &workflowPath);

// "~" and "~/..." as the home folder (the file picker's paths and hand-written workflows
// both use them); any other text as it is. The scheduler applies it to every literal of a
// `file` or `folder` input before the node sees it or its key is made (10.1).
QString expandHome(const QString &path);

} // namespace fugue
