#pragma once

// The launcher entries of apps (#398, epic #397, spec 12.18): an XDG desktop entry per app, so an app shows in the desktop's
// launcher like any program and starts as `fugue --app <ref>` in its own window. This is the whole store: one small file in
// $XDG_DATA_HOME/applications and, when the app has a picture, a copy of it in $XDG_DATA_HOME/fugue/app-icons (the entry's
// Icon is that path, so no icon theme or cache is needed). Nothing here knows the window or the App view: a test gives it a
// scratch data folder.
//
// An app is named by its `ref`, what `fugue --app` takes: a template's id, or the absolute path of a workflow file, a template
// folder or a package. The entry's file is `fugue-app-<key>.desktop`; the key is the id as it is, or for a path the file's
// stem and a short hash of the path, so two apps of the same name never share an entry. The window an entry starts takes the
// same name as its desktop file (QGuiApplication::setDesktopFileName), which is what ties the window to the entry in the
// launcher and the task bar; entries Fugue did not write are never touched.

#include <QString>

namespace fugue {

struct LauncherApp {
    QString ref;     // what follows `--app`
    QString name;    // the entry's Name
    QString comment; // one line, may be empty
    QString icon;    // a picture file to copy beside the entry; empty or missing: the entry uses Fugue's own icon
};

class LauncherEntries {
public:
    // `dataHome`: where the entries go; empty is the user's own ($XDG_DATA_HOME, else ~/.local/share).
    // `program`: what the entry runs; empty is `fugue`, or the AppImage's own path when run from one.
    explicit LauncherEntries(QString dataHome = {}, QString program = {});

    // The name the entry and the window share (without `.desktop`): `fugue-app-<key>`.
    static QString desktopName(const QString &ref);

    bool has(const QString &ref) const;
    // Writes the entry (and its icon), replacing the one the app had. Returns why not, or empty.
    QString add(const LauncherApp &app) const;
    // Removes the entry and its icon; none is not a problem. Returns why not, or empty.
    QString remove(const QString &ref) const;

    QString entryPath(const QString &ref) const;
    QString iconPath(const QString &ref, const QString &suffix) const;

    // `Exec` quoted as the desktop entry spec asks (a path with a space, a quote or a `$` stays one argument).
    static QString execLine(const QString &program, const QString &ref);

private:
    QString m_dataHome;
    QString m_program;
};

} // namespace fugue
