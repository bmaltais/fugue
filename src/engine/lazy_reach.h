#pragma once

// Which nodes of a compiled graph may not run because only lazy inputs reach them (spec 3.4, 9.2). The scheduler
// decides at run time; this is the same rule read from the graph alone, for what looks ahead of a run (paired
// placement): sinks and nodes nothing reads are roots, a node is certain when a certain node reads it through an
// input that is not lazy, and the others are conditional.

#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QString>

#include "engine/compiler.h"
#include "engine/document.h"
#include "engine/registry.h"

namespace fugue {

QSet<QString> conditionalNodes(const CompiledGraph &graph, const Registry &registry);

// The hashes of the handles anywhere inside `value` (a derived handle nests its parts).
void collectHashes(const QJsonValue &value, QSet<QString> &hashes);

// The hashes of the models that only conditional nodes hold, as the run would compile `document` (groups expanded, muted nodes out).
// What readiness asks for leaves these out, so a model missing on a side a Switch may not take stops nothing: if that side is chosen the
// node fails with its own model_missing error (3.4). Empty when the document does not compile. A model a node certain to run also
// holds, or one no compiled node holds, is not in it.
QSet<QString> conditionalModelHashes(const Document &document, const Registry &registry);

// Document::models() without those: the models a run is refused on, which Get ready fetches and the Needs panel and `app run` ask for.
QJsonArray modelsToHave(const Document &document, const Registry &registry);

} // namespace fugue
