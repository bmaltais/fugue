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
// Closing the window stops it.

#include "engine/document.h"
#include "engine/node_type.h"

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "engine/model_catalog.h"
#include "engine/model_offers.h"
#include "engine/model_shape.h"

#include <QThreadPool>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class CanvasModel;

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

signals:
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
    std::shared_ptr<fugue::FileHashes> m_hashes;
    std::shared_ptr<std::atomic_bool> m_stopHashing = std::make_shared<std::atomic_bool>(false);
    QThreadPool m_hashPool;
    // Which listed files fit the node's diffusion model (#85), by node and input (fitsKey): by
    // path, read from the files' headers on m_shapePool (not behind a long hash). `asked` is the
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
    fugue::ModelShapes *m_shapes = &fugue::ModelShapes::shared(); // lives as long as the process
    QThreadPool m_shapePool;
    static QString fitsKey(const QString &node, const QString &input);
    // The diffusion model handle of `node` on the level shown, or of the node its model links
    // from (Apply LoRA from Load Models, through other Apply LoRAs); empty when none is picked.
    QJsonObject diffusionModelOf(const QString &node, int depth = 0) const;
    // For a text encoder, VAE or LoRA input of a node whose diffusion model is picked: which of
    // the listed files fit it, at once from the headers read before, else read on the pool.
    void requestFits(const QString &node, const QString &input);
};
