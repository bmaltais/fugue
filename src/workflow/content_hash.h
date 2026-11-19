#pragma once

// "Unsaved" means the content differs from what was last saved (spec 11.2), not that an edit
// happened. This is the one function that says what "the content" is, so the undo stack (#17)
// can replace it with its own hash without touching the callers.
//
// What counts: the graph, the groups and the layout. What does not: `requires` (written on
// save from the graph), `run` (only in outputs) and `layout.view` (panning and zooming never
// make a workflow unsaved).

#include "engine/document.h"

#include <QString>

namespace fugue {

QString contentHash(const Document &document);

} // namespace fugue
