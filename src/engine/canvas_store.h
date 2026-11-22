#pragma once

// The canvas revision store (#208, epic #206, spec 14.5): a canvas is a chain of immutable,
// content-hashed revisions, and one of them is the head. Nothing here knows a window or a node; a
// revision is a PaintDocument (engine/paint_document.h) saved as tiles (engine/canvas_tiles.h).
//
// On disk, under one root (`<data>/canvases`, so inside a project's own folder under FUGUE_HOME):
//
//   blobs/<aa>/<hex>.png          a tile's PNG, named by the hash of its raw pixels (shared by every canvas)
//   trees/<hex>.json              a revision's content: size, and per layer its name, kind, visibility,
//                                 opacity and tile hashes. Pixels and layer properties only.
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
// Picture layers are placeholders, as in an .ora (engine/paint_file.h): the picture is a node's
// input, never part of a revision.

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

#include "engine/canvas_tiles.h"
#include "engine/clock.h"
#include "engine/paint_document.h"
#include "engine/result.h"

namespace fugue::canvas {

inline constexpr int storeFormat = 1;

// What one layer of a tree is.
struct LayerRecord {
    QString name;
    mask::LayerKind kind = mask::LayerKind::Paint;
    bool visible = true;
    double opacity = 1;
    TileMap tiles; // empty for a picture layer; absent tiles are 0
    int channels() const { return kind == mask::LayerKind::Mask ? 1 : 4; }
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
};

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
    Result<CanvasInfo> info(const QString &canvasId) const;
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

    // The revision as a document (no history). Picture layers come back as placeholders.
    Result<mask::PaintDocument> load(const QString &canvasId, const QString &revisionId) const;

    // The tree hash a document would have, with nothing written.
    static Result<QString> treeHashOf(const mask::PaintDocument &document);

private:
    QString canvasDir(const QString &canvasId) const;
    QString blobPath(const QString &hash) const;
    QString treePath(const QString &hash) const;
    QString resolvedPath(const QString &canvasId, const QString &revisionId) const;
    // "" for an open one, else "accepted" or "dropped".
    QString resolution(const QString &canvasId, const QString &revisionId) const;
    Result<QStringList> historyOfHead(const QString &canvasId, QString *head) const;
    Result<QString> storeTree(const mask::PaintDocument &document);
    Errors storeBlob(const QString &hash, const mask::MaskPaint &tile);
    Result<mask::MaskPaint> readBlob(const QString &hash, int width, int height, int channels) const;

    QString m_root;
    Clock m_clock;
};

// The tree as the JSON that is hashed and stored, and back (a bad one is canvas_corrupt).
QJsonObject treeToJson(const Tree &tree);
Result<Tree> treeFromJson(const QJsonObject &object);
// The tree of a document (hashes only, no files).
Result<Tree> treeOf(const mask::PaintDocument &document, const TileSink &consume = {});

} // namespace fugue::canvas
