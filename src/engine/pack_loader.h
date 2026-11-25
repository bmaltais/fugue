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
#include "engine/pack_secrets.h"
#include "engine/registry.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace fugue {

// The ABI of this host's native plugins (6.9): `runtime.abi` is "<major>.<min_host_minor>".
constexpr int kHostAbiMajor = 1;
constexpr int kHostAbiMinor = 0;

// A pack folder is named after the pack's id, or `<id>@<major>` (the major of its version) when
// another major of the pack is installed beside it (6.6). The first is how packs always lived.
QString packFolderName(const QString &id, int major);
struct PackRef {
    QString id;
    int major = -1; // -1: no @<major> in the name
};
PackRef parsePackRef(const QString &folderName);

// ~/.local/share/fugue/packs (dataDir() in paths.h, then "packs").
QString packsDir();

// The folders to look in, in order: packsDir(), then settings.json's `pack_paths`
// (~ expanded; a relative path is ignored). A path holds packs, one per subfolder, each
// in a folder named after the pack's id.
QStringList packSearchPaths();

// What a project that inherits `packs` (spec 1.4.2) also sees, read-only and after its own: the Default
// project's packs folder and its `pack_paths`. Empty for the Default project, a root that does not inherit
// `packs`, and an anonymous root.
QStringList inheritedPackPaths();

// A pack of the Default project, as this session sees it: listed and usable, never written.
struct InheritedPack {
    QString id;
    QString version;
    QString folder; // the Default's folder it runs from
    int major = -1;
};
// The Default's packs this session can use now: those found in inheritedPackPaths(), except the ones an own
// pack of the same id and major hides. The built-in ids and the Default's group library (`local`) are never
// among them. A pack that does not read is left out.
QList<InheritedPack> inheritedPacks();
// Whether the pack of this id and version is one of them (the Default's trust counts for those only).
bool isInheritedPack(const QString &id, const QString &version);
// The folder an id (`<id>` or `<id>@<major>`) names among the inherited packs, or empty.
QString inheritedPackFolder(const QString &folderName);

// The words a person reads where an inherited pack is listed: it runs from the Default project's folder, so
// whatever it writes there (its environment, caches, downloads, state) lands in the Default's folders.
QString inheritedPackWarning();

enum class PackOrigin {
    BuiltIn, // ships with Fugue; may use the reserved ids and `runtime.kind: "builtin"`
    Disk,
    // The user's group library (5.3): the folder `local` of the first search path, written by
    // Fugue (engine/group_library.h). Its id is `local` and it holds groups only, so nothing
    // in it runs a program.
    Library,
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
    QList<PackSecretSpec> secrets; // the secrets pack.json declares (6.4); empty when the pack did not read
    bool inherited = false;     // from the Default project's folders (1.4.2): read-only here
    bool hidden = false;        // an inherited pack an own pack of the same id and major hides: not loaded, no problem
    QList<PackProblem> disabled; // nodes that did not load, in pack.json's order
};

struct PackReport {
    QList<LoadedPack> packs;   // in search order
    bool hasProblems() const;  // any pack or node disabled (a hidden one is not a problem), or any error (not warning) in a pack
};

// Loads one pack folder into the registry: its declared types, then its nodes, and sets the
// installed version of the pack (Registry::setPackVersion) when the pack is enabled.
LoadedPack loadPack(Registry &registry, const QString &folder, PackOrigin origin = PackOrigin::Disk);

// Loads every pack found under `searchPaths`, each subfolder holding a pack.json. A pack id
// found twice at the same major loads once, from the earlier path; the later one is disabled.
// Different majors of a pack load side by side (6.5). A path that does
// not exist is skipped. The first path's folder `local` is the group library (PackOrigin::Library).
PackReport loadPacks(Registry &registry, const QStringList &searchPaths);
// The same, then `inherited` (the Default's folders): a pack there that an earlier one of the same id and
// major hides is listed as `hidden` and not loaded; the others are listed with `inherited` set.
PackReport loadPacks(Registry &registry, const QStringList &searchPaths, const QStringList &inherited);

// loadPacks(registry, packSearchPaths(), inheritedPackPaths()), the problems also written to stderr, one line each.
// What the window and the CLI call after the built-ins.
PackReport loadInstalledPacks(Registry &registry);

} // namespace fugue
