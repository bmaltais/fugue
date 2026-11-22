#pragma once

// The `fugue/canvas` handle (#208, epic #206, spec 14.5): what flows between nodes for a canvas, in
// the pattern of `fugue/stitch`. A descriptor, never pixels: the canvas id and one revision's hash.
// It carries no path, so a workflow, a run folder and a share package stay portable; the store
// (engine/canvas_store.h) of the session resolves the pair. Putting the revision in the descriptor is
// what puts it into a cache key: a different revision is a different value.
//
// As a data value (2.6) it is `{"data": "fugue/canvas", "path": <file>, "meta": {"canvas": id,
// "revision": hash}}` where `path` is a tiny `canvas.json` in the node's work folder holding the same
// meta, canonical JSON, so the same descriptor is the same bytes. A node slice builds on these
// functions; there are no nodes yet.

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include "engine/result.h"

namespace fugue::canvas {

inline constexpr char canvasHandleType[] = "fugue/canvas";

struct CanvasHandle {
    QString canvas;   // a canvas id (isCanvasId)
    QString revision; // sha256:...
    bool operator==(const CanvasHandle &o) const { return canvas == o.canvas && revision == o.revision; }
};

// {"canvas", "revision"}.
QJsonObject canvasHandleMeta(const CanvasHandle &handle);
// The handle a `meta` object, or a whole `{"data", "path", "meta"}` value, describes; input_invalid
// when it is not one, has other keys, a bad id or hash, or anything that looks like a path.
Result<CanvasHandle> canvasHandleFrom(const QJsonValue &value);
// The canonical bytes of canvas.json.
QByteArray canvasHandleBytes(const CanvasHandle &handle);
// Writes canvas.json into `workDir` and returns the data value to emit.
Result<QJsonObject> writeCanvasHandle(const QString &workDir, const CanvasHandle &handle);

} // namespace fugue::canvas
