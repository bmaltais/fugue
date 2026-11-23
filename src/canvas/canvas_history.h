#pragma once

// The revision history of a canvas for the window (#210, epic #206, spec 14.5): the revisions and the
// candidates of one canvas as a list, any two of them compared (as two flattened pictures for
// CompareView and a written summary of what differs), and a candidate accepted or dropped. All the
// work is engine/canvas_store.h and engine/canvas_diff.h; this turns it into properties QML binds to.
// It makes no window and reads no pixel of a revision until one is selected.
//
// A row of `revisions` is {id, short, time, producer, kind, onHead, name, tag}: kind is "head", "history",
// "candidate" or "dropped" (a candidate hidden by drop); `name` is how every surface names the revision ("Head",
// "Candidate 82d368f5", "Dropped 82d368f5", or the short hash) and `tag` its status word ("Head", "Candidate", "Dropped",
// or empty), capitalized (docs/ui-ergonomics.md D3). Candidates come first, then the history from
// the head back. Selecting a revision compares it (B) with its parent (A); compare(a, b) picks both.

#include <QObject>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include "engine/canvas_store.h"

namespace fugue {

class CanvasHistory : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // The folder of the store; empty is the session's (`<data>/canvases`). A test sets its own.
    Q_PROPERTY(QString storeRoot READ storeRoot WRITE setStoreRoot NOTIFY storeChanged)
    Q_PROPERTY(QString canvasId READ canvasId WRITE setCanvasId NOTIFY canvasChanged)
    // [{id, name, head, created, width, height, size, changed, changedText}], newest first (canvas/canvas_choices.h).
    Q_PROPERTY(QVariantList canvases READ canvases NOTIFY storeChanged)
    Q_PROPERTY(QVariantList revisions READ revisions NOTIFY revisionsChanged)
    Q_PROPERTY(QString revisionA READ revisionA NOTIFY compareChanged)
    Q_PROPERTY(QString revisionB READ revisionB NOTIFY compareChanged)
    Q_PROPERTY(QUrl sourceA READ sourceA NOTIFY compareChanged)
    Q_PROPERTY(QUrl sourceB READ sourceB NOTIFY compareChanged)
    Q_PROPERTY(QUrl sourceDifference READ sourceDifference NOTIFY compareChanged)
    Q_PROPERTY(QSize sizeA READ sizeA NOTIFY compareChanged)
    Q_PROPERTY(QSize sizeB READ sizeB NOTIFY compareChanged)
    // What differs between A and B, one line each (layers, tiles, the rectangle, a size change).
    Q_PROPERTY(QStringList summary READ summary NOTIFY compareChanged)
    // The kind of B ("candidate" when it can be accepted or dropped), and "" when nothing is selected.
    Q_PROPERTY(QString kindB READ kindB NOTIFY compareChanged)
    // The last thing that failed, as a sentence with its hint; cleared by the next action.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    // True when B has a parent to compare with (A is not B).
    Q_PROPERTY(bool hasParent READ hasParent NOTIFY compareChanged)
    // A file name for saving B's picture: "<canvas name>-<short hash>.png".
    Q_PROPERTY(QString exportName READ exportName NOTIFY compareChanged)
    // What the last export did, as a sentence ("Saved ... "); cleared by the next selection.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)

public:
    explicit CanvasHistory(QObject *parent = nullptr) : QObject(parent) {}

    QString storeRoot() const { return m_root; }
    void setStoreRoot(const QString &root);
    QString canvasId() const { return m_canvas; }
    void setCanvasId(const QString &id);
    QVariantList canvases() const;
    QVariantList revisions() const { return m_revisions; }
    QString revisionA() const { return m_a; }
    QString revisionB() const { return m_b; }
    QUrl sourceA() const { return m_sourceA; }
    QUrl sourceB() const { return m_sourceB; }
    QUrl sourceDifference() const { return m_difference; }
    QSize sizeA() const { return m_sizeA; }
    QSize sizeB() const { return m_sizeB; }
    QStringList summary() const { return m_summary; }
    QString kindB() const;
    QString error() const { return m_error; }
    bool hasParent() const { return !m_b.isEmpty() && m_a != m_b; }
    QString exportName() const;
    QString notice() const { return m_notice; }

    // Reads the canvas again (after another process wrote to it).
    Q_INVOKABLE void reload();
    // B is `revision`, A its parent (itself when it has none).
    Q_INVOKABLE void select(const QString &revision);
    // A revision as A, B unchanged (the head, or B itself, when B is not set).
    Q_INVOKABLE void setA(const QString &revision);
    Q_INVOKABLE void compare(const QString &a, const QString &b);
    // Saves the picture of B (what the viewer shows) at `target`, a path or a file URL, by its extension: .png, .jpg,
    // .jpeg and .webp are the flattened picture, .ora keeps the layers; another extension (or none) gets ".png" added.
    // A file already there is never replaced. True when written.
    Q_INVOKABLE bool exportPicture(const QString &target);
    // The canvas as a whole (the Canvas view's File menu, #232). Each reads the store again; false (and `error`) when it
    // could not. A blank name for duplicateCanvas is "<name> copy".
    Q_INVOKABLE bool renameCanvas(const QString &name);
    // Makes the copy and shows it.
    Q_INVOKABLE bool duplicateCanvas(const QString &name = QString());
    // Deletes the shown canvas for good and shows the newest of the others (none: the empty state).
    Q_INVOKABLE bool deleteCanvas();
    // Accept a candidate into the head (see CanvasStore::accept); true when the head moved. The new head is selected.
    Q_INVOKABLE bool accept(const QString &revision);
    // Hide a candidate; true when done.
    Q_INVOKABLE bool drop(const QString &revision);
    // Shows a canvas just made elsewhere (the New canvas dialog, #232): the list is read again and it is the one shown.
    Q_INVOKABLE void useCanvas(const QString &id);
    // Saves the shown canvas as a package to share (#249; Share…): its layers as they are now, or with the history of
    // every step too. ".fuguepack" is added; a file already there is never replaced. True when written, else `error`.
    Q_INVOKABLE bool sharePackage(const QString &target, bool withHistory);
    // Makes a canvas from a picture file in this store and opens it (#228; Import picture…): a path or a file: URL; a
    // blank name is the file's. A shared canvas (.fuguepack, #249) comes in as a new canvas with its history when it has
    // one, and the notice says when its id was taken here so it got another. Returns why not, or empty; the new canvas is then `canvasId`.
    Q_INVOKABLE QString importPicture(const QString &path, const QString &name = QString());

signals:
    void storeChanged();
    void canvasChanged();
    void revisionsChanged();
    void compareChanged();
    void errorChanged();
    void noticeChanged();

protected:
    // Kept protected so CanvasViewModel (canvas/canvas_view_model.h), the same model with a head picture, an overlay and
    // a region, shares all of it.
    canvas::CanvasStore store() const;
    void fail(const Errors &errors);
    void clearCompare();
    // The picture of a revision as a PNG file in the history's own folder (made once); false when it cannot be.
    bool picture(const QString &revision, QUrl *url, QSize *size);

    QString m_root;
    QString m_canvas;
    QVariantList m_revisions;
    QString m_head;
    QStringList m_candidates; // open candidates and dropped ones, to tell a row's kind
    QString m_a, m_b;
    QUrl m_sourceA, m_sourceB, m_difference;
    QSize m_sizeA, m_sizeB;
    QStringList m_summary;
    QString m_error;
    QString m_notice;
    QTemporaryDir m_folder;
};

} // namespace fugue
