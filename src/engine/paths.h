#pragma once

// Fugue's own folders (spec 1.4). XDG variables are honored; the defaults are shown. Under a FUGUE_HOME
// root (engine/fugue_home.h) they all lie in it.

#include <QString>
#include <QStringList>

namespace fugue {

QString configDir(); // $XDG_CONFIG_HOME/fugue  (~/.config/fugue)
QString stateDir();  // $XDG_STATE_HOME/fugue   (~/.local/state/fugue)
QString dataDir();   // $XDG_DATA_HOME/fugue    (~/.local/share/fugue)
QString cacheDir();  // $XDG_CACHE_HOME/fugue   (~/.cache/fugue)
// The ordinary session's folders even from inside a project (spec 1.4.2): the XDG folders as they were before
// setFugueHome redirected them (it saves them once in FUGUE_BASE_XDG_{CONFIG,DATA,STATE}_HOME). Without a root
// they are configDir(), dataDir() and stateDir(). The project registry, the default location of new projects
// and, later, what a project inherits are found through these.
QString baseConfigDir();
QString baseDataDir();
QString baseStateDir();
// The 512 px copies of the pictures the canvas shows (spec 12.7): <cacheDir>/thumbnails.
QString thumbnailsDir();
// The media library's thumbnails of the user's own files (spec 12.7): <cacheDir>/thumbs.
QString mediaThumbnailsDir();
// The folder Fugue may write documents into besides the output folder (workflows, templates): ~/Documents/Fugue,
// or <FUGUE_HOME>/documents.
QString documentsDir();
// The folder of private per-session files (copies for an external viewer): $XDG_RUNTIME_DIR, else the desktop's
// runtime folder; <FUGUE_HOME>/run under a root. Empty when there is none.
QString runtimeDir();
// providers.json as the messages name it: "~/.config/fugue/providers.json", or its real path under FUGUE_HOME.
QString providersFilePath();
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

// A folder of Fugue's own, then each of `extra` (from settings.json) with "~" expanded, cleaned. A
// relative entry is left out: it would mean a different folder to the window and the CLI. A
// folder named twice counts once. Pack and model search paths are made this way.
QStringList searchPaths(const QString &own, const QStringList &extra);

} // namespace fugue
