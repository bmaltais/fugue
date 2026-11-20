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

#include <functional>

namespace fugue {

// Where a pack came from. `ref` is a tag or a commit and is required for git.
struct PackSource {
    enum class Kind { Path, Git };
    Kind kind = Kind::Path;
    QString location; // an absolute folder, or a git URL
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
    QStringList setupCommand; // empty when the pack has no setup
    QString setupDescription;
    PackSource source;        // empty location when installed by copying into the folder by hand
    QString oldVersion;       // for an update: the version being replaced
};

enum class Answer { Yes, No, NoHuman };

// What a person is asked. A callback is null when the caller cannot ask: that is NoHuman.
struct PackPrompts {
    std::function<Answer(const PackSummary &)> install;   // add, update: install this pack here?
    std::function<Answer(const PackSummary &)> remove;    // remove this pack?
    std::function<Answer(const PackSummary &)> trust;     // allow this pack's nodes to run?
    std::function<Answer(const PackSummary &)> runSetup;  // run this exact command?
    // What a command prints while it runs (git, setup), a line at a time. May be null.
    std::function<void(const QString &)> log;
};

struct PackAdded {
    PackSummary pack;
    bool setupRan = false;
    bool setupSkipped = false; // the pack has a setup and the person declined it
};

// The installed pack folders' record of where each came from, or an empty source.
PackSource readPackSource(const QString &packFolder);

// A summary of the pack at `folder` (an installed or staged one). Reads pack.json and the
// manifests of its nodes; errors when the pack does not load.
Result<PackSummary> summarizePack(const QString &folder);

// `fugue pack add`. A folder is copied; a git URL (https, ssh, git, file or user@host:path) is
// cloned and `ref` checked out. `packsDir` is where the pack goes.
Result<PackAdded> addPack(const QString &location, const QString &ref, const QString &packsDir,
                          const PackPrompts &prompts);

// `fugue pack update [id]`: each given pack (all of those with a recorded source when `id` is
// empty) is fetched again, at `ref` when given, else at the ref it was installed at. Returns
// the packs that changed. A pack already at that version and commit is left alone.
Result<QList<PackAdded>> updatePacks(const QString &id, const QString &ref, const QString &packsDir,
                                     const PackPrompts &prompts);

// `fugue pack remove`: only a pack in `packsDir`, not one found through a settings path.
// Also forgets its trust.
Result<PackSummary> removePack(const QString &id, const QString &packsDir, TrustStore &trust,
                               const PackPrompts &prompts);

// `fugue pack trust`: asks, then records trust for the installed version.
Result<PackSummary> trustPack(const QString &id, const QString &packsDir, TrustStore &trust,
                              const PackPrompts &prompts);

// A command as one line a person can read and copy: arguments with spaces are quoted.
QString displayCommand(const QStringList &command);

} // namespace fugue
