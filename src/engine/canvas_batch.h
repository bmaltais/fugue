#pragma once

// Run a canvas action over a folder of pictures (#250, Photoshop's Automate > Batch). Each picture becomes a canvas of
// the session's store (import), the action's workflow is bound to that canvas and run, and the head the run left is
// exported to the output folder. The pieces are small and composed, so the command line (`fugue canvas batch`), the
// window's Batch dialog (canvas/canvas_batch.h) and a test use the same ones:
//   - batchPictures(): the pictures of a folder, in name order;
//   - batchOutputPath(): the output file for an input, never one that exists or that an earlier picture took;
//   - loadBatchAction(): an action by file or id, parsed and checked against the graph;
//   - batchPrepare(): import one picture, bind the action to it (the whole canvas is the selection);
//   - batchFinish(): export the canvas's head, then drop the temporary canvas (unless asked to keep it);
//   - runBatch(): the loop over all of them, given a `runner` that runs one bound workflow and waits for it. The
//     command line's runner starts a detached worker (cli/canvas_batch.cpp); a window drives prepare and finish itself
//     from its own run service, so it never blocks.
// A picture that fails (unreadable, the action failed, a name that could not be written) is reported and the next one
// goes on. Cancel (the hook `cancelled`, or a runner answering "cancelled") stops after the picture under way; the
// pictures not started are reported as cancelled. Nothing is ever written over a file.
//
// Events (JSON objects, the hook `event`): {"event": "batch.started", "total"}, {"event": "item.started", "index",
// "total", "file"}, {"event": "item.done", "index", "total", "file", "status": "ok"|"failed"|"cancelled", "output"?,
// "code"?, "error"?}, {"event": "batch.done", "ok", "failed", "cancelled"}.
//
// The selection: an action that needs one (`required` or `optional`) gets the whole canvas, as Photoshop's batch acts on
// the whole picture. An action of selection `bounds` (Expand: it needs a crop frame) cannot be batched and is refused.

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

#include "engine/canvas_action.h"
#include "engine/canvas_export.h"
#include "engine/canvas_store.h"
#include "engine/document.h"
#include "engine/result.h"

namespace fugue {
class Registry;
}

namespace fugue::canvas {

// An action found and checked: the workflow, its block, and what the check found in the graph.
struct BatchAction {
    QString id; // the file's base name
    QString file;
    Document document;
    Action action;
    ActionGraph graph;
};

// `ref` is a file (an existing path), else the id of an action in one of `roots` (`<root>/<id>.fugue`; a root may be a
// resource folder such as ":/fugue/actions"). `action_invalid` when the block or the graph is wrong, or the action cannot
// be batched; `file_not_found` when `ref` is neither.
Result<BatchAction> loadBatchAction(const QString &ref, const QStringList &roots, const Registry &registry);

struct BatchSpec {
    BatchAction action;
    QString inFolder;
    QString outFolder;
    PictureExport picture;   // format and size of what is written (png by default)
    QString prompt;          // for an action with a prompt input
    std::optional<qint64> seed; // a fixed seed for every picture; none: a random one each
    bool keepCanvases = false;  // keep each picture's canvas in the store (with the result as its head) instead of removing it
};

// The pictures a canvas can be made from (.png, .jpg, .jpeg, .webp, .bmp, .tif, .tiff, .ora) directly in `folder`, by
// name. A folder that does not exist is `file_not_found`.
Result<QStringList> batchPictures(const QString &folder);

// `<out>/<base>.<ext>`, else `<base>-2.<ext>`, `-3`, ... : the first that is not a file and not in `taken`. The caller adds
// the one it uses to `taken` (the names a batch has used, so two inputs that differ only in extension do not meet).
QString batchOutputPath(const QString &outFolder, const QString &base, const QString &extension, const QSet<QString> &taken);

// What a picture needs checking before any is run: a prompt the action needs, a selection it cannot get.
Errors checkBatch(const BatchSpec &spec);

// One picture imported and its action bound.
struct BatchPrepared {
    QString canvas;
    QString revision; // the head it was bound at
    int width = 0;
    int height = 0;
    Document bound;   // the run's copy of the workflow
};

Result<BatchPrepared> batchPrepare(const BatchSpec &spec, const QString &file, CanvasStore &store);
// Writes the canvas's head into the output folder; the output path. Removes the temporary canvas unless `keepCanvases`.
Result<QString> batchFinish(const BatchSpec &spec, const BatchPrepared &prepared, const QString &file, QSet<QString> &taken, CanvasStore &store);
// Removes the temporary canvas of a picture that failed (unless `keepCanvases`).
void batchDiscard(const BatchSpec &spec, const BatchPrepared &prepared, CanvasStore &store);

struct BatchRunResult {
    QString status; // "ok", "failed" or "cancelled"
    QString code;
    QString message;
};
// Runs one bound workflow and waits until it ends. `workflow` is the action's file.
using BatchRunner = std::function<BatchRunResult(const Document &bound, const QString &workflow)>;

struct BatchHooks {
    std::function<void(const QJsonObject &)> event; // may be empty
    std::function<bool()> cancelled;                 // may be empty
};

struct BatchItem {
    QString file;
    QString status; // "ok", "failed" or "cancelled"
    QString output; // the file written, for "ok"
    QString code;
    QString error;
    QJsonObject toJson() const;
};

struct BatchReport {
    QList<BatchItem> items;
    QString outFolder;
    int ok() const;
    int failed() const;
    int cancelled() const;
    QJsonObject toJson() const;
};

// The whole batch. An error (and no report) when nothing could start: the folder is missing or holds no pictures, the
// output folder cannot be made, the spec does not fit the action.
Result<BatchReport> runBatch(const BatchSpec &spec, const BatchRunner &runner, const BatchHooks &hooks = {});

} // namespace fugue::canvas
