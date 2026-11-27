#pragma once

// Editing groups on the canvas (spec 5.2-5.5), split out of CanvasModel like WidgetEdit
// (canvas/widget_edit.h) and exposed to QML as `model.groups`. It has three jobs:
//
// - the level shown: enter(), leave() and leaveTo() (a breadcrumb click). Entering shows a
//   group definition's inside; editing it edits the definition, so every instance changes
//   (5.3). Entering and leaving are not edits, like the viewport. The scope itself (which
//   instances are entered, the whole workflow behind them) stays in the CanvasModel, which
//   draws the level: breadcrumb, inGroup, scopeGroup(), scopeDocument().
// - the group edits: Make group, Ungroup and Make unique, and the interface of the group
//   shown (expose, rename, reorder its ports). Each is one undo step, made through the
//   model's commit(); the document changes themselves are engine/groups.h.
// - the library (5.3): save, update and rename, and the notice line.
//
// A refusal sets `problem`, one sentence with how to fix it, and returns false; an edit that
// worked clears it. `notice` says where a library action put its result. The window shows
// both (GroupDialogs.qml) until dismiss().

#include "engine/document.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>

class CanvasModel;

namespace fugue {
class Registry;
}

class GroupEdit : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // Why the last group edit was refused, one sentence; empty after one that worked.
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    // Where a library action put its result; empty when there is nothing to say.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)

public:
    GroupEdit(const fugue::Registry *registry, CanvasModel *canvas, QObject *parent = nullptr);

    QString problem() const { return m_problem; }
    QString notice() const { return m_notice; }
    // Clears the problem and the notice (the window's dismiss button).
    Q_INVOKABLE void dismiss();
    // Says the outcome of a library action made elsewhere (a group deleted from the node
    // library, #156) on the window's notice line: `problem` when it failed, else `notice`.
    Q_INVOKABLE void announceLibraryResult(const QString &notice, const QString &problem);

    // --- the level shown ---
    //
    // An undo restores the whole workflow and the canvas stays on its level, or goes up to
    // the nearest level that is still there.

    // Shows the inside of a group instance of the level (Tab, a double-click on its body).
    Q_INVOKABLE bool enterGroup(const QString &instance);
    // Back to the level above (Escape), with the instance selected; false at the top.
    Q_INVOKABLE bool leaveGroup();
    // Back to the level `depth` (0: the top), as a breadcrumb click does.
    Q_INVOKABLE void leaveTo(int depth);

    // --- make, ungroup (5.5) ---

    // Make group (Ctrl+Shift+E) from the selection; `leaveSinks` leaves its sinks outside,
    // wired to the group's outputs. The instance's id, or empty (then `problem` says why).
    // Inside a group, the exposed inputs and outputs of the group shown that led to the
    // nodes now lead through the new one.
    Q_INVOKABLE QString makeGroup(bool leaveSinks = true);
    // The group.make command: with sinks selected, makeGroupAsked(their titles) first, for
    // the window to ask whether they stay outside (5.5); without, makeGroup() at once.
    Q_INVOKABLE void requestMakeGroup();
    // Ungroup (Ctrl+Shift+G) the selected group instances; their inner nodes are selected.
    Q_INVOKABLE QStringList ungroupSelection();
    // Make unique (5.3): the selected instance gets its own copy of its definition.
    Q_INVOKABLE QString makeUnique();

    // --- the interface of the group shown (5.4) ---
    //
    // Each edit is one undo step on the whole workflow, so instances elsewhere follow.

    // {id, name, version, inputs: [{name, label, section, min, max, default, choices, innerChoices,
    // targets: ["node.input"], sets, type}], outputs: [{name, label, from, fromNode, fromPort,
    // comesFrom, type}]}; empty at the top. `sets` and `comesFrom` say it in words, with the inner
    // nodes' titles and the ports' labels: "Sets Model on Encode and Sample", "From Image on Picture".
    Q_INVOKABLE QVariantMap groupInterface() const;
    // The inner inputs (or outputs) of the group shown that are not exposed yet, in the canvas's
    // order, as [{node, port, title: "<node title> > <port label>"}]: the editor's Add lists.
    Q_INVOKABLE QVariantList exposeChoices(bool output) const;
    // Expose an inner input (into: as one more target of that exposed input) or output, and
    // stop exposing it.
    Q_INVOKABLE bool exposeInput(const QString &node, const QString &input, const QString &into = {});
    Q_INVOKABLE bool unexposeInput(const QString &node, const QString &input);
    Q_INVOKABLE bool exposeOutput(const QString &node, const QString &output);
    Q_INVOKABLE bool unexposeOutput(const QString &name);
    // Stops exposing an exposed input altogether (every target).
    Q_INVOKABLE bool removeExposedInput(const QString &name);
    // The name the face exposes an inner port under, or empty.
    Q_INVOKABLE QString exposedName(const QString &node, const QString &port, bool output) const;
    // The inner inputs that could join an exposed input (one-to-many, 5.4): of its type and
    // not exposed yet, as [{node, input, title}].
    Q_INVOKABLE QVariantList exposeCandidates(const QString &name) const;
    Q_INVOKABLE bool editExposedInput(const QString &name, const QVariantMap &change);
    Q_INVOKABLE bool renameExposedOutput(const QString &name, const QString &newName);
    Q_INVOKABLE bool moveExposed(bool output, const QString &name, int to);
    Q_INVOKABLE bool renameGroup(const QString &name);
    // A port a rename can relabel (#72): on a group card, or an inner port the group shown
    // exposes; {group, name, output, label (as shown)}, or empty.
    Q_INVOKABLE QVariantMap portToRename(const QString &node, const QString &port, bool output) const;
    // Sets the label the group's face shows for that port; the name stays. One undo step.
    Q_INVOKABLE bool renamePort(const QString &node, const QString &port, bool output, const QString &label);

    // --- the library (5.3, engine/group_library.h) ---

    // Save to library: the selected group instance's definition, or the group shown, goes to
    // the library as local/<name> at the version 3.6 gives, which the workflow's definition
    // and its instances take. `notice` says where it went.
    Q_INVOKABLE bool saveGroupToLibrary();
    // What updating the node's group would change: {name, from, to, addedInputs,
    // removedInputs, addedOutputs, removedOutputs}; empty when there is no update. (Whether
    // there is one is CanvasModel::updateAvailable, which the node rows show.)
    Q_INVOKABLE QVariantMap groupUpdate(const QString &node) const;
    // Replaces the workflow's definition of the node's group with the library's: every
    // instance moves together (5.3). One undo step.
    Q_INVOKABLE bool updateGroupInstance(const QString &node);
    // The group.update command: groupUpdateAsked(groupUpdate(selected)) for the window to show.
    Q_INVOKABLE void requestGroupUpdate();
    // Renames a group in the library itself (#74); the workflow follows its new id.
    Q_INVOKABLE bool renameLibraryGroup(const QString &libraryId, const QString &name);
    // A library group was renamed elsewhere (Settings): every open document follows its new id.
    void followLibraryRename(const QString &from, const QString &to);
    // A group card renamed from the canvas (F2, a double-click on its title): the group takes
    // the name, and the library is offered it.
    void renameGroupCard(const QString &node, const QString &name);

    // Where the library is (tests point it elsewhere); the user's own by default.
    void setLibraryDir(const QString &dir) { m_libraryDir = dir; }

    // Sets the problem (a paste or a face edit the model refused).
    void setProblem(const QString &problem);

signals:
    void problemChanged();
    void noticeChanged();
    void makeGroupAsked(const QStringList &sinks);
    void groupUpdateAsked(const QString &node, const QVariantMap &changes);
    // saveGroupToLibrary() or renameLibraryGroup() changed the library: the node types are
    // read again (#70).
    void librarySaved();
    // A group of the library was renamed in this workflow: rename it there too? (#74)
    void libraryRenameOffered(const QString &libraryId, const QString &name);

private:
    // An inner port as a person reads it: its label and its node's title (the ids when the
    // level shown has no such row).
    struct PortWords {
        QString port;
        QString node;
    };
    PortWords portWords(const fugue::PortRef &port, bool output) const;
    QString setsText(const QList<fugue::PortRef> &targets) const;
    QString comesFromText(const fugue::PortRef &from) const;
    void setNotice(const QString &notice);
    QString libraryPath() const;
    // The sinks among the selected nodes, for the question Make group asks first.
    QStringList selectedSinks() const;
    // A change of the group shown's face, committed as one step named `text`, or refused.
    bool applyFaceEdit(const std::function<fugue::Result<fugue::Document>(const fugue::Document &, const QString &)> &edit,
                       const QString &text);

    const fugue::Registry *m_registry;
    CanvasModel *m_canvas;
    QString m_problem;
    QString m_notice;
    QString m_libraryDir;
};
