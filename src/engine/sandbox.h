#pragma once

// The sandbox of a pack's programs (spec 6.7.1, epic #731): bubblewrap, run as a separate program. This module turns "what the
// program needs" and "what it is handed" into a bwrap command line (no shell: a program and a list of arguments) and says whether
// bwrap can run here. It starts nothing; the runner and the pack manager start the command.
//
// What is bound is bound by descriptor, never by a path string a program could swap for a link between the check and the
// start: build() opens each folder and file with openat2 and no links (engine/beneath_fs.h), checks the path the kernel has
// for the descriptor, and names it to bwrap with --ro-bind-fd / --bind-fd. The descriptors travel in Command::descriptors,
// inherited by bwrap only (the caller clears close-on-exec for them in the child and closes them once it started).
//
//   Run:   the pack folder read-only (its `.venv` writable), the run's work folder writable, the files it is handed and the
//          paths of needs.files.read read-only, /usr and a few /etc files read-only, a minimal /proc and /dev, an empty /tmp,
//          no home, no network unless `needs.network`, a clean environment.
//   Setup: the pack folder read-only but its `.venv`, the uv cache writable, the network on, uv's Python installs read-only.

#include "engine/beneath_fs.h"
#include "engine/result.h"

#include <QJsonObject>
#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace fugue::sandbox {

// What a node declares (node.json `needs`, 6.3) that the sandbox honours.
struct Needs {
    bool network = false;
    bool gpu = false;        // the device nodes of NVIDIA's driver are bound (and /dev/dri/renderD* when gpu.api is "vulkan")
    QString gpuApi;          // gpu.api as declared ("any", "cuda", "vulkan"), empty when none
    QStringList readPaths;   // absolute, cleaned, `~` as the canonical home
};

// `needs.gpu` is an object (6.3) or `true`; `network` a bool; `files` is a bool (the old form: nothing is granted) or
// `{"read": [...]}` of absolute paths or `~/...` (`home` stands for ~; empty: the user's home folder). A `write` list that is
// not empty is refused: write grants are not supported yet (a program returns files as outputs). A `read` path must pass
// grantProblem(). Unknown keys are not this module's business.
Result<Needs> parseNeeds(const QJsonObject &needs, const QString &home = {});
// What a manifest's `needs` says is wrong for the sandbox, or empty; the node loader calls it so a bad one refuses to load.
QString needsProblem(const QJsonObject &needs);
// The union of several nodes' needs: what the pack as a whole may do.
Needs merged(const Needs &a, const Needs &b);

// The areas no read grant may be, hold or lie in (6.7.1): keys and credentials, the places a launcher could be planted, Fugue's
// own folders (this session's and the base ones: config, data, state, cache), the root of every project in the registry and
// of this session, ~/snap, and the system folders that hold sockets, devices and configuration. Canonical paths.
QStringList denied(const QString &home = {});
// Why `path` may not be granted for reading, or empty (6.7.1): it is ~ itself, has a dot component under ~, is, holds or lies
// in a denied() area, lies in /tmp (but not in the home folder) or in a folder holding a project.json of a Fugue project, or
// lies in the output folder. `path` is taken as it is (the caller passes a canonical one).
QString grantProblem(const QString &path, const QString &home = {});
// Whether a file handed to a program (an input, a model; `path` canonical) may never be: keys and credentials, Fugue's own
// config, state and packs folders, the system folders, and the root of another project. Fugue's data and cache folders are
// not among them: models and runs live there.
bool handedDenied(const QString &path, const QString &home = {});
// Why `path` may not be granted (it is, holds or lies inside a denied path), or empty.
QString deniedReason(const QString &path, const QString &home = {});

// The plain lines the Trust question and `pack info` show: "Can use the network", "Can use the GPU", "Can read ~/X"; for a
// pack that asks for nothing, the sentence that says what it sees; for one that is not sandboxed, "NOT sandboxed: this
// pack's program runs as you.".
QStringList plainLines(const Needs &needs, bool sandboxed, bool hasSetup = false, const QString &home = {});
// What the pack asks for, one plain line each and nothing else (no line about the sandbox itself): the network (and, as it is
// the host's, the services and the X11/Xwayland display of this computer), the setup's network, the GPU, each read grant. What
// a person is shown as "Declares", and what a later look at the pack is compared with (samePack).
QStringList needsLines(const Needs &needs, bool hasSetup = false, const QString &home = {});

struct Profile {
    enum class Kind { Run, Setup };
    Kind kind = Kind::Run;
    QString packDir;               // the pack's folder
    QString workDir;               // Run: the run's work folder (writable, the program's working directory)
    Needs needs;
    QStringList modelFiles;        // read-only: the model files it is given; each must lie in one of `modelRoots`, else refused
    QStringList modelRoots;        // the model folders
    QStringList handedFiles;       // read-only: the media files it is given; a link among them binds its target only inside a model root
    QString program;               // run inside: the node's entry, or the setup command's program
    QStringList arguments;
    QStringList environment;       // NAME=value, added to the clean one (FUGUE_PROTOCOL, FUGUE_NODE_DIR, ...)
    QString uvCache;               // Setup: the folder uv may write its cache in (made by the caller): Fugue's own, per pack
    int exchangeFd = -1;           // Run: the open exchange folder of a program kept between runs (writable), bound at `exchangeDir`
    QString exchangeDir;
    QStringList gpuNodes;          // the device paths to bind when needs.gpu (empty: found under /dev)
    QString home;                  // the real home, for the uv folders; empty: the user's
};

// The descriptors a command line names (`--ro-bind-fd N`): open in the parent with close-on-exec, to be inherited by bwrap
// alone. They close when the last copy of the Command goes.
struct Descriptors {
    std::vector<beneath::Fd> held;
    QList<int> numbers() const;
};

struct Command {
    QString program;       // bwrap
    QStringList arguments; // everything, the program to run last
    std::shared_ptr<Descriptors> descriptors = std::make_shared<Descriptors>();
    QStringList mounted;   // host paths bound for the files handed over: a program that lives on can see only these
    QStringList skipped;   // handed files left out (a link whose target is outside the model roots, a file that is not there)
    QString refused;       // why it must not run (a read grant or a model file that may not be bound, or cannot be opened)
};

// The bwrap command line for `profile`. Paths are made canonical, then opened without following links; the program is looked
// up on PATH when not absolute.
Command build(const Profile &profile);

// `text` with its control characters made spaces (escape sequences, the other C0 characters, DEL and C1), so a program's
// words can never drive a terminal: a tab stays, and newlines when `keepNewlines`. For everything a program says that reaches
// a terminal, a log or an error message.
QString printable(QString text, bool keepNewlines = false);

// The environment bwrap itself is started with: PATH=/usr/bin:/bin and nothing else, so bwrap's own process inside the sandbox
// (pid 1, whose /proc/1/environ the program can read) holds none of Fugue's variables. The program's is set by --setenv.
QProcessEnvironment bwrapEnvironment();

// /usr/bin/bwrap, empty when it is not there: never looked up on PATH, which a pack's program or a project could shape.
QString bwrapPath();
// uv's folder of Python installs as uv finds it (UV_PYTHON_INSTALL_DIR, XDG_DATA_HOME/uv/python, ~/.local/share/uv/python).
QString uvPythonDir(const QString &home = {});
// A fresh uv cache for one setup of a pack: Fugue's own, `<cache>/uv/<pack id>+<128 random bits>`, never the user's ~/.cache/uv
// (uv links its files into venvs) nor one another setup (another source or version of the same id) could have filled. The
// caller makes it and removes it when the setup ends; `pack remove` sweeps what a crash left.
QString uvCacheFor(const QString &packId);

// Whether bwrap (0.8 or newer) is installed and can make a user namespace here with the flags of a real run
// (`--unshare-all --unshare-user --disable-userns --new-session --die-with-parent`), tried once and remembered, and the kernel
// has openat2 (beneath::available(): Linux 5.6 or newer), without which nothing can be bound by descriptor.
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
