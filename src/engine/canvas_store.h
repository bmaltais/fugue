#pragma once

// The canvas revision store (#208, epic #206, spec 14.5): a canvas is a chain of immutable,
// content-hashed revisions, and one of them is the head. Nothing here knows a window or a node; a
// revision is a PaintDocument (engine/paint_document.h) saved as tiles (engine/canvas_tiles.h).
//
// On disk, under one root (`<data>/canvases`, so inside a project's own folder under FUGUE_HOME):
//
//   blobs/<aa>/<hex>.png          a tile's PNG, named by the hash of its raw pixels (shared by every canvas)
//   trees/<hex>.json              a revision's content: size, and per layer its name, kind, visibility,
//                                 opacity, blend, layer mask, origin and tile hashes. Pixels and layer
//                                 properties only; a property at its default is left out, so a tree
//                                 written before it existed keeps its hash.
//   canvases/<id>/canvas.json     {"format", "id", "name", "created"}
//   canvases/<id>/commits/<hex>.json   the commit record: tree hash, parent, producer, time
//   canvases/<id>/head            the head revision's hash, moved only through moveHead()
//
// A *tree hash* is the canonical-JSON hash (engine/canonical.h) of the tree, so the same pixels and
// layer properties give the same tree hash whatever made them, when, or in which canvas: it is what a
// cache key uses. A *revision* is named by the hash of its commit record (git's idea: tree, parent,
// who, when), so two commits of one tree are two revisions. Every object is written once, by a rename
// into place, and never changed; a stored object is checked against its name when it is read.
//
// The head is a ref file. moveHead() is a compare-and-swap: it takes a per-canvas lock, moves the head
// only when it is still what the caller last saw, and says what the head is when it is not (the caller
// rebases or keeps both; the store does neither). A revision that was added but whose move lost stays
// stored, a candidate nobody points at.
//
// A *candidate* is a stored revision that is not in the head's history and that nobody has resolved:
// a Canvas Write that was asked to keep its result for review, or that lost the head's race (#210).
// It is accepted into the head (accept(): the head moves to it when it was made from the head, else
// its added layers are placed on the head as a new revision) or dropped (drop()). Both leave a small
// file `canvases/<id>/resolved/<hex>` ("accepted <revision>" or "dropped") that hides it from the
// list; nothing is deleted, and a dropped one can still be accepted.
//
// Layer edits (#226; engine/canvas_layer_ops.h): editLayers() stores the tree an edit gives as a new
// revision whose parent is the one edited, with the same tiles (nothing is re-encoded), and moves the
// head to it by compare-and-swap; only the head is edited, so an edit never forks the history. Undo
// moves the head back to its parent and marks the revision it left `undone` (resolved/<hex>), which
// keeps it out of the candidates; redo moves the head forward to the newest undone revision whose
// parent is the head. Any head can be undone this way, a Canvas Write's accepted layer included.
//
// Picture layers are placeholders, as in an .ora (engine/paint_file.h): the picture is a node's
// input, never part of a revision.

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "engine/canvas_guides.h"
#include "engine/canvas_layer_source.h"
#include "engine/canvas_tiles.h"
#include "engine/edge_fill.h"
#include "engine/clock.h"
#include "engine/paint_document.h"
#include "engine/result.h"

namespace fugue::canvas {

inline constexpr int storeFormat = 1;
// What treeFromJson accepts (a canvas is at most 8192 x 8192: 1024 tiles a layer).
inline constexpr int maxTreeLayers = 512;
inline constexpr int maxTreeTiles = 100000;

// What one layer of a tree is.
struct LayerRecord {
    QString name;
    mask::LayerKind kind = mask::LayerKind::Paint;
    bool visible = true;
    double opacity = 1;
    TileMap tiles; // empty for a picture layer; absent tiles are 0
    QString compositeOp; // "" is svg:src-over (mask::LayerInfo::compositeOp)
    bool hasMask = false;  // a layer mask: one channel, its own tiles
    bool maskEnabled = true;
    // Locked (#226): its pixels, its layer mask and its place in the stack are protected (applyLayerEdit refuses a delete, a merge, a
    // transform or a mask edit of it); showing, opacity, blend, a name and duplicating are not pixels and stay open.
    bool locked = false;
    TileMap maskTiles;
    QString producer, created; // the layer's origin (mask::LayerInfo)
    // A group (#252): a layer that holds layers, bottom first, and composites them as one (the kind is Paint, `tiles` empty;
    // its own opacity, blend and layer mask apply to the composite). Stored as kind "group" with "children".
    bool group = false;
    std::vector<LayerRecord> children;
    // The pixels this layer was placed or transformed from (#236, engine/canvas_layer_source.h); `tiles` stay what the canvas shows.
    std::optional<LayerSource> source;
    int channels() const { return kind == mask::LayerKind::Mask ? 1 : 4; }
    int tileCount() const; // its own tiles and its children's
    bool operator==(const LayerRecord &o) const {
        return name == o.name && kind == o.kind && visible == o.visible && opacity == o.opacity && tiles == o.tiles && compositeOp == o.compositeOp &&
               hasMask == o.hasMask && maskEnabled == o.maskEnabled && locked == o.locked && maskTiles == o.maskTiles && producer == o.producer && created == o.created &&
               group == o.group && children == o.children && source == o.source;
    }
};

struct Tree {
    int width = 1;
    int height = 1;
    std::vector<LayerRecord> layers; // bottom first
    int tileCount() const;
};

// The commit record, with the hashes it is named by.
struct Revision {
    QString id;       // sha256:... of the commit record
    QString tree;     // the tree hash
    QString parent;   // empty for the first revision
    QString producer; // what made it: "new", "import", later a node or a person's edit
    QString time;     // ISO 8601 UTC with milliseconds
};

struct CanvasInfo {
    QString id;
    QString name;
    QString created;
    QString head; // empty until the first revision
};

// The answer of moveHead and commit.
struct HeadMove {
    bool moved = false;
    QString head;     // the head after the call; when !moved, what it was instead of `expected`
};

struct Commit {
    Revision revision;
    HeadMove move;
};

// A candidate and whether the head is the revision it was made from (so accepting moves the head to it).
struct Candidate {
    Revision revision;
    bool onHead = false;
    bool dropped = false; // dropped candidates are listed only when asked for
};

// The answer of accept.
struct Accepted {
    Revision head;     // the revision the head is on after the call (the candidate itself, or the new revision that placed its layers)
    bool rebased = false;
    bool moved = false; // false: the head moved meanwhile and nothing was changed; ask again
    QString current;   // the head when !moved
    QString note;      // set when the head moved but the acceptance mark could not be written (the layer is placed; do not run again)
};

// A revision with its tree, as a canvas made elsewhere carries it (importCanvas).
struct ImportedRevision {
    Revision revision;
    std::shared_ptr<const Tree> tree; // revisions of one tree share it: it is checked and stored once
};
// The pixels of the tile with this hash, `width` x `height` x `channels` (a package's tile, read and decoded).
using TileFetch = std::function<Result<mask::MaskPaint>(const QString &hash, int width, int height, int channels)>;

struct LayerEdit; // engine/canvas_layer_ops.h

bool isCanvasId(const QString &text); // [a-z0-9][a-z0-9_-]{0,63}

class CanvasStore {
public:
    // `root` is the folder the store lives in (made when something is written). A test passes its own
    // folder and a clock it controls.
    explicit CanvasStore(QString root, Clock clock = {});
    // The store of the session: `<dataDir()>/canvases` (under FUGUE_HOME, the project's own).
    static CanvasStore forSession();

    QString root() const { return m_root; }

    // ---- canvases ----
    // Makes a canvas whose first revision is `document` (producer `producer`), head on it.
    Result<CanvasInfo> create(const QString &name, const mask::PaintDocument &document, const QString &producer);
    // The same, with `edits` (engine/canvas_layer_ops.h: Group and the group's settings) applied to the document's tree before it
    // is stored: ONE first revision whose layers are the edited tree (a file with groups, #227). A bad edit fails with nothing made.
    Result<CanvasInfo> createWithEdits(const QString &name, const mask::PaintDocument &document, const std::vector<LayerEdit> &edits, const QString &producer);
    Result<CanvasInfo> info(const QString &canvasId) const;
    // ---- the canvas as a whole (the Canvas view's File menu, #232) ----
    // Renames the canvas (input_invalid for a blank name, canvas_not_found for an id this store lacks). Its id, history and
    // head do not change.
    Result<CanvasInfo> rename(const QString &canvasId, const QString &name);
    // A copy of the canvas with its whole history, head and resolutions, under a new id and `name` (a blank name is "<name> copy").
    // The tiles are shared (the blobs are the store's, by content).
    Result<CanvasInfo> duplicate(const QString &canvasId, const QString &name);
    // Deletes the canvas for good: its revisions, head and candidates. The tiles in blobs/ stay (nothing in the store is
    // deleted by content yet). canvas_not_found for an id this store lacks.
    Result<QString> remove(const QString &canvasId);
    // Empties the canvas: ONE new revision of the head, which must be `expected`, that has the same size and one empty
    // transparent layer; Undo takes it back. {moved: false, head: <the head>} when the head is not `expected`.
    Result<Commit> clear(const QString &canvasId, const QString &expected, const QString &producer);
    // Grows, shifts or cuts the canvas (#240, engine/canvas_resize.h): ONE new revision of the head, which must be `expected`,
    // whose canvas is the rectangle `to` of the old one (in its pixels; x and y may be negative, and it may reach past any
    // side), layers moved and re-tiled, and, when it grows and `fill` is not Transparent, a new bottom layer holding the new
    // area's pre-fill (`blur` is EdgeBlur's radius). Undo takes it back. {moved: false, head: <the head>} when the head is not
    // `expected`; input_invalid for a size past the canvas limit or a rectangle that holds none of the canvas.
    Result<Commit> resize(const QString &canvasId, const QString &expected, const mask::IRect &to, edgefill::Mode fill, int blur, const QString &producer);
    // The canvas's guides and grid spacing (#253), kept in `canvases/<id>/guides.json` beside canvas.json: NOT a revision (no
    // history step, no Undo; a duplicate copies it). A canvas with no file has no guides and the default grid. canvas_not_found
    // for an id this store lacks, canvas_corrupt for a file that does not validate (engine/canvas_guides.h).
    Result<GuideSet> guides(const QString &canvasId) const;
    Errors setGuides(const QString &canvasId, const GuideSet &set);
    // Every canvas of the store, newest first (by creation time, then id); one that cannot be read is left out.
    std::vector<CanvasInfo> list() const;

    // ---- revisions ----
    // Stores `document` as a revision of the canvas with this parent (empty for none) and does not
    // move the head. The revision is a candidate until moveHead points at it.
    Result<Revision> addRevision(const QString &canvasId, const mask::PaintDocument &document, const QString &parent, const QString &producer);
    // addRevision with the head's current value as `expected` and parent, then moveHead.
    Result<Commit> commit(const QString &canvasId, const mask::PaintDocument &document, const QString &expectedHead, const QString &producer);
    // Compare-and-swap of the head: to `revision` when it is `expected` now (empty: no head yet).
    Result<HeadMove> moveHead(const QString &canvasId, const QString &expected, const QString &revision);

    // `ref` is "head", a full revision hash, or the start of one (at least 8 hex digits after an
    // optional "sha256:"), one revision only. Returns the full hash.
    Result<QString> resolve(const QString &canvasId, const QString &ref) const;
    Result<Revision> revision(const QString &canvasId, const QString &revisionId) const;
    Result<Tree> tree(const QString &treeHash) const;
    // The revisions from `from` (default: the head) back along the parents, newest first, at most `limit`.
    Result<std::vector<Revision>> log(const QString &canvasId, const QString &from = {}, int limit = 100) const;
    // ---- candidates (#210) ----
    // The unresolved candidates of the canvas, newest first (and the dropped ones too when `withDropped`).
    Result<std::vector<Candidate>> candidates(const QString &canvasId, bool withDropped = false) const;
    // Accepts a candidate. Made from the head: the head moves to it. Made from an older revision: when
    // it only added layers on top of that revision, a new revision of the head with those layers is
    // made and becomes the head (canvas_conflict when it changed anything else). A revision that is in
    // the head's history or already accepted is canvas_not_candidate.
    Result<Accepted> accept(const QString &canvasId, const QString &revisionId);
    // Hides a candidate (canvas_not_candidate for a revision in the head's history or an accepted one).
    // Dropping a dropped one is not an error. Returns the revision.
    Result<Revision> drop(const QString &canvasId, const QString &revisionId);

    // ---- layer edits and undo (#226) ----
    // Applies `edit` to the head, which must be `base` (the revision the caller shows): a new revision
    // whose parent is `base` (producer `producer`), and the head moved to it. When the head is not
    // `base` nothing is written and the answer is {moved: false, head: <the head>}. An edit that
    // changes nothing writes nothing: the answer's revision is `base` and moved is false with head `base`.
    Result<Commit> editLayers(const QString &canvasId, const QString &base, const LayerEdit &edit, const QString &producer);
    // A layer mask painted or added (#230): the layer at `position` of `base` (the head, as editLayers) gets `mask` (one
    // channel, the canvas's size) as its layer mask, turned on. Only the mask's tiles that are not stored yet are
    // written, so a stroke that touched one corner of a large mask adds the tiles of that corner and shares the rest.
    // ONE new revision, producer `producer` (`canvas-view:mask-add`, `canvas-view:mask-paint`); a mask equal to the one
    // the layer has writes nothing (as editLayers). input_invalid for a mask of another size or channels.
    Result<Commit> setLayerMask(const QString &canvasId, const QString &base, int position, const mask::MaskPaint &mask, const QString &producer);

    // A picture placed on the canvas as a new layer (#236; Edit > Paste as new layer, a dropped file): the picture, which may be
    // bigger than the canvas, is kept whole as the layer's original (engine/canvas_layer_source.h) and shown centered on (`cx`, `cy`)
    // canvas pixels, cut at the canvas's edge. The layer goes right above the layer at `above` (a position, bottom first; -1: on
    // top) and is named `name`, or "Pasted N" for the lowest N no layer has. ONE revision of `base` (the head), producer `producer`
    // ("canvas-view:paste"); {moved: false, head: <the head>} when the head is not `base`. input_invalid for a picture that is
    // not four-channel or is past what a canvas holds (placeableProblem), or an `above` that is no layer.
    Result<Commit> pasteLayer(const QString &canvasId, const QString &base, const mask::MaskPaint &picture, double cx, double cy, int above, const QString &name,
                              const QString &producer);

    // A paint layer moved, scaled, turned or flipped (#236, engine/canvas_transform.h): the layer at `position` of `base` (the head, as
    // editLayers) is drawn again from its original by `transform` (its own pixels the first time, which then are its original; a
    // layer painted on since starts again from the layer as it is) and its layer mask goes through the same placement. ONE new
    // revision, producer `producer` ("canvas-view:transform"); a placement equal to the current one writes nothing. input_invalid, in
    // words, for a group, a mask or picture layer, or a placement that cannot be drawn.
    // `seen`, when given, is the placement the caller read the layer at: when the layer is not that any more (a different original, a different
    // placement) nothing is written and the answer is input_invalid ("The layer changed meanwhile").
    Result<Commit> transformLayer(const QString &canvasId, const QString &base, int position, const LayerTransform &transform, const QString &producer,
                                  const SeenPlacement *seen = nullptr);

    // A layer written at the tree level, groups kept (#274; a Canvas Write): `layer` goes on top of the revision `parent`'s layers as a
    // new revision with that parent (producer `producer`), the head not moved: moveHead does that, or accept places it on a newer
    // head. The layer's tiles are ones storeLayerTiles stored (a tile that is not stored is canvas_corrupt on the next read).
    Result<Revision> addLayerRevision(const QString &canvasId, const QString &parent, const LayerRecord &layer, const QString &producer);
    // Cuts a one- or four-channel layer into tiles and stores the ones not stored yet: the tile maps of a LayerRecord that addLayerRevision takes.
    Result<TileMap> storeLayerTiles(const mask::MaskPaint &layer);

    // ---- selections (#230, docs/canvas-actions-design.md 3) ----
    // A selection painted in a Canvas view (Quick Mask) kept by content: its one-channel tiles in blobs/ and
    // `selections/<hex>.json` ({format, width, height, tiles}), named by the canonical hash of that record, so an equal
    // selection is stored once and a Canvas Region's `selection` input (that hash) is a literal a cache key and a replay
    // hold. 255 is selected (the part to change), 0 not.
    Result<QString> storeSelection(const mask::MaskPaint &selection);
    // The stored selection (canvas_not_found for a hash the store has none of, canvas_corrupt for one that is not what it is named for).
    Result<mask::MaskPaint> selection(const QString &hash) const;

    // Moves the head from `expected` to its parent (canvas_no_step for the first revision); {moved: false} when it is not `expected`.
    Result<HeadMove> undo(const QString &canvasId, const QString &expected);
    // The revision redo would move the head to ("" when none): the newest undone one whose parent is the head.
    Result<QString> redoTarget(const QString &canvasId) const;
    // Moves the head from `expected` to redoTarget() (canvas_no_step when there is none).
    Result<HeadMove> redo(const QString &canvasId, const QString &expected);

    // The revision as a document (no history). Picture layers come back as placeholders.
    Result<mask::PaintDocument> load(const QString &canvasId, const QString &revisionId) const;
    // The revision's layer tree, groups kept (load() composites a group into one layer).
    Result<Tree> loadTree(const QString &canvasId, const QString &revisionId) const;

    // The pixels of one stored tile (`width` x `height`, `channels` as the layer's kind says); canvas_corrupt when missing or not what it is named for.
    Result<mask::MaskPaint> tilePixels(const QString &hash, int width, int height, int channels) const { return readBlob(hash, width, height, channels); }

    // Where a stored tile's PNG is (it may not be there: readable only after tilePixels has checked it).
    QString blobFile(const QString &hash) const { return blobPath(hash); }
    // The tree hash a document would have, with nothing written.
    static Result<QString> treeHashOf(const mask::PaintDocument &document);
    // The revision with these fields and the hash its commit record is named by (what addRevision stores).
    static Revision makeRevision(const QString &tree, const QString &parent, const QString &producer, const QString &time);

    // ---- a canvas made elsewhere (#249; engine/canvas_package.h) ----
    // Makes a canvas of `chain` (oldest first, one line of parents, the last the head) named `name`. The input is
    // untrusted, so it is checked before anything stays: every revision is the record its id names (the same fields
    // hash to the same id), a tree is the content its hash names, each parent is the revision before it, and each tile
    // `fetch` gives is the pixels its hash names and the size its place gives. The canvas keeps `wantedId` when it is a
    // valid id nobody has in this store; otherwise it gets a new one (the answer's id says which). Nothing is written
    // outside the store's root; a failure leaves nothing behind. input_invalid for a chain that is not one,
    // canvas_corrupt for a tree or tile that is not what it is named for.
    Result<CanvasInfo> importCanvas(const QString &wantedId, const QString &name, const std::vector<ImportedRevision> &chain, const TileFetch &fetch);

private:
    QString canvasDir(const QString &canvasId) const;
    QString blobPath(const QString &hash) const;
    QString treePath(const QString &hash) const;
    QString resolvedPath(const QString &canvasId, const QString &revisionId) const;
    // "" for an open one, else "accepted" or "dropped".
    QString resolution(const QString &canvasId, const QString &revisionId) const;
    Result<QStringList> historyOfHead(const QString &canvasId, QString *head) const;
    Result<QString> storeTree(const mask::PaintDocument &document);
    Result<QString> storeTreeRecord(const Tree &tree);
    Result<Revision> addCommit(const QString &canvasId, const QString &treeHash, const QString &parent, const QString &producer);
    // editLayers' body: `change` of the head `base`'s tree, one revision; `newTiles` lets it name tiles the base has not (SetMask, merges).
    // `keepSame`: a change that gives the same tree still makes a revision (a resize to the same rectangle), else it writes nothing.
    Result<Commit> commitTree(const QString &canvasId, const QString &base, const std::function<Result<Tree>(const Tree &)> &change, const QString &producer, bool newTiles, bool keepSame = false);
    Result<Commit> commitEdit(const QString &canvasId, const QString &base, const LayerEdit &edit, const QString &producer, bool newTiles);
    QString selectionPath(const QString &hash) const;
    Errors storeBlob(const QString &hash, const mask::MaskPaint &tile);
    Result<mask::MaskPaint> readBlob(const QString &hash, int width, int height, int channels) const;

    QString m_root;
    Clock m_clock;
};

// How many groups deep a group goes (0 for a layer that is none); a tree holds at most maxGroupDepth.
inline constexpr int maxGroupDepth = 8;
int groupDepth(const LayerRecord &layer);
// Whether any layer of the tree is a group.
bool treeHasGroup(const Tree &tree);

// The tree as the JSON that is hashed and stored, and back (a bad one is canvas_corrupt).
QJsonObject treeToJson(const Tree &tree);
Result<Tree> treeFromJson(const QJsonObject &object);
// The tree of a document (hashes only, no files).
Result<Tree> treeOf(const mask::PaintDocument &document, const TileSink &consume = {});
// A tree as a document, its tiles read through `read` (CanvasStore::tilePixels): what load() does.
Result<mask::PaintDocument> documentOf(const Tree &tree, const std::function<Result<mask::MaskPaint>(const QString &hash, int width, int height, int channels)> &read);

} // namespace fugue::canvas
