#pragma once

// One root for everything Fugue writes (spec 1.4.1, epic #204): FUGUE_HOME, or `--home <dir>` on the
// window and the command line. Set, it is the only place a session reads its own state from and writes
// to: settings, state, data, caches, saved results, the runtime and temp folders. Unset, nothing
// changes: Fugue behaves exactly as before.

#include <QString>

namespace fugue {

// Reads `--home <dir>` or `--home=<dir>` out of argv (removing it, so the command parser never sees it),
// or FUGUE_HOME, and makes the root's folders. Then points XDG_CONFIG_HOME, XDG_DATA_HOME,
// XDG_STATE_HOME, XDG_CACHE_HOME, XDG_RUNTIME_DIR and TMPDIR into it (so QStandardPaths and every child
// process, the helper, ffmpeg and the packs, inherit them), and FUGUE_HOME itself as an absolute path.
// HOME is left alone. Call it first in main(), before any Qt object and any thread. Returns false and
// fills `error` when the root cannot be used (a relative-path trick, a file in the way, no `--home`
// value); nothing is changed then. Without a root it does nothing and returns true.
bool applyFugueHome(int &argc, char **argv, QString *error);

// Projects (spec 1.4.2) pick the root too, with this order: `--home`, `--project <name|path>`, FUGUE_HOME,
// FUGUE_PROJECT, else the ordinary session (Default). `--home` with `--project` is an error. `default`
// names the ordinary session. "Last used" is not part of it: a script must always get the same session.
//
// The window only, after applyFugueHome when nothing selected a root: opens the project the window last
// ran in (the registry's `last`), if it is still a project. A gone or broken one means Default, silently.
bool applyLastUsedProject(QString *error);

// True when the root is a project the person chose: `--project`, FUGUE_PROJECT or the last used one. A root
// given by FUGUE_HOME or `--home` (scripts, tests, an agent's scratch home) is not, and is never remembered
// as the last used project, even when it holds a project.json.
bool rootIsChosenProject();

// Makes the root's own folders that are not XDG ones (`documents/`, `outputs/`): the suggested place for
// workflows and the default output folder must exist before anything is saved or a dialog opens there.
// Called by setFugueHome (so every session heals a root made before this) and by `project new`.
bool makeRootFolders(const QString &root, QString *error);

// The same with the root given (the unit tests'): an absolute folder.
bool setFugueHome(const QString &root, QString *error);

// The root, an absolute clean path, or empty when Fugue runs without one.
QString fugueHome();

// A folder under the root (`outputs`, `documents`), or empty without a root: the caller keeps its own
// default then. The one function the home-dependent defaults go through.
QString fugueHomeDir(const QString &name);

} // namespace fugue
