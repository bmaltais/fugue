#pragma once

// Loading packs from disk (spec 6.4, 6.5). A pack is a folder with `pack.json` and one
// subfolder per node holding that node's `node.json`. loadPacks() reads only manifests and
// registers the types (the registry's own checks apply); programs, libraries and group
// definitions are opened on first use, so nothing here looks for `runtime.entry`.
//
// A manifest that fails validation disables that node, or the whole pack when it is
// pack.json that fails, with a reason a person can read; it never stops the rest. The
// returned report says what loaded and what did not, for the window and `fugue pack list`.
//
// The ids `fugue` and `local` belong to Fugue: a pack folder on disk cannot claim them, so
// it cannot pass for a built-in and be trusted without a prompt (6.7). The built-in pack
// is loaded with PackOrigin::BuiltIn, from the compiled-in resources, through the same code.

#include "engine/error.h"
#include "engine/registry.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace fugue {

// The ABI of this host's native plugins (6.9): `runtime.abi` is "<major>.<min_host_minor>".
constexpr int kHostAbiMajor = 1;
constexpr int kHostAbiMinor = 0;

// ~/.local/share/fugue/packs (dataDir() in paths.h, then "packs").
QString packsDir();

// The folders to look in, in order: packsDir(), then settings.json's `pack_paths`
// (~ expanded). A path holds packs, one per subfolder.
QStringList packSearchPaths();

enum class PackOrigin {
    BuiltIn, // ships with Fugue; may use the reserved ids and `runtime.kind: "builtin"`
    Disk,
};

// A node of a pack that did not load, or a node folder pack.json does not list.
struct PackProblem {
    QString node;    // the subfolder name
    QString reason;  // one sentence
};

struct LoadedPack {
    QString path;    // the pack's folder
    QString id;      // empty when pack.json could not be read
    QString version;
    QString name;
    bool enabled = false;       // false: nothing of the pack is registered
    Errors errors;              // why a pack is disabled; for one that loaded, warnings and declared-type problems
    QStringList nodes;          // type ids registered, sorted
    QList<PackProblem> disabled; // nodes that did not load, in pack.json's order
};

struct PackReport {
    QList<LoadedPack> packs;   // in search order
    bool hasProblems() const;  // any pack disabled or any node disabled
};

// Loads one pack folder into the registry: its declared types, then its nodes, and sets the
// installed version of the pack (Registry::setPackVersion) when the pack is enabled.
LoadedPack loadPack(Registry &registry, const QString &folder, PackOrigin origin = PackOrigin::Disk);

// Loads every pack found under `searchPaths`, each subfolder holding a pack.json. A pack id
// found twice loads once, from the earlier path; the later one is disabled. A path that does
// not exist is skipped.
PackReport loadPacks(Registry &registry, const QStringList &searchPaths);

// loadPacks(registry, packSearchPaths()), the problems also written to stderr, one line each.
// What the window and the CLI call after the built-ins.
PackReport loadInstalledPacks(Registry &registry);

} // namespace fugue
