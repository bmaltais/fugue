#pragma once

// The group library (spec 5.3): groups saved from a workflow into the reserved `local` pack,
// ~/.local/share/fugue/packs/local/<name>/, where each is a node type of runtime.kind "group"
// (6.1) like a pack's. The pack loader reads it as PackOrigin::Library.
//
// A saved group is two files in its folder:
//   - node.json: the manifest, with the group's face as its inputs and outputs (for search and
//     `fugue node describe`) and runtime {"kind": "group", "definition": "group.fugue"};
//   - group.fugue: a workflow document with no nodes whose `groups` holds the group and every
//     group it uses, and whose layout.groups their insides' layout.
// pack.json lists the folders.
//
// A workflow always embeds the definition it uses (5.3), so these files are read only when a
// group is added from the library or updated, never to run one. Updates are explicit: the
// canvas offers one when the library has a newer version than the workflow's definition.

#include "engine/document.h"
#include "engine/node_type.h"
#include "engine/result.h"
#include "engine/version.h"

#include <QString>
#include <QStringList>

namespace fugue {

class Registry;

// packsDir() + "/local".
QString libraryDir();

// The key a group definition carries (4.4, kept as an unknown key by older readers) naming the
// library entry it came from or was saved to, so saving updates only that entry.
inline constexpr char kLibraryKey[] = "library";

// Saves the group of `document` (with the groups it uses, and their layout) to the library
// folder `dir`, as local/<name> where <name> is the part of its id after the '/'. Saving over
// an earlier version bumps it under the rules of 3.6: major when an exposed input or output
// went or changed type, minor when one was added, patch for any other change; saving the same
// definition again keeps the version. The version saved, which the workflow's definition and
// its instances should take, so the workflow is in step with the library.
struct SavedGroup {
    QString id; // local/<name>
    Version version;
    bool changed = true; // false: the library had this very definition
};
Result<SavedGroup> saveToLibrary(const Document &document, const QString &group, const Registry &registry,
                                 const QString &dir = libraryDir());

// A group of the library, as listed by its pack.json (#74).
struct LibraryGroup {
    QString id; // local/<name>
    QString name;
    Version version;
};
QList<LibraryGroup> libraryGroups(const QString &dir = libraryDir());

// Removes a library group: its folder, and its line in pack.json. Workflows that use it keep their
// own copy of its definition (5.3). group_invalid for a group the library does not have.
Errors removeFromLibrary(const QString &id, const QString &dir = libraryDir());

// Renames a library group: its display name, and its id and folder with it (local/<name>, as in a
// workflow). The new id; group_invalid for an unknown group or a name another library group has.
Result<QString> renameInLibrary(const QString &id, const QString &name, const Registry &registry,
                                const QString &dir = libraryDir());

// The definition file of a library group type (runtime.kind "group"): a document whose groups
// hold `type.id` and what it uses. file_not_found or invalid_document when it cannot be read.
Result<Document> loadLibraryGroup(const NodeType &type);

// What updating the workflow's definition of a group to the library's changes (3.6): the
// exposed inputs and outputs that come and go, by name.
struct GroupChanges {
    Version from, to;
    QStringList addedInputs, removedInputs, addedOutputs, removedOutputs;
};
GroupChanges groupChanges(const Document &workflow, const Document &library, const QString &group,
                          const Registry &registry);

// The workflow's definitions of `group` and of every group it uses, replaced by the library's
// (a group the workflow has under the same id but the library does not use is left alone).
// Every instance takes the new version and drops the values and links of exposed inputs and
// outputs the new definition lacks. Nested groups the library brings that clash with another
// definition the workflow holds under their id come in under a new id.
Document updateGroup(const Document &workflow, const Document &library, const QString &group, const Registry &registry);

// Puts the definition of every library or pack group (runtime.kind "group", 6.1) the document
// uses but does not hold into its groups, with the groups they use, as adding one from the
// library does; so a file an agent or the CLI wrote with only the type id runs (5.3). What
// cannot be read stays out (compile then says unknown_node_type). The ids embedded.
QStringList embedGroupTypes(Document &document, const Registry &registry);

} // namespace fugue
