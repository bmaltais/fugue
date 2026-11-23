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

// The id of the Active canvas (#246), or empty when there is none to name. Called once per compile.
using ActiveCanvasProvider = std::function<QString()>;

// The Active canvas as a compile names it: `shown` (the canvas last used in a Canvas view) when the session's store
// has it, else the newest canvas of the root, else empty. A headless run (the CLI) passes no `shown`.
QString activeCanvasOrNewest(const QString &shown);

inline constexpr char canvasRegionType[] = "fugue/canvas-region";

// Replaces, in every node of `graph` that reads a canvas (Canvas Region), the `revision` input by
// the full hash it names, "head" when the input is not set. A node whose `canvas` or `revision` is
// not a literal (a link) is left as it is: its kernel refuses a revision that is not a full hash.
// An empty `canvas` (not yet chosen) means the Active canvas (#246): `active` names it, and it is written into the
// node as a literal, so the id is in the cache key and in compiled.json and a replay is exact. With no `active`, or
// none to name (a root with no canvas), it is left empty for the kernel to refuse in words.
// Returns what could not be resolved (canvas_not_found, canvas_ambiguous), each on its node.
Errors resolveCanvasRevisions(CompiledGraph &graph, const CanvasRevisionResolver &resolve, const ActiveCanvasProvider &active = {});

} // namespace fugue
