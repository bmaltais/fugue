#pragma once

// Pinning a canvas revision at compile time (#209, epic #206, spec 14.5). A Canvas Region reads the
// canvas at a named revision, by default `head`; `head` moves, so a compile replaces it with the
// revision's full hash, once, before anything runs. The hash is then an ordinary literal input: it is
// in the node's cache key and in compiled.json, so the same revision gives the same result, a re-run
// of that compiled graph is a cache hit, and a run can be replayed. A later compile sees the head as
// it has become (after a Canvas Write moved it), and that is a different key.
//
// Qt Quick-free and store-free: what turns a canvas id and a name into a hash comes in as a
// CanvasRevisionResolver, so a test passes a fake and the app passes the session's store.

#include <QString>

#include <functional>

#include "engine/compiler.h"
#include "engine/result.h"

namespace fugue {

// The full revision hash that `ref` ("head", a hash, or the start of one) names in the canvas.
using CanvasRevisionResolver = std::function<Result<QString>(const QString &canvas, const QString &ref)>;

// The resolver of the session's canvas store (canvas/canvas_store.h).
CanvasRevisionResolver sessionCanvasResolver();

inline constexpr char canvasRegionType[] = "fugue/canvas-region";

// Replaces, in every node of `graph` that reads a canvas (Canvas Region), the `revision` input by
// the full hash it names, "head" when the input is not set. A node whose `canvas` or `revision` is
// not a literal (a link) is left as it is: its kernel refuses a revision that is not a full hash.
// Returns what could not be resolved (canvas_not_found, canvas_ambiguous), each on its node.
Errors resolveCanvasRevisions(CompiledGraph &graph, const CanvasRevisionResolver &resolve);

} // namespace fugue
