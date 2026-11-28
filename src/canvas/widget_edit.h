#pragma once

// Editing a node's input values in place (spec 6.2 widgets, 3.2, 2.5), the C++ half of the
// widgets in src/canvas/widgets/. The QML only draws and forwards what was typed or picked;
// everything that decides is here: which widget an input has and its constraints, turning
// typed text into a value, checking that value with the engine's own rules (checkLiteral,
// never the QML's), and writing it to the document through the canvas model's undo stack.
//
// Each commit is one undo step. A value set at once (a toggle, a choice, a chevron step, a
// picked file or model) is one CanvasModel::commit(). Typing and dragging are a session:
// begin() opens one edit on the model, preview() puts each valid value in the document as it
// is typed (so a save or a copy meanwhile has it), and finish() ends the edit, one step for
// the whole session; cancel() puts the value back and leaves no step. An invalid value is
// never written: preview() and the set calls return why, for the field to show, and leave the
// document as it was. A session ends by itself, as its own step, when any other edit begins
// (a command run while the field has focus, an undo), so the two never merge into one step;
// and without a step when the document is replaced (another tab, a file opened). Either way
// sessionEnded tells the field. A value is never written to a node or input that is gone.
//
// Seeds (6.2): "fixed" stores a number, "random" stores {"auto": "random"}, and "increment"
// stores a number and lists the input under layout.nodes.<id>.increment, so the file keeps
// the choice and a run still sees a plain number; advanceSeeds() adds one to each of them
// after a run starts (CanvasRuns calls it). Leaving "random" takes the seed of the node's last
// result (the run's value on the output of the same name), else draws one. A node added on
// the canvas starts its seeds on "random" (CanvasModel::addNode).
//
// Resource inputs show the model picker (2.5). Its list comes from a model source: by
// default each enabled provider instance (providers.json, or spec 8.2's default without one:
// this machine and each paired host) is asked to describe() itself when a picker opens, and
// every model it offers of the input's resource type, for one of the node type's
// capabilities (image.generate for Generate Image, image.edit for Edit Image; any capability
// for a node type that names none), is listed in the instances' order, with how to make a
// model that is not ready ready (8.4: Fugue never starts one by itself). An instance whose
// describe() fails lists nothing (engine/model_offers.h). After them come the input type's
// model files on disk (8.9), by folder in the model folders' order, listed when the picker
// opens. A name typed into the picker is stored as a handle served by name,
// {"resource": type, "name": …}.
//
// A file on disk is named by its content hash (2.5). Picking one whose hash is remembered
// stores its handle at once; otherwise the file is hashed on a worker thread, one file at a
// time (a model of several GB on a slow disk takes from seconds to minutes), hashState() says
// how far, and the handle is stored as its own undo step when the hash is known. The hash is
// saved to the shared store, so it is computed once for every Fugue process. A pick belongs to
// the level it was made at: entering a group meanwhile hides it, and it is stored once that
// level is shown again. Any other value set on the input meanwhile wins, and another document
// (another tab, a file opened) drops the pick; the hashing goes on and is remembered either way.
// Closing the window stops it. The glue the App view's picker shares (rows, empty text, download plan, the hash and header
// threads) is in workflow/model_picker.h; what stays here is the fits judging, the picks and their undo steps (#452).

#include "engine/document.h"
#include "engine/node_type.h"

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "canvas/canvas_choices.h"
#include "engine/model_catalog.h"
#include "engine/model_offers.h"
#include "engine/model_recommend.h"
#include "engine/model_shape.h"
#include "workflow/model_picker.h"

#include <QThreadPool>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class CanvasModel;
class QUndoStack;

namespace fugue {
class FileHashes;
class ModelDownloads;
class ModelLibrary;
class Registry;
}

class WidgetEdit : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // Bumped whenever a model list arrives, so the pickers re-read models().
    Q_PROPERTY(int modelsRevision READ modelsRevision NOTIFY modelsChanged)
    // Bumped whenever a picked file's hashing moves on or ends, so the inputs re-read hashState().
    Q_PROPERTY(int hashRevision READ hashRevision NOTIFY hashingChanged)
    // Bumped whenever the root's canvases may have changed, so the `canvas` inputs re-read canvases().
    Q_PROPERTY(int canvasesRevision READ canvasesRevision NOTIFY canvasesChanged)
    // Bumped whenever the document may have changed (an edit, an undo, another document), so a Canvas view re-reads
    // regionNodes().
    Q_PROPERTY(int graphRevision READ graphRevision NOTIFY graphChanged)

public:
    WidgetEdit(const fugue::Registry *registry, CanvasModel *canvas, QObject *parent = nullptr);
    ~WidgetEdit() override;

    // What the widget of an input needs: {widget, type, label, min, max, step, integer,
    // choices, optional, default (when there is one)}. `widget` is a 6.2 name, or "model" for
    // a resource input. Empty for an input that cannot be edited here: a placeholder's (no
    // known type), a list, a media or data input.
    Q_INVOKABLE QVariantMap spec(const QString &node, const QString &input) const;
    // The stored value: a number, text or boolean, {"auto": "random"} or a resource handle as
    // a map; undefined when the input is unset (it uses its default) or linked.
    Q_INVOKABLE QVariant value(const QString &node, const QString &input) const;
    // Where a linked input's value comes from: "<node title> · <output label>".
    Q_INVOKABLE QString source(const QString &node, const QString &input) const;

    // Why this text cannot be the input's value, in one sentence; empty when it can. Numbers
    // take a dot or the language's decimal separator. Empty text unsets a number, and an
    // optional text input with no default; a required number with no default needs one.
    Q_INVOKABLE QString check(const QString &node, const QString &input, const QString &text) const;
    // Sets the input from typed text, as one undo step. Returns check()'s answer; nothing is
    // written when it is not empty.
    Q_INVOKABLE QString setText(const QString &node, const QString &input, const QString &text);
    // Sets the input to a value (a boolean, a choice, a number, a handle), one undo step.
    Q_INVOKABLE QString setValue(const QString &node, const QString &input, const QVariant &value);
    // A file or folder from the picker (a file: URL or a path), stored as a local path.
    Q_INVOKABLE QString setPath(const QString &node, const QString &input, const QVariant &url);
    // A color from the color picker, stored as #rrggbb, or #rrggbbaa when not opaque.
    Q_INVOKABLE QString setColor(const QString &node, const QString &input, const QColor &color);
    // The Paste button of a Load Image's file input (workflow/image_inputs.h): whether the input
    // has one, and replacing its image with the clipboard's, as one undo step.
    Q_INVOKABLE bool canPasteImage(const QString &node, const QString &input) const;
    Q_INVOKABLE bool pasteImage(const QString &node);
    // The images in the inputs folder, newest first: {path, name, modified}. At most 48.
    Q_INVOKABLE QVariantList recentInputs() const;
    // Points the node's file at one of them, one undo step.
    Q_INVOKABLE bool useInput(const QString &node, const QString &path);
    // Takes the stored value out, so the default applies (or, for an optional input, nothing).
    Q_INVOKABLE void unset(const QString &node, const QString &input);

    // --- a typing or dragging session: one undo step in all ---
    Q_INVOKABLE void begin(const QString &node, const QString &input);
    // Writes the text (previewValue: the value) when it is valid; returns why not otherwise.
    Q_INVOKABLE QString preview(const QString &text);
    Q_INVOKABLE QString previewValue(const QVariant &value);
    Q_INVOKABLE void finish();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool editing() const { return !m_session.node.isEmpty(); }
    // Ends the session as finish() does and tells the field (sessionEnded). The canvas model
    // calls it when another edit begins.
    void endSession();
    // True while the session writes its own value (not another edit).
    bool writing() const { return m_writing; }

    // --- the text window (#715): a larger editor for a node's text inputs ---
    // The node's text inputs in the order its card shows them: [{name, label, widget ("multiline" or "line"), linked, syntax (the manifest's, or empty)}].
    Q_INVOKABLE QVariantList textInputs(const QString &node) const;
    // One typed change from a window that stays open beside the canvas: it writes `text` to the input inside a session of its own
    // (begun here when this input has none open, so another edit that ended it, or a field that took its place, never gets the
    // text), one undo step for the visit. Returns why not, as preview() does.
    Q_INVOKABLE QString typeInto(const QString &node, const QString &input, const QString &text);
    // Ends the session when it is this input's.
    Q_INVOKABLE void finishInto(const QString &node, const QString &input);
    // What a linked input gets: {linked, source (the node), sourceTitle, output (its label), has (the source ran and made text),
    // text (that run's, whole)}; {linked: false} for an input with a value.
    Q_INVOKABLE QVariantMap linkedText(const QString &node, const QString &input) const;

    // --- numbers (spin, slider, seed) ---
    // `from` moved by `steps` of the input's step (1 for a whole number, else 0.1 when it has
    // none), held inside min and max and rounded to the step.
    Q_INVOKABLE double stepped(const QString &node, const QString &input, double from, int steps) const;
    // The value at a share (0 to 1) of a slider's range, rounded to the step; and back.
    Q_INVOKABLE double valueAt(const QString &node, const QString &input, double fraction) const;
    Q_INVOKABLE double fraction(const QString &node, const QString &input, double value) const;
    // How the number shows in a field: whole numbers without a fraction.
    Q_INVOKABLE static QString numberText(double value);

    // --- seeds ---
    // "fixed", "random" or "increment".
    Q_INVOKABLE QString seedMode(const QString &node, const QString &input) const;
    // One undo step. From random to fixed or increment, a new number is drawn.
    Q_INVOKABLE void setSeedMode(const QString &node, const QString &input, const QString &mode);
    // Every incrementing seed gains one (back to its minimum past the maximum), one undo step.
    Q_INVOKABLE void advanceSeeds();
    // "Run from here" (spec 3.4): the whole workflow as a run takes it, with every random or increment seed of the nodes
    // strictly upstream of `from` set to the seed its shown result used, when there is one: `seedUsed(path, input)`, the
    // "Seed used" the node at that path id last showed. `from` is a path id; a group instance stands for every node inside it.
    // Upstream is read in the flattened graph, so it reaches into the group instances before `from` and, inside a group, out to
    // what feeds it. A seed is held where its value is written: on its node, or on the group instance whose exposed input
    // drives it. One written in a group definition that has more than one instance is left as it is, since holding it there
    // would hold it in every copy of the group in the run. The canvas's own document is not touched. `held` gets
    // "node/input" for each increment seed held on the level shown, for advanceSeedsExcept not to advance.
    using SeedUsed = std::function<std::optional<double>(const QString &path, const QString &input)>;
    fugue::Document holdUpstream(const QString &from, const SeedUsed &seedUsed, QSet<QString> *held) const;
    // advanceSeeds() leaving the increment seeds named in `held` (holdUpstream's keys) as they are.
    void advanceSeedsExcept(const QSet<QString> &held);

    // --- the model picker ---
    // One model (engine/model_offers.h): a provider's, or a file on disk.
    using Model = fugue::ListedModel;
    // Lists the models of a resource type that offer one of `capabilities` (all models of
    // the type when it is empty), grouped by provider instance in the user's order.
    using ModelSource = std::function<void(const QString &resourceType, const QStringList &capabilities,
                                           std::function<void(QList<Model>)> done)>;
    // Replaces where models come from (tests pass a fixed list).
    void setModelSource(ModelSource source) { m_source = std::move(source); }
    // The model folders searched for files on disk (tests); by default the settings' roots
    // (ModelLibrary::roots) when the picker opens.
    void setModelRoots(QStringList roots) { m_modelRoots = std::move(roots); }
    // Lists the input type's files on disk, and asks the source for the providers' models;
    // modelsChanged when they come.
    Q_INVOKABLE void requestModels(const QString &node, const QString &input);
    // The models last listed for the input, as maps {name, title, handle, status, provider,
    // note}, plus {path, folder, size} for a file on disk (provider empty); the providers'
    // first, then the files.
    Q_INVOKABLE QVariantList models(const QString &node, const QString &input) const;
    Q_INVOKABLE bool modelsLoading(const QString &node, const QString &input) const;
    // The diffusion model a text encoder, VAE or LoRA list is judged against (#85, #86): its
    // name, for the "Fits …" heading; empty when the list is not judged against one.
    // Each listed file also has `family` (engine/model_family.h, "" when none), and `recommended` with
    // `recommendedWhy` for the one file of a model the rule picks (engine/model_recommend.h, #370).
    Q_INVOKABLE QString fitsModel(const QString &node, const QString &input) const;
    // What the picker says when no model is listed: what kind is missing and how to get one.
    Q_INVOKABLE QString modelsEmptyText(const QString &node, const QString &input) const;
    int modelsRevision() const { return m_modelsRevision; }
    // Picks a listed model (its handle as a map), one undo step.
    Q_INVOKABLE QString setModel(const QString &node, const QString &input, const QVariantMap &handle);
    // A model served by this name, as typed.
    Q_INVOKABLE QString setModelName(const QString &node, const QString &input, const QString &name);
    // Picks the model file at `path`, which must be one of the input type's files in the model
    // folders: its handle now when its hash is remembered, else once hashed (see the top).
    // Returns why not, or empty.
    Q_INVOKABLE QString setDiskModel(const QString &node, const QString &input, const QString &path);
    // For a node whose model inputs name a file that is not on disk (its yellow warning, #600): per such input, the file of the
    // same weights that is (engine/model_use.h), as [{input, file, path, tier, text, otherSource}]. `text` is "Use what you have: ..."
    // as the other surfaces say it; the node's card offers "Use it", which is setDiskModel(node, input, path): one undo step.
    // Reads the model folders and headers: asked when the card is kept up, not on each hover.
    Q_INVOKABLE QVariantList onDiskOffers(const QString &node);
    // A picked file being hashed for the input: {progress: 0 to 1, name}; {error} when it could
    // not be read; empty otherwise.
    Q_INVOKABLE QVariantMap hashState(const QString &node, const QString &input) const;
    int hashRevision() const { return m_hashRevision; }

    // --- models to download (#134) ---
    // Every picker of a resource type also lists the catalog's models of that type that are not
    // on disk (engine/model_catalog.h), under "Download": models() gives them as maps with
    // `catalog` true, an `id`, the `title`, a `detail` (scale, architecture, license, size), its `size` in bytes, the
    // `keywords` a filter matches and the `note` (what it is good for). Picking one does not fetch
    // it: the window shows catalogInfo() (the license, the size, the source, where it will go) and
    // asks; downloadCatalogModel() is what its Download button calls. The file is fetched by the
    // window's one download line (workflow/model_downloads.h, shown in the banner), kept only when
    // its sha256 is the catalog's, and then put in the input, as a file picked from disk is.
    // Replaces where the catalog comes from (tests).
    void setCatalog(fugue::ModelCatalog catalog) { m_catalog = std::move(catalog); m_catalogSet = true; }
    void setDownloads(fugue::ModelDownloads *downloads);
    // What the question shows about a catalog model of the input's type: {title, description,
    // scale, architecture, license, licenseLine, licenseUrl, commercial, attribution, author,
    // source, url, host, size, sizeText, folder (where it will be saved), file}; empty when unknown.
    Q_INVOKABLE QVariantMap catalogInfo(const QString &node, const QString &input, const QString &id) const;
    // Queues the download of a catalog model for the input; returns why not, or empty. When the
    // file arrives its handle is put in the input (one undo step), if the node still has it.
    Q_INVOKABLE QString downloadCatalogModel(const QString &node, const QString &input, const QString &id);

    // --- the canvas picker (#211, spec 6.2 `canvas`, 14.5) ---
    // An input with the `canvas` widget holds a canvas id, as a line does, so a workflow stays
    // portable; the picker lists this root's canvases (the session's store, a project's own).
    // The rows: [{id, name, width, height, size, changed, changedText, head}], newest first.
    Q_INVOKABLE QVariantList canvases();
    // The row of one canvas of this root; empty when there is none by that id (the input then
    // shows it as missing, with the list still available).
    Q_INVOKABLE QVariantMap canvasRow(const QString &id);
    // Makes a canvas from a picture or .ora file (Import picture…), in this root's store, and sets the input to
    // its id as one undo step. Returns why not, or empty. The canvas itself is not undone with the step: nothing is
    // ever deleted (spec 14.5). A blank canvas, or one from the clipboard, comes from the New canvas dialog.
    Q_INVOKABLE QString importCanvas(const QString &node, const QString &input, const QString &path, const QString &name);
    // Sets the input to a canvas the New canvas dialog just made (#232), as one undo step; why not, or empty.
    Q_INVOKABLE QString useCanvas(const QString &node, const QString &input, const QString &id);
    // The folder of this root's canvas store, empty for the session's: where the dialog makes its canvas.
    Q_INVOKABLE QString canvasStoreRoot() const { return m_canvases.storeRoot(); }
    // --- the region picker (#213) ---
    // What the picker window needs of a node with a canvas and a rectangle (Canvas Region): {ok: true,
    // canvas, revision, storeRoot ("" is the session's), x, y, width, height, padding, scale, cap, multiple, canvasLinked, maskLinked, revisionLinked,
    // editable, selection (the painted selection's hash, #230), selectionWritable}. The numbers are the stored ones or the defaults; a linked input's value is not known
    // before a run, so its default stands (padding and the rest) and `editable` is false when any of
    // x, y, width, height is linked. Empty for another node.
    Q_INVOKABLE QVariantMap regionState(const QString &node) const;
    // The node as user text names it (CanvasModel::nodeName): its title, the id only to tell two of one title apart.
    Q_INVOKABLE QString nodeName(const QString &node) const;
    // The Canvas Region nodes of the level shown, [{id, name, canvas}] in title order: the nodes a Canvas view can follow
    // without a hunt in the graph (#228).
    Q_INVOKABLE QVariantList regionNodes() const;
    // Adds a Canvas Region node that reads `canvas`, with `rect` ({x, y, width, height}) as its rectangle, at the middle
    // of the graph's view, as ONE undo step ("Add a Canvas Region"): what a rectangle drawn on a canvas with no Region
    // node becomes. Its id, or "" when it could not be made.
    Q_INVOKABLE QString addRegionNode(const QString &canvas, const QVariantMap &rect);
    // Sets x, y, width and height of the node as ONE undo step; false when nothing changed or one is linked.
    // A drawn rectangle replaces a painted selection: the node's `selection` is emptied in the same step (#230).
    Q_INVOKABLE bool setRegion(const QString &node, int x, int y, int width, int height);
    // A selection painted in a Canvas view (#230): the node's `selection` input becomes the stored selection's hash and
    // x, y, width and height its bounds (so the frame, the padded box and the size the model gets read right), as ONE
    // undo step ("Use selection"); the rectangle stays as it was when it is linked. False when nothing changed, the node
    // has no `selection` input or it is linked.
    Q_INVOKABLE bool setSelection(const QString &node, const QString &selection, int x, int y, int width, int height);
    // The document's undo stack: a view that shows a node's values (the Canvas view's region) re-reads on its index.
    QUndoStack *undoStack() const;
    int canvasesRevision() const { return m_canvases.revision(); }
    int graphRevision() const { return m_graphRevision; }
    // The list's source (tests set the store's folder).
    fugue::CanvasChoices *canvasChoices() { return &m_canvases; }

signals:
    void canvasesChanged();
    void graphChanged();
    void modelsChanged();
    void hashingChanged();
    // A session ended without finish() or cancel(): another edit began, or the document was
    // replaced. The field closes.
    void sessionEnded();

private:
    struct Target {
        const fugue::InputSpec *spec = nullptr;
        fugue::PortKind kind = fugue::PortKind::Unknown;
        QString widget;
    };
    // The input's spec when it can be edited here.
    Target target(const QString &node, const QString &input) const;
    // Parses typed text: the value, `unset` for empty text that unsets, or why not.
    QString parse(const Target &target, const QString &text, QJsonValue *value, bool *unsetIt) const;
    QString checkValue(const Target &target, const QJsonValue &value) const;
    // Writes the value (undefined: unset) as one step, or into the open session.
    void write(const QString &node, const QString &input, const QJsonValue &value, const QString &text);
    QString stepText(const QString &node, const QString &input) const;
    double randomSeed(const fugue::InputSpec &spec) const;
    // The capabilities of the node type the input is on (a group's: its inner node's).
    QStringList capabilities(const QString &node, const QString &input) const;
    // Where the input's model list is kept: its resource type and capabilities.
    QString modelsKey(const QString &node, const QString &input) const;
    // The seed the node's last result was made with, when a run showed one.
    std::optional<double> lastSeed(const QString &node, const QString &input) const;
    // The model folders in effect now (the settings' roots, or setModelRoots()).
    fugue::ModelLibrary library() const;
    fugue::FileHashes &hashes();
    // Hashes `path` on the worker thread unless it is being hashed already.
    void startHashing(const QString &path);
    void hashed(const QString &path, const QString &hash);
    // Stores the handle of each pick of the level shown whose hash is known.
    void storePicks();
    // The pick for the input on the level shown, else null.
    struct Pick;
    const Pick *pickFor(const QString &node, const QString &input) const;
    // Drops it; true when there was one.
    bool dropPick(const QString &node, const QString &input);

    // The files being fetched for an input: stored once they have arrived (downloadsFinished).
    struct Download {
        QString scope, node, input, path, resource, name, hash;
    };
    void downloadsFinished();
    fugue::CanvasChoices m_canvases;
    int m_graphRevision = 0;
    // Whether `input` has the canvas widget, and its target; empty Target otherwise.
    Target canvasTarget(const QString &node, const QString &input) const;
    fugue::ModelCatalog m_catalog;
    bool m_catalogSet = false;
    QHash<QString, QList<fugue::CatalogModel>> m_catalogMissing; // by modelsKey()
    fugue::ModelDownloads *m_downloads = nullptr;
    QList<Download> m_downloading;

    const fugue::Registry *m_registry;
    CanvasModel *m_canvas;
    struct Session {
        QString node, input;
        fugue::InputValue before;
        bool open = false; // holds an edit open on the model
    } m_session;
    bool m_writing = false;
    ModelSource m_source;
    QHash<QString, QList<Model>> m_models; // by modelsKey()
    QSet<QString> m_loading;
    int m_modelsRevision = 0;
    std::optional<QStringList> m_modelRoots;
    QHash<QString, QList<Model>> m_diskModels; // by modelsKey()
    // A file picked for an input and being hashed.
    struct Pick {
        QString scope; // CanvasModel::scopePath() when it was picked
        QString node, input;
        QString path, resource, name;
        QString hash;  // known, waiting for its level to be shown
        QString error; // it could not be read
        double progress = 0;
    };
    QList<Pick> m_picks;
    QSet<QString> m_hashing; // paths on the worker thread
    int m_hashRevision = 0;
    QVariantList m_offers; // onDiskOffers()'s last answer, for the node and document it was asked for
    QByteArray m_offersKey;
    QElapsedTimer m_offersAge;
    fugue::ModelWorkers m_work; // the hash store and the hash and header threads (workflow/model_picker.h, #452)
    // Which listed files fit the node's diffusion model (#85), by node and input (fitsKey): by
    // path, read from the files' headers on m_work's header thread (not behind a long hash). `asked` is the
    // request the answer belongs to; one that comes after a later request is dropped.
    struct Judged {
        bool fits = false;
        QString why; // why it does not, when that is worth a line (a format the helper cannot load)
    };
    struct Fits {
        int asked = 0;
        QString model; // the diffusion model's name, for the list's heading; empty for a diffusion model's own list
        QHash<QString, Judged> byPath;
    };
    QHash<QString, Fits> m_fits;
    int m_fitsAsked = 0;
    // The Recommended marks of the listed files (#370), by node and input (fitsKey), read from the files'
    // headers on m_work like the fits; `asked` drops an answer that a later request overtook.
    struct Marks {
        int asked = 0;
        QHash<QString, fugue::ModelMark> byPath;
    };
    QHash<QString, Marks> m_marks;
    void requestMarks(const QString &node, const QString &input);
    // The size in MB of the other models the node has picked (a text encoder and VAE beside a diffusion
    // model...), which share the device's memory with the one listed; remembered hashes only.
    qint64 otherModelsMb(const QString &node, const QString &input);
    fugue::ModelShapes *m_shapes = &fugue::ModelShapes::shared(); // lives as long as the process
    static QString fitsKey(const QString &node, const QString &input);
    // The diffusion model handle of `node` on the level shown, or of the node its model links
    // from (Apply LoRA from Load Models, through other Apply LoRAs); empty when none is picked.
    QJsonObject diffusionModelOf(const QString &node, int depth = 0) const;
    // For a text encoder, VAE or LoRA input of a node whose diffusion model is picked: which of
    // the listed files fit it, at once from the headers read before, else read on the pool.
    void requestFits(const QString &node, const QString &input);
};
