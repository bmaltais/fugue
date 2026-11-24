#pragma once

// Which inputs of a node apply with its current values (`shown_when`, spec 6.1, #429), read from a
// graph. The canvas hides the rows that do not, and the CLI says "not used" for them; both ask
// here, so they cannot disagree. A hidden input keeps its value and the engine still gets it:
// nothing here changes what runs.

#include "engine/document.h"
#include "engine/node_type.h"
#include "engine/registry.h"

#include <QList>
#include <QMap>
#include <QString>

#include <optional>

namespace fugue {

struct UnusedInput {
    QString input;
    QString why; // "Applies only when Fill is edge-blur."
};

// The inputs of `node` whose condition does not hold. `specs` are the inputs of the node's type, or of a group
// instance's face. An input with a link is never listed (a wire never ends at a hidden row).
// `nodes` is the graph the node's links lead into.
QList<UnusedInput> unusedInputs(const Nodes &nodes, const Document &document, const Registry &registry, const Node &node,
                                const QList<InputSpec> &specs);

// The same for every node of the workflow's graph (group instances by their faces), by node id; a node
// with nothing unused is left out. What `fugue validate` and `fugue workflow describe` say.
QMap<QString, QList<UnusedInput>> unusedInputs(const Document &document, const Registry &registry);

// Whether the value on `source` carries `property`, from the declaration of the output's type
// (`carries`, `carries_when`). Nothing when it cannot be known statically: a node that is not
// there, a type that is not installed, a group instance, a bypassed node.
std::optional<bool> sourceCarries(const Nodes &nodes, const Document &document, const Registry &registry,
                                  const PortRef &source, const QString &property);

} // namespace fugue
