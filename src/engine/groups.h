#pragma once

// Editing groups (spec 5, 4.4): the document changes behind Make group, Ungroup, Make unique and
// exposing inputs and outputs, as pure functions from one Document to the next. The canvas
// calls them (each is one undo step there); they read no files.
//
// "The level" is the document's graph.nodes: the canvas hands in the level on screen, which is
// the inside of a group definition when the user has entered one (CanvasModel::scopeDocument),
// with every definition of the workflow in `groups`. Instances of a group can sit in the graph
// and inside other definitions, so changes to a group's face (a renamed or dropped exposed
// input or output) are carried to every instance wherever it is: forEachNodes() visits each
// node map.
//
// Errors use the code `group_invalid` (an edit the group cannot take) or `type_mismatch` (one
// exposed input driving inner inputs of different types); the message says what to do.

#include "engine/document.h"
#include "engine/node_type.h"
#include "engine/result.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

namespace fugue {

class Registry;

// The face of a group of this document as a node type, as an instance shows it: id, version,
// name, description and category from the definition; each exposed input typed and constrained
// as its first target (seen through nested groups), narrowed by the group, with its label and
// section (`advanced` for the section "Advanced", collapsed by default, 5.4) and as default the
// group's default, else the inner input's own value, else the inner default (4.4); each output
// typed as its source. An exposed input or output that does not resolve is left out. Empty
// (no id) when the document has no such group.
NodeType groupFace(const Document &document, const Registry &registry, const QString &group);

// Whether `outer` is `inner` or holds an instance of it, directly or through other groups of
// the document. Adding an instance of `outer` inside `inner` would then be recursion (5.2).
bool groupContains(const Document &document, const QString &outer, const QString &inner);

// Calls `visit` with graph.nodes and then the nodes of each definition, so an edit can reach
// every instance of a group.
void forEachNodes(Document &document, const std::function<void(Nodes &)> &visit);

// A group id not used by the document or the registry: local/<base>, else local/<base>-2, ...
QString uniqueGroupId(const Document &document, const Registry &registry, const QString &base);

// The nodes of the level among `nodes` whose type is a sink (5.5).
QStringList sinksAmong(const Document &document, const Registry &registry, const QStringList &nodes);

struct GroupMade {
    Document document;
    QString instance; // the new instance in the level
    QString group;    // its group id
};

// Make group (5.2): the nodes become the inside of a new group local/group (or -2, ...), named
// "Group", version 1.0.0, and one instance takes their place at the top left of their positions.
// Every link that crosses the boundary becomes part of the face: the links from one outside
// output into inner inputs of one type become one exposed input driving them all (the instance
// carries the link); every inner output with a link to the outside becomes an exposed output
// and those links read from the instance. A list input fed from outside becomes an exposed list
// input; one fed from both sides cannot be split (group_invalid). Inner positions and reroute
// points move into layout.groups.<id>, relative to the group's top left; the nodes leave their
// Frames.
Result<GroupMade> makeGroup(const Document &document, const QStringList &nodes, const Registry &registry);

struct Ungrouped {
    Document document;
    QStringList nodes;             // the inner nodes in the level, by their new ids
    QHash<QString, QString> ids;   // inner id -> its id in the level
};

// Ungroup (5.2): the instance's inner nodes come back into the level, keeping their ids where
// the level has none of them, with their links and values. Each exposed input's value on the
// instance (else the group's default) goes to every input it drives; links from the instance's
// outputs come from the inner node instead. A muted instance's nodes come back muted. The
// definition is dropped when nothing else uses it.
Result<Ungrouped> ungroup(const Document &document, const QString &instance);

// Make unique (5.3): the instance gets a copy of its definition under a new id
// (local/<name>-2, ...), so editing it leaves the other instances as they are.
Result<GroupMade> makeUnique(const Document &document, const QString &instance, const Registry &registry);

// Exposes an inner input of the group (5.4) as a new exposed input named after it, or, with
// `into`, adds it as one more target of that exposed input (one-to-many; the types must match).
// The name it is exposed under in `exposedName`.
Result<Document> exposeInput(const Document &document, const QString &group, const PortRef &target,
                             const Registry &registry, const QString &into = {}, QString *exposedName = nullptr);
// Stops driving the inner input; an exposed input left with no target goes, and every instance
// drops its value.
Result<Document> unexposeInput(const Document &document, const QString &group, const PortRef &target);
// Exposes an inner output, named after it.
Result<Document> exposeOutput(const Document &document, const QString &group, const PortRef &from,
                              QString *exposedName = nullptr);
// Drops an exposed output; links from it on every instance go.
Result<Document> unexposeOutput(const Document &document, const QString &group, const QString &name);

// What the interface editor changes on an exposed input (5.4). A new name renames the input on
// every instance too; it must match [a-z][a-z0-9_]* and be free.
struct ExposedChange {
    QString name;
    QString label;
    QString section;
    std::optional<double> min, max;
    // The group's default (4.4): a literal checked against the narrowed input, or none.
    QJsonValue defaultValue = QJsonValue::Undefined;
    // The choices it narrows a choice input to, a subset of the inner one's, or none.
    std::optional<QStringList> choices;
};
// Sets an exposed input's name, label, section, range, default and choices (an empty name keeps
// it; none removes the group's narrowing or default). group_invalid for an unknown group or
// input, a name taken or not valid, a minimum above the maximum, or choices the inner input does
// not have; a default that does not fit is the literal's own error (value_out_of_range, ...).
Result<Document> editExposedInput(const Document &document, const QString &group, const QString &name,
                                  const ExposedChange &change, const Registry &registry);
// Renames an exposed output, and every link that reads it from an instance. group_invalid for an
// unknown group or output, or a name taken or not valid; the same name changes nothing.
Result<Document> renameExposedOutput(const Document &document, const QString &group, const QString &name,
                                     const QString &newName);
// The group under a new id (not used yet): the definition, every instance and its layout.
Document renameGroupId(const Document &document, const QString &from, const QString &to);

// Renames a group (its display name); its id follows (local/<name>, 5.3) when that is free,
// on every instance and in the layout. The id it has after in `newId`.
Result<Document> renameGroup(const Document &document, const QString &group, const QString &name,
                             const Registry &registry, QString *newId = nullptr);

// Moves an exposed input (or output) to position `to` of its list.
Result<Document> moveExposed(const Document &document, const QString &group, bool output, const QString &name, int to);

// After the inside of a group changed: exposed targets and outputs that name an inner node or
// port that is gone are dropped, and so are the instance values of exposed inputs left with no
// target (and links from outputs that went). True when anything changed.
bool pruneGroup(Document &document, const QString &group, const Registry &registry);

// The exposed inputs and outputs of `group` with these names are gone: every instance drops their
// values and the links read from them, and an outer group's face that led to them loses that
// target or output, outwards through every level.
void dropExposedPorts(Document &document, const QString &group, const QSet<QString> &inputs, const QSet<QString> &outputs);

// Drops those of the candidate definitions that no node of the graph uses, directly or through
// other groups, with their layout.groups entries, and then the definitions only they used.
// Other unused definitions stay (a file may carry them on purpose).
void dropUnusedGroups(Document &document, QStringList candidates);

} // namespace fugue
