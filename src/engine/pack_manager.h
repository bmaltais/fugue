#pragma once

// Installing, updating, removing and trusting packs (spec 6.6, 6.7, 6.8, 13.6). The CLI and
// the window both call this; neither decides on its own that the human agreed. Every step
// that needs a human goes through `PackPrompts`, and an answer of "no human here" fails with
// `needs_confirmation`. There is no way to answer for the person.
//
// A pack is installed by copying a folder, or cloning a git repository at a tag or commit,
// into packsDir()/<pack id>. Beside pack.json the folder gets `.fugue-source.json`, which
// says where it came from, so `update` can fetch it again. Updates are explicit; nothing
// updates by itself. A new version asks for trust again because trust is per version.
//
// Nothing here runs a pack's programs. `setup` (6.8) is the one thing that runs on install,
// and only after the person was shown the exact command and said yes.

#include "engine/error.h"
#include "engine/result.h"
#include "engine/trust_store.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

namespace fugue {

// Where a pack came from. `ref` is a tag or a commit and is required for git.
struct PackSource {
    enum class Kind { Path, Git, Bundle };
    Kind kind = Kind::Path;
    QString location; // an absolute folder, or a git URL; for a Bundle the .fugueapp/.fuguepack file it came in (nothing to update from)
    QString ref;      // git only
    QString commit;   // git only: what `ref` resolved to when it was fetched
};

// What the person is asked to agree to.
struct PackSummary {
    QString id;
    QString version;
    QString name;
    QString folder;
    QStringList runtimeKinds; // "command", "native", ... of the pack's nodes, sorted
    QStringList permissions;  // "network", "files", "gpu": what the nodes declare (6.3), sorted
    QStringList nodes;        // node type ids
    QStringList secrets;      // the names of the secrets the pack reads (6.4 `settings`), in pack.json's order; never values
    QStringList secretsSet;   // of those, the ones already set for this pack id from this same source (an update keeps them)
    QStringList secretsOtherSource; // names set for this pack id from ANOTHER source: this pack does not get them (6.4.1)
    QStringList setupCommand; // empty when the pack has no setup
    QString setupDescription;
    qint64 setupSize = -1;    // bytes its setup downloads, when pack.json's setup says (`size`); -1 when it does not
    PackSource source;        // empty location when installed by copying into the folder by hand
    QString oldVersion;       // for an update: the version being replaced (empty when it did not load)
    bool updating = false;    // the pack replaces an installed one
};

// The process group of the setup that is running now, 0 when none. A setup runs in a group of its own, so the terminal's Ctrl-C
// does not reach it: a command that was asked twice to stop (the CLI's signal handler) kills that group itself (#418).
std::atomic<qint64> &runningSetupGroup();

enum class Answer { Yes, No, NoHuman };

// What a person is asked. A callback is null when the caller cannot ask: that is NoHuman.
struct PackPrompts {
    std::function<Answer(const PackSummary &)> install;   // add, update: install this pack here?
    std::function<Answer(const PackSummary &)> remove;    // remove this pack?
    std::function<Answer(const PackSummary &)> trust;     // allow this pack's nodes to run?
    std::function<Answer(const PackSummary &)> runSetup;  // run this exact command?
    // What a command prints while it runs (git, setup), a line at a time. May be null.
    std::function<void(const QString &)> log;
    // True when the person stopped the work: git and a setup are killed, and the rest is skipped.
    std::function<bool()> cancelled;
    // A git clone above this many bytes is refused (0: no limit). A pack is a few files.
    qint64 maxCloneBytes = 0;
    // How long a change waits for the packs folder's lock, held by another command (#418); then `pack_busy`.
    int lockWaitMs = 2000;
    // Brings a git source into `workFolder` (an empty folder) and returns the commit it is at; null runs git. For tests, so
    // nothing reaches a network.
    std::function<Result<QString>(const PackSource &, const QString &workFolder)> fetch;
};

struct PackAdded {
    PackSummary pack;
    bool setupRan = false;
    bool setupSkipped = false; // the pack has a setup and the person declined it
};

// Whether the setup `command` has run to its end in this installed pack folder: a marker (`.fugue-setup.json`) the
// install or `setUpPack` writes when it succeeds. A pack installed before the marker existed has none.
bool packSetupDone(const QString &packFolder, const QStringList &command);

// The installed pack folders' record of where each came from, or an empty source.
PackSource readPackSource(const QString &packFolder);

// A summary of the pack installed at `folder`. Reads pack.json and the manifests of its nodes; errors when the pack does
// not load. Like any load it records the digest of its secrets for trust (6.7); a staged copy is summarized without that.
Result<PackSummary> summarizePack(const QString &folder);

// A pack read into a staging folder (a folder is copied, a git address cloned), not yet installed. Installing it
// (installPrepared) puts that same copy in place, so what was shown is what is installed. The staging folder goes with the
// last reference to it.
struct PreparedPack {
    PackSummary summary; // as `addPack` would ask about it (folder: where it would go)
    QString staging;
    QString folder;      // the staged pack folder
    PackSource source;
    PreparedPack() = default;
    PreparedPack(const PreparedPack &) = delete;
    ~PreparedPack();
};

// True for what a git source may be: https, ssh, git, file or user@host:path.
bool looksLikeGitUrl(const QString &text);
// True when `a` and `b` are the same pack as a person reads it: id, version, setup command, permissions, secrets, runtime kinds, nodes
// and (when `a` names one) the commit. What a yes covers, and no more.
bool samePack(const PackSummary &shown, const PackSummary &now);

// Reads the pack at `source` into a staging folder and checks it (it loads, no symbolic links, not installed at that major
// already). Nothing is installed and nothing is asked. `source.ref` is required for git.
Result<std::shared_ptr<PreparedPack>> preparePack(const PackSource &source, const QString &packsDir, const PackPrompts &prompts = {});
// Asks `prompts.install` and puts the prepared pack in place, as `addPack` does after its staging.
Result<PackAdded> installPrepared(const std::shared_ptr<PreparedPack> &prepared, const QString &packsDir, TrustStore &trust,
                                  const PackPrompts &prompts);

// `fugue pack add`. A folder is copied; a git URL (https, ssh, git, file or user@host:path) is
// cloned and `ref` checked out. `packsDir` is where the pack goes.
//
// The setup question is asked before anything changes, and a setup that fails puts the pack
// (or, for an update, the old version) back as it was. A new, updated or re-installed pack is
// not trusted: its id's trust is forgotten (6.7). The id may not be found in any search path.
Result<PackAdded> addPack(const QString &location, const QString &ref, const QString &packsDir, TrustStore &trust,
                          const PackPrompts &prompts);

// `fugue pack update [id]`: each given pack (all of those with a recorded source when `id` is
// empty) is fetched again, at `ref` when given, else at the ref it was installed at. Returns
// the packs that changed. A pack already at that commit under that ref is left alone. `ref`
// needs an id. For all packs a pack that fails is a warning and the others go on.
Result<QList<PackAdded>> updatePacks(const QString &id, const QString &ref, const QString &packsDir, TrustStore &trust,
                                     const PackPrompts &prompts);

// `fugue pack remove`: only a pack in `packsDir`, not one found through a settings path.
// Also forgets its trust.
Result<PackSummary> removePack(const QString &id, const QString &packsDir, TrustStore &trust,
                               const PackPrompts &prompts);

// Runs the setup of an installed pack (folder name `id`, as `trustPack` takes) after asking, and records that it ran.
// `setup_declined` when the answer is no, `setup_failed` when it fails. A pack with no setup is returned as it is.
Result<PackSummary> setUpPack(const QString &id, const QString &packsDir, const PackPrompts &prompts);

// `fugue pack trust`: asks, then records trust for the installed version.
Result<PackSummary> trustPack(const QString &id, const QString &packsDir, TrustStore &trust,
                              const PackPrompts &prompts);

// A command as one line a person can read and copy: arguments with spaces are quoted.
QString displayCommand(const QStringList &command);

} // namespace fugue
