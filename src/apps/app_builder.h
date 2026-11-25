#pragma once

// Make an app (#393, epic #397, spec 12.19): the builder that turns the workflow on the canvas into an app (spec 4.9) with no
// hand-editing of JSON, and edits the block of an app it already is. The class is the logic; AppBuilderDialog.qml only draws it
// (`model.appBuilder` on the canvas model, as `model.groups` is the group editor's).
//
// The draft: the same list the block holds (engine/app_binding.h), kept apart from the document until Apply. begin() reads the
// workflow's block into it (or an empty draft), the calls below change it, and apply() writes it back through app::setApp as ONE
// undo step on the document ("Make app" or "Edit app"), so Ctrl+Z takes the block out again. Nothing changes the document before
// that, and Cancel throws the draft away. The rules are the group interface editor's (spec 5.4, canvas/group_edit.h): an exposed
// input has a label, a section ("Advanced" folds), a range and a default, can drive several inner inputs of one type, and the
// list is the form's order. Unlike a group's face, the builder needs no group: it lists any unlinked value input of the workflow.
//
// Choosing: pickAt() is the canvas's click-to-expose mode (a click on an input row exposes it, a second click stops exposing
// it, a click on a Save or Preview node adds or removes it as an output), `inputChoices()` and `outputChoices()` are the same
// lists for the keyboard, and exposeInput()/addOutput() the calls both use. Roles (prompt, image, seed, model...) are guessed from
// the input the way the templates name them, so a form shows the right control; a plain input gets its input's name.
//
// Checking: `problems` says in words what apply() would refuse (no title, no output, an output that is not a sink, a target
// that is linked or of mixed types), from the title and app::checkApp, the one check the App view and `fugue app` use.
//
// Preview: `preview` is a second AppView (apps/app_view.h) given the workflow with the draft's block in a copy, so the pane
// shows the form the App view will show, with its own controls (AppInput), live as the draft changes.

#include <QJsonObject>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

#include "apps/app_view.h"
#include "engine/app_binding.h"

class CanvasModel;

namespace fugue {
class Registry;
}

class AppBuilder : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through the canvas model")
    // The builder is open: the dialog shows, or the canvas is in click-to-expose mode.
    Q_PROPERTY(bool open READ isOpen NOTIFY stateChanged)
    // Click-to-expose: the dialog steps aside and the canvas takes clicks as picks until finishPicking().
    Q_PROPERTY(bool picking READ picking NOTIFY stateChanged)
    // The workflow already had an app block when the builder opened: it says Edit app, and offers Remove app.
    Q_PROPERTY(bool editing READ editing NOTIFY stateChanged)
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY changed)
    Q_PROPERTY(QString description READ description WRITE setDescription NOTIFY changed)
    // "app" or "graph": what opening the workflow shows first.
    Q_PROPERTY(QString defaultView READ defaultView WRITE setDefaultView NOTIFY changed)
    // [{key, label (as set), shown (what the form says), role, kind, type, section, advanced, min, max, ranged, default,
    //   targets: ["node.input"], titles: ["Node title"], first, last}], in the form's order.
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY changed)
    // [{node, title, label (as set), type, sink, first, last}], in the order the results show.
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY changed)
    // Where the draft's inputs and outputs are on the canvas, in the world: [{x, y, width, height, output}].
    Q_PROPERTY(QVariantList marks READ marks NOTIFY changed)
    // What apply() would refuse, one sentence each; empty when it would go through.
    Q_PROPERTY(QStringList problems READ problems NOTIFY changed)
    // The last pick, load or refusal in one sentence ("Added Prompt", "Seed is linked to another node, ..."); empty for none.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    // The App view's form of the draft, for the preview pane.
    Q_PROPERTY(fugue::AppView *preview READ preview CONSTANT)

public:
    AppBuilder(const fugue::Registry *registry, CanvasModel *canvas, QObject *parent = nullptr);

    bool isOpen() const { return m_open; }
    bool picking() const { return m_picking; }
    bool editing() const { return m_editing; }
    QString title() const { return m_draft.title; }
    QString description() const { return m_draft.description; }
    QString defaultView() const { return m_draft.defaultView; }
    QVariantList inputs() const;
    QVariantList outputs() const;
    QVariantList marks() const;
    QStringList problems() const;
    QString notice() const { return m_notice; }
    fugue::AppView *preview();
    void setTitle(const QString &title);
    void setDescription(const QString &description);
    void setDefaultView(const QString &view);

    // --- open and close ---

    // The app.make command: asks the window to open the builder (it shows the graph first when the App view is shown).
    Q_INVOKABLE void requestOpen() { emit openRequested(); }
    // The workflow has an app block now: the menu says Edit app.
    bool hasBlock() const;

    // Opens on the workflow's block, or an empty draft with `suggestedTitle`. False (and `notice` says why) inside a group:
    // an app is made of the whole workflow.
    Q_INVOKABLE bool begin(const QString &suggestedTitle = {});
    // Throws the draft away; the document was never touched.
    Q_INVOKABLE void cancel();
    // Writes the draft into the workflow as one undo step. False (the problems stay) when it has any. `applied` follows, with
    // whether to show the App view.
    Q_INVOKABLE bool apply(bool openAppView = false);
    // Takes the block out of the workflow, as one undo step ("Remove app").
    Q_INVOKABLE bool removeApp();

    // --- the inputs ---

    // Exposes the input of a node as one more input of the form, as the group editor's "Expose on the group". False, with
    // `notice` saying why, when it cannot be a form input (linked, a wire-only type, already exposed, no such input).
    Q_INVOKABLE bool exposeInput(const QString &node, const QString &input);
    // Exposes it, or stops exposing it when it is already a target (a second click); the sentence it says is the notice.
    Q_INVOKABLE bool toggleInput(const QString &node, const QString &input);
    // Stops exposing a form input altogether (every target).
    Q_INVOKABLE bool removeInput(const QString &key);
    // `change`: label, advanced (bool), min, max, default (each a string; empty clears). False for a key not in the list.
    Q_INVOKABLE bool editInput(const QString &key, const QVariantMap &change);
    Q_INVOKABLE bool moveInput(const QString &key, int to);
    // The inner inputs that could join the form input (one input driving several, 5.4): of its type, not exposed yet;
    // [{node, input, title, label}].
    Q_INVOKABLE QVariantList driveCandidates(const QString &key) const;
    // Adds the inner input as one more target of the form input.
    Q_INVOKABLE bool alsoDrive(const QString &key, const QString &node, const QString &input);
    // Every input of the workflow that could be exposed, for the keyboard: [{node, input, title, label, exposed}].
    Q_INVOKABLE QVariantList inputChoices() const;

    // --- the outputs ---

    // Adds a node's result as an output. Any node of the workflow goes in: one that is not a Save or Preview sink is a problem
    // the list says, the way checkApp does.
    Q_INVOKABLE bool addOutput(const QString &node);
    Q_INVOKABLE bool toggleOutput(const QString &node);
    Q_INVOKABLE bool removeOutput(const QString &node);
    Q_INVOKABLE bool setOutputLabel(const QString &node, const QString &label);
    Q_INVOKABLE bool moveOutput(const QString &node, int to);
    // The Save and Preview nodes of the workflow, for the keyboard: [{node, title, added}].
    Q_INVOKABLE QVariantList outputChoices() const;

    // --- click-to-expose ---

    // Starts and ends picking on the canvas (the dialog steps aside while it lasts).
    Q_INVOKABLE void startPicking();
    Q_INVOKABLE void finishPicking();
    // A click at a world point: an input row toggles as an input, a sink as an output; the notice says which.
    Q_INVOKABLE void pickAt(qreal x, qreal y);

    // --- the block ---

    // The draft as the block holds it (what apply() writes).
    fugue::app::App draft() const { return m_draft; }
    QJsonObject block() const { return fugue::app::toJson(m_draft); }

signals:
    void openRequested();
    void stateChanged();
    void changed();
    void noticeChanged();
    // apply() wrote the block; `openAppView`: the window shows the App view now.
    void applied(bool openAppView);

private:
    void setNotice(const QString &text);
    void touch();
    void say(const QString &text) { setNotice(text); }
    int indexOfInput(const QString &key) const;
    int indexOfOutput(const QString &node) const;
    QString titleOf(const QString &node) const;
    fugue::Document documentWithDraft() const;
    bool writeBlock(bool remove, bool openAppView);

    const fugue::Registry *m_registry;
    CanvasModel *m_canvas;
    fugue::app::App m_draft;
    bool m_open = false;
    bool m_picking = false;
    bool m_editing = false;
    QString m_notice;
    std::unique_ptr<fugue::AppView> m_preview;
};
