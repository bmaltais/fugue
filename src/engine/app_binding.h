#pragma once

// Fugue Apps (#384, epic #397, spec 4.9): a workflow shown as one simple screen. The graph stays the truth; the app is a
// list of marked inputs and outputs kept in a top-level `app` block of workflow.fugue. No node type, no second file format.
//
//   "app": {"format": 1, "title": "Remove background", "description": "...", "defaultView": "app" | "graph",
//           "inputs":  [{"role": "image", "targets": ["load.file"], "label": "Picture"}, {"role": "seed", ...}, ...],
//           "outputs": [{"node": "save", "label": "Result"}]}
//
// `inputs` is the list an `action` block has (engine/input_binding.h: role, name, targets, label, section, default, min,
// max, choices, choice_values), in the order a form shows it; an app is an action with no canvas Region. A role is one of
// prompt, negative_prompt, strength, seed, model, layer, image, mask; a role with a `name` is keyed by the name, so two
// pictures are `{"role": "image", "name": "style"}` and `{"role": "image", "name": "subject"}`. `outputs` name sinks (a Save
// or Preview node) by id. Like `action`, the block is a top-level key the document keeps as `extra` (spec 4.1), written by
// an older Fugue back unchanged, and outside sourceHash(): it changes no cache key.
//
// Value in, result out: bindApp puts a form's values into a copy of the workflow; the run's results of the output nodes
// are the app's results (run history and the results store are the existing ones; the app owns none).
//
// Deleting a node: pruneApp drops the targets and outputs whose node or input is gone, as 5.4 does for exposed ports, and
// reports each as a warning `app_stale`, so a later issue can say it once ("This app lost its Prompt: the node was deleted").
// checkApp reports what is still wrong (a linked or ill-typed target, an output that is not a sink, no output at all) as
// `app_invalid`. Errors: `app_invalid` for a block not well formed; `unsupported_format_version` for a later format.

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

#include "engine/document.h"
#include "engine/input_binding.h"
#include "engine/result.h"

namespace fugue {

class Registry;

namespace app {

inline constexpr int appFormat = 1;

struct AppOutput {
    QString node;  // a sink's id
    QString label; // what the result is called; empty: the node's own name
};

struct App {
    int format = appFormat;
    QString title;
    QString description;
    QString defaultView = QStringLiteral("app"); // "app" or "graph": what opening shows first
    QList<InputBinding> inputs;                  // in the order a form shows them
    QList<AppOutput> outputs;

    const InputBinding *input(const QString &key) const;
    QStringList outputNodes() const;
};

// The block alone.
Result<App> parseApp(const QJsonValue &block);
// The document's block: nothing when it has none.
Result<std::optional<App>> appOf(const Document &document);

// The block as it is written.
QJsonObject toJson(const App &app);
// Puts the block in the document's extra (it is saved, shared and kept by older Fugue with the document).
void setApp(Document &document, const App &app);
void clearApp(Document &document);

// Drops what the document no longer has (targets, inputs left with none, outputs) and returns a warning `app_stale` for each.
Errors pruneApp(App &app, const Document &document, const Registry &registry);

// What does not fit the graph, as errors `app_invalid` in words.
Errors checkApp(const App &app, const Document &document, const Registry &registry);

// The run's copy of the workflow with the values in (by InputBinding::key()); an input with no value keeps its block
// default, else the workflow's own. The file is never changed.
Result<Document> bindApp(const Document &document, const App &app, const QMap<QString, QJsonValue> &values);

} // namespace app
} // namespace fugue
