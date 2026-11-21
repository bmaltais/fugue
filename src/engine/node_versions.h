#pragma once

// Moving a workflow node to a newer major version of its type (spec 3.6, #90, #118). Several
// majors of a type are installed side by side and a node runs on the major it recorded; this is
// what the window needs to offer the newer one: whether there is one, what it changes in the
// node's inputs and outputs (shown before anything moves), and the move itself, which only
// rewrites the node's `version`, as an agent does by editing the file.

#include <QString>
#include <QStringList>

#include <optional>

#include "engine/node_type.h"

namespace fugue {

class Registry;
struct Node;

// The inputs and outputs, by name, that `after` adds, drops or retypes against `before`
// (3.6: a minor adds, a major renames, removes or retypes).
struct PortChanges {
    QStringList addedInputs, removedInputs, addedOutputs, removedOutputs;
    bool any() const {
        return !addedInputs.isEmpty() || !removedInputs.isEmpty() || !addedOutputs.isEmpty() || !removedOutputs.isEmpty();
    }
};
PortChanges portChanges(const NodeType &before, const NodeType &after);

// The newest installed version of the node's type when it is a newer major than the one the
// node runs on, and the major it recorded is installed (so it runs, and is only offered the
// newer one); nothing otherwise, or when the type is not installed. A group instance is the caller's
// to leave out: groups update from the library (5.3).
std::optional<Version> newerMajorFor(const Registry &registry, const Node &node);

} // namespace fugue
