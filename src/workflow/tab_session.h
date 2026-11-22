#pragma once

// The tabs of the last session (spec 12.10, #212): which saved workflows were open and which one
// was shown, kept in `tabs.json` in the state folder of the root (the Default's own, or the
// project's: `stateDir()`), so the next start opens them again. Pure rules, no window.
//
//   {"format": 1, "tabs": ["/path/a.fugue", ...], "active": "/path/a.fugue"}
//
// Only tabs that have a file are listed. A tab with no file (a new workflow, a template, a paste)
// has no place to come back from: its unsaved content is the recovery copies' business
// (workflow/recovery_store.h), which are offered at start as before.

#include <QString>
#include <QStringList>

#include "engine/result.h"

namespace fugue {

struct TabSession {
    QStringList paths; // in tab order
    QString active;    // the shown tab's path; empty when it has none
};

// Writes the session whole or not at all (engine/atomic_file), making the folder. Empty when done.
Errors saveTabSession(const QString &path, const TabSession &session);

// Reads it. A missing file, one that is not JSON or not this format, is an empty session; entries
// that are not non-empty strings are dropped.
TabSession loadTabSession(const QString &path);

// What a start does with a session.
struct TabRestore {
    QStringList paths;   // the files to open, in order, each once
    QString active;      // the one to show: the session's when it is still there, else the last
    QStringList skipped; // what is not there any more (or not a file, or not an absolute path)
};
TabRestore planRestore(const TabSession &session);

} // namespace fugue
