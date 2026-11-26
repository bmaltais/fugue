#pragma once

// Which nodes of a compiled graph may not run because only lazy inputs reach them (spec 3.4, 9.2). The scheduler
// decides at run time; this is the same rule read from the graph alone, for what looks ahead of a run (paired
// placement): sinks and nodes nothing reads are roots, a node is certain when a certain node reads it through an
// input that is not lazy, and the others are conditional.

#include <QSet>
#include <QString>

#include "engine/compiler.h"
#include "engine/registry.h"

namespace fugue {

QSet<QString> conditionalNodes(const CompiledGraph &graph, const Registry &registry);

} // namespace fugue
