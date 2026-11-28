#pragma once

// The sandbox of a pack's programs (spec 6.7.1, epic #731): bubblewrap, run as a separate program. This module only
// turns "what the program needs" and "what it is handed" into a bwrap command line (no shell: a program and a list of
// arguments) and says whether bwrap can run here. It starts nothing; the runner and the pack manager start the command.
//
//   Run:   the pack folder read-only (its `.venv` writable), the run's work folder writable, the files it is handed
//          read-only, /usr and a few /etc files read-only, a minimal /proc and /dev, an empty /tmp, no home, no network
//          unless `needs.network`, a clean environment.
//   Setup: the pack folder and the uv cache writable, the network on, uv's Python installs read-only, nothing else.

#include "engine/result.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace fugue::sandbox {

// What a node declares (node.json `needs`, 6.3) that the sandbox honours.
struct Needs {
    bool network = false;
    bool gpu = false;        // declared and shown; the device binds are slice 2 of #731
    QStringList readPaths;   // absolute, cleaned
    QStringList writePaths;  // absolute, cleaned

};

// `needs.gpu` is an object (6.3) or `true`; `network` a bool; `files` is a bool (the old form: nothing is granted) or
// `{"read": [...], "write": [...]}` of absolute paths or `~/...` (`home` stands for ~; empty: the user's home folder).
// A path that is relative, or that is, contains or lies inside a path of denied() is an error naming it. Unknown keys
// are not this module's business.
Result<Needs> parseNeeds(const QJsonObject &needs, const QString &home = {});
// What a manifest's `needs` says is wrong for the sandbox, or empty; the node loader calls it so a bad one refuses to load.
QString needsProblem(const QJsonObject &needs);
// The union of several nodes' needs: what the pack as a whole may do.
Needs merged(const Needs &a, const Needs &b);

// Paths no pack may be given, however it asks (6.7.1): keys, the credentials of uv, Fugue's own config, state and pack
// folders (the pack-secrets file and trust.json are in them), and the system folders that hold sockets and devices.
QStringList denied(const QString &home = {});
// Why `path` may not be granted (it is, holds or lies inside a denied path), or empty.
QString deniedReason(const QString &path, const QString &home = {});

// The plain lines the Trust question and `pack info` show: "Can use the network", "Can use the GPU", "Can read ~/X",
// "Can write ~/X"; for a pack that asks for nothing, the sentence that says what it sees; for one that is not
// sandboxed, "NOT sandboxed: this pack's program runs as you.".
QStringList plainLines(const Needs &needs, bool sandboxed, const QString &home = {});

struct Profile {
    enum class Kind { Run, Setup };
    Kind kind = Kind::Run;
    QString packDir;               // the pack's folder
    QString workDir;               // Run: the run's work folder (writable, the program's working directory)
    QStringList writableFolders;   // Run: more folders to write in (a persistent program shares the runs folder)
    Needs needs;
    QStringList handedFiles;       // read-only: the inputs and model files it is given
    QStringList allowedRoots;      // a handed file that is a symlink binds its target only inside one of these
    QString program;               // run inside: the node's entry, or the setup command's program
    QStringList arguments;
    QStringList environment;       // NAME=value, added to the clean one (FUGUE_PROTOCOL, FUGUE_NODE_DIR, ...)
    QString uvCache;               // Setup: the folder uv may write its cache in (made by the caller)
    QString home;                  // the real home, for the uv folders; empty: the user's
};

struct Command {
    QString program;       // bwrap
    QStringList arguments; // everything, the program to run last
    QStringList mounted;   // host paths bound for the files handed over: a program that lives on can see only these
    QStringList skipped;   // handed files left out (a link whose target is outside the allowed roots, a file that is not there)
};

// The bwrap command line for `profile`. Paths are made canonical; the program is looked up on PATH when not absolute.
Command build(const Profile &profile);

// bwrap's path, empty when it is not installed.
QString bwrapPath();
// Whether bwrap is installed and can make a user namespace here: `bwrap --unshare-user --ro-bind / / true`, run once and
// remembered. Not installed, or the kernel or policy refuses it: false.
bool available();
// Why not, and what to do about it, for the error `sandbox_unavailable` (empty when available).
QString unavailableHint();
// For tests: look for bwrap at `path` (an empty string: "no bwrap") and forget the answer, or nullopt to go back to PATH.
void setBwrapForTest(std::optional<QString> path);
// For tests of the pack manager that run real setup commands on a machine without bwrap: a setup then runs as the user.
// Nothing in the app calls it.
void runSetupUnsandboxedForTest(bool on);
// Whether runSetupUnsandboxedForTest is on.
bool setupUnsandboxedForTest();

} // namespace fugue::sandbox
