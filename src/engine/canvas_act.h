#pragma once

// Running a canvas action (#231, docs/canvas-actions-design.md): what the window's task bar (canvas/canvas_actions.h) and
// `fugue canvas act` (cli/canvas_act.cpp) share, so a script runs an action the way the bar's Generate does. No Qt GUI, no
// queue, no run: a request in, the bound copies of the workflow out.
//   - collectActions(): the listing. Workflows from sources (templates of the library, files of the roots) that carry a valid
//     `action` block are actions; the first source of an id wins; a block or graph that does not fit lists the action as
//     broken, a pack its nodes need that is not installed is listed with the offer, not as broken;
//   - notRunnable(): why an entry cannot run now (broken, pack not installed);
//   - planAct(): everything Generate does before it queues: the rectangle held in the canvas, Expand's growth of the canvas
//     (one revision, with the new area pre-filled), the router's padding and scale, the layer role's layer, the prompt, the
//     seeds of Count N (variations 2 to N hidden, "(n of N)" after the layer's name), and the bound copy of the workflow of
//     each run.
// An Expand that ends cancelled or failed has its growth taken back (takeBackGrowth).
// Errors are coded: `mask_empty`, `canvas_head_moved`, `action_invalid`, `pack_missing`, `model_missing` (from the caller's
// check) and `act_refused` for the rest (a word the person fixes: select an area, type a prompt, pick a side).

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <optional>

#include "engine/canvas_action.h"
#include "engine/document.h"
#include "engine/result.h"
#include "engine/templates.h"

namespace fugue {
class Registry;
}

namespace fugue::canvas {

// One action found: its workflow, its block and what the check found in the graph.
struct ActionEntry {
    QString id;
    QString file;
    bool library = false; // from the template library, else compiled into Fugue
    Document document;
    std::optional<Action> action;
    ActionGraph graph;
    QString broken; // the reason it cannot run, in words
    QList<CarriedPack> carried; // the packs its template folder holds
    // The packs its nodes need that are not installed: {id, name, source, ref}. The action is not broken for that: the bar
    // offers to install them, and the inputs of the block that point into their nodes wait for them.
    QVariantList needPacks;
};

struct ActionSource {
    QString id;
    QString file;
    bool library = false;
    QList<CarriedPack> carried;
};

// The actions of `sources`, in order, the first of an id winning (a plain workflow with no block is not an action and never
// hides one of the same id).
QList<ActionEntry> collectActions(const QList<ActionSource> &sources, const Registry &registry);

// The packs `document` needs that this Fugue lacks, each with where to get it. {id, name, source, ref}.
QVariantList packNeeds(const Document &document, const QList<CarriedPack> &carried, const Registry &registry);

// Why `entry` cannot run now: `action_invalid` (broken), `pack_missing` (the message names the pack). Nothing when it can.
std::optional<Error> notRunnable(const ActionEntry &entry);

// What the bar has a field for stays on the bar (the prompt, the seed, the layer); every other input is Advanced.
bool inPopover(const ActionInput &input);

// The action's workflow with `values` (by input key) put in the targets of each Advanced input, else the block's default (else
// the file's own value stays): the workflow as a run would have it, for checking which models it needs. A choice input's word is
// left alone (its number is bound at the run).
Document withValues(const ActionEntry &entry, const QJsonObject &values);

// The errors of 8.5: nothing can serve the node now (no model that can run it, not here, not running).
bool cannotRunNow(const QString &code);
// A run's error in plain words, for the action named `actionName` (design 4).
QString plainWords(const Error &error, const QString &actionName);

// A fully cached Canvas Write lands nothing (design decision 5): its revision placed on the head again. `moved`: it was placed;
// else it is still on the head (`layer` is its newest layer's name: same prompt, seed and selection, nothing new can come).
struct PlacedAgain {
    bool moved = false;
    QString layer;
};
Result<PlacedAgain> placeWriteAgain(const QString &canvas, const QString &revision);

// The size of a canvas at a revision, and that revision in full ("" or "head": the head).
struct Shown {
    QString revision;
    int width = 0, height = 0;
};
Result<Shown> shownOf(const QString &canvasId, const QString &ref);

// How far an Expand grows the canvas (`expand`: {side, percent, pixels}), and the grown bounds in the old canvas's pixels.
// An Error with `act_refused` for a side that is not left, right, top or bottom.
Result<RouteRect> expandedBounds(const QVariantMap &expand, int width, int height);

struct NodeSet {
    QString node, input;
    QJsonValue value;
};

struct ActRequest {
    QString canvas;
    QString revision;                     // the head that was shown, in full; empty: the canvas's head
    QString prompt;
    std::optional<RouteRect> rect;        // the selection's rectangle, in canvas pixels
    QString selection;                    // a stored selection's hash
    QString layer;                        // the one layer Canvas Region reads instead of the whole picture
    QString currentLayer;                 // the layer a layer-role action works on
    QVariantMap expand;                   // {side, percent | pixels}: for an action on the canvas bounds
    QJsonObject values;                   // by input key: the Advanced values the person set (a choice input's word)
    QList<NodeSet> nodeSets;              // literals put straight into the copy's nodes after the binding
    std::optional<qint64> seed;           // fixed (s, s+1, ...); none: random each
    int count = 1;                        // 1 to maxVariations; 1 for an action with no seed role
    // Called after every check that needs no change and before the canvas is grown: the caller's own refusal (the models are not
    // on this machine). Nothing when it passes.
    std::function<std::optional<Error>()> beforeChange;
};

struct ActPlan {
    QString canvas;
    QString revision;                     // the head the runs read (an Expand's grown canvas)
    int width = 0, height = 0;            // that canvas's size
    std::optional<RouteRect> rect;        // the selection the Region got
    bool grew = false;                    // Expand made a revision first
    QString growRevision;
    QList<Document> runs;                 // one bound copy per run, in order
};

inline constexpr int maxActVariations = 4;

Result<ActPlan> planAct(const ActionEntry &entry, const ActRequest &request);

// Expand grew the canvas before its runs; a run that was cancelled or failed leaves the empty strip (#310, design: Expand). Takes
// the growth back when nothing else has changed the canvas since: the head is still `plan.growRevision`, so it moves back to the
// revision before it, as the bar's Undo does (the growth revision stays in the history, and Redo can bring it back). Nothing when
// the plan did not grow, or the head moved meanwhile (a Count N variation landed, a person edited): that canvas is left alone.
// True when the head was moved back.
bool takeBackGrowth(const ActPlan &plan);
// The line that says so: "The canvas is back to its size: Expand was cancelled." / "... did not finish."
QString growthTakenBack(const QString &actionName, bool cancelled);

} // namespace fugue::canvas
