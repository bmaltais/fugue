#pragma once

// Canvas actions (#231, docs/canvas-actions-design.md 1): a workflow with Canvas Region, Canvas Write and the model nodes
// in it, plus a top-level `action` block naming the inputs the canvas fills in. Pure: a document and a registry in,
// a parsed block, its check against the graph and the bound copy of the workflow out; no Qt GUI, no store, no run.
//
// The block (design 1.2):
//   "action": {"format": 1, "name": "Generative fill", "description": "...", "glyph": "sparkles",
//              "selection": "required" | "optional" | "bounds", "region": "<Canvas Region id>",
//              "inputs": [{"role": "prompt", "targets": ["encode.prompt"], "label", "section", "default", "min", "max",
//                          "choices", "optional", "fallback"}, {"name": "steps", "targets": ["sample.steps"]}, ...]}
// It is a top-level key the document keeps as `extra` on a round trip (spec 4.1) and it is outside sourceHash(), so it
// changes no cache key. A role is one of prompt, negative_prompt, strength, seed, model, layer; an input with no role
// is a plain one with a `name`. `fallback` (research G8) is the prompt a run uses when the person leaves it empty.
//
// The check (design 1.3): the Region that takes the selection is `region`, or the one Canvas Region; every Canvas Write
// takes its canvas from a Region of the shown canvas (the selection's, or one with a blank canvas); every target is a
// top-level node's input (or a group instance's exposed input) of the role's type, not linked, all targets of one input
// of one type. Fugue never guesses a binding when it runs an action.
//
// The binding (design 1.3, 5): the selection's Region gets the canvas, the full hash of the head that was shown, the
// rectangle (or a stored selection's hash and its bounds), and the router's padding and scale; a Region with a blank
// canvas gets the same canvas and revision; each bound input's value goes into its targets. The file is never changed.
//
// Errors: `action_invalid` for a block that is not well formed and for a graph it does not fit (the message says what
// to fix, in words); `unsupported_format_version` for a block of a later format ("Needs a newer Fugue").

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

#include "engine/canvas_router.h"
#include "engine/document.h"
#include "engine/result.h"

namespace fugue {
class Registry;
}

namespace fugue::canvas {

inline constexpr int actionFormat = 1;
inline constexpr char regionType[] = "fugue/canvas-region";
inline constexpr char writeType[] = "fugue/canvas-write";

struct ActionInput {
    QString role;  // one of actionRoles(), or empty for a plain input
    QString name;  // a plain input's name; empty for a role's
    QList<PortRef> targets;
    QString label;
    QString section; // "Advanced" shows only in the bar's advanced state
    QJsonValue defaultValue = QJsonValue::Undefined;
    std::optional<double> min;
    std::optional<double> max;
    std::optional<QStringList> choices;
    bool optional = false; // the prompt may be empty
    QString fallback;      // the prompt used when it is left empty (G8)

    // How the bar and a binding name it: the role, else the name.
    QString key() const { return role.isEmpty() ? name : role; }
};

struct Action {
    int format = actionFormat;
    QString name;
    QString description;
    QString glyph;
    QString selection = QStringLiteral("required");
    QString region; // as written: empty means the workflow's one Canvas Region
    QList<ActionInput> inputs;

    const ActionInput *input(const QString &key) const;
    // An empty prompt cannot be sent: the action has a prompt role that is not optional and has no fallback.
    bool needsPrompt() const;
};

// prompt, negative_prompt, strength, seed, model, layer.
const QStringList &actionRoles();

// The block alone.
Result<Action> parseAction(const QJsonValue &block);
// The document's block: nothing when it has none.
Result<std::optional<Action>> actionOf(const Document &document);

// What the check found in the graph.
struct ActionGraph {
    QString region;      // the Canvas Region that takes the selection
    QStringList regions; // every top-level Canvas Region
    QStringList writes;  // every top-level Canvas Write
};

Result<ActionGraph> checkAction(const Action &action, const Document &document, const Registry &registry);

struct ActionBinding {
    QString canvas;   // the shown canvas
    QString revision; // the full hash of the head that was shown
    // The rectangle selection; with `selection` set, the stored selection's bounds.
    std::optional<RouteRect> rect;
    QString selection; // a stored selection's hash (sha256:...), or empty for a rectangle
    std::optional<int> padding;  // the router's, when it gave one
    std::optional<double> scale;
    QMap<QString, QJsonValue> values; // by ActionInput::key(): the bar's values (prompt, seed, ...)
};

// The run's copy of the workflow: `document` with the binding put in. An input with no value keeps its block default,
// else the workflow's own value. An empty prompt takes its fallback.
Result<Document> bindAction(const Document &document, const Action &action, const ActionGraph &graph, const ActionBinding &binding);

} // namespace fugue::canvas
