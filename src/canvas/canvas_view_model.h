#pragma once

// The backend of the Canvas view (#221, epics #216 and #217, spec 14.5): the picture of one canvas at a revision you
// choose, built to live in an area of the workspace as well as in the File > Canvas… dialog, so it takes a small
// context (the canvas, a follow target) and knows no dialog. It is CanvasHistory (canvas/canvas_history.h: the
// canvases, revisions, candidates, compare, accept, drop, export) plus what a view of the canvas needs:
//
//  - `choices`: the strip of what to look at, the head first, then the candidates waiting for review, then the
//    few latest revisions; `earlier` is the rest of the history, for a menu. `show(id)` picks one. A candidate
//    is compared with the HEAD (what accepting it would change), whatever it was made from.
//  - an overlay: with a candidate chosen, `overlay` puts it over the head picture; off, the head shows alone.
//    `pictureSource` and `pictureSize` are what the viewer draws, either way.
//  - a follow target: `followNode` is a Canvas Region node (read through WidgetEdit, as the region picker does) whose
//    rectangle `region` describes in canvas pixels, with the padded context box and which revision the node reads;
//    `useForRegion()` writes the shown revision to the node's `revision` as one undo step. Setting the target
//    moves the view to the node's canvas.
//  - auto-refresh: the store's folders are watched, so a head moved by a run, a candidate that arrived or a
//    canvas made elsewhere shows within a moment; a view on the head moves along with it.
// It makes no window. The `region` map is {valid, reason, x, y, width, height, context: {x, y, width, height}, reads,
// readsId, readsShown, writable, editable, padding, scale, cap, multiple}: `editable` is false while X, Y, Width or Height
// is linked to another node (the rectangle is then drawn but not edited).

#include <QFileSystemWatcher>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

#include "canvas/canvas_history.h"
#include "canvas/widget_edit.h"

namespace fugue {

class CanvasViewModel : public CanvasHistory {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(WidgetEdit *edits READ edits WRITE setEdits NOTIFY followChanged)
    Q_PROPERTY(QString followNode READ followNode WRITE setFollowNode NOTIFY followChanged)
    Q_PROPERTY(QVariantList choices READ choices NOTIFY choicesChanged)
    Q_PROPERTY(QVariantList earlier READ earlier NOTIFY choicesChanged)
    Q_PROPERTY(bool overlay READ overlay WRITE setOverlay NOTIFY pictureChanged)
    // True when the shown revision is a candidate (open or dropped): the overlay has something to toggle.
    Q_PROPERTY(bool showsCandidate READ showsCandidate NOTIFY compareChanged)
    Q_PROPERTY(QUrl headSource READ headSource NOTIFY pictureChanged)
    Q_PROPERTY(QUrl pictureSource READ pictureSource NOTIFY pictureChanged)
    Q_PROPERTY(QSize pictureSize READ pictureSize NOTIFY pictureChanged)
    Q_PROPERTY(QString head READ head NOTIFY choicesChanged)
    Q_PROPERTY(QVariantMap region READ region NOTIFY regionChanged)

public:
    explicit CanvasViewModel(QObject *parent = nullptr);

    // How many of the latest revisions the strip lists before the rest go to `earlier`.
    static constexpr int recentCount = 5;

    WidgetEdit *edits() const { return m_edits; }
    void setEdits(WidgetEdit *edits);
    QString followNode() const { return m_follow; }
    void setFollowNode(const QString &node);
    QVariantList choices() const { return m_choices; }
    QVariantList earlier() const { return m_earlier; }
    bool overlay() const { return m_overlay; }
    void setOverlay(bool on);
    bool showsCandidate() const;
    QUrl headSource() const { return m_headSource; }
    QUrl pictureSource() const;
    QSize pictureSize() const;
    QString head() const { return m_head; }
    QVariantMap region() const { return m_region; }

    // Looks at a revision: the head, a candidate (against the head) or any other (against its parent).
    Q_INVOKABLE void show(const QString &revision);
    // Writes the shown revision to the followed node's `revision` (the head as "head", so the node keeps reading the
    // head) as one undo step. False when nothing was written: no node, it reads this already, or the input is linked.
    Q_INVOKABLE bool useForRegion();
    // Editing the followed region on the picture (#224). `snapRegion` is the rectangle with its edges on the node's
    // Multiple of (`keepSize`: only the top left moves, as a ratio lock or a move needs); `previewRegion` the numbers a
    // rectangle means for the node, on the shown picture: {x, y, width, height, context: {x, y, width, height},
    // modelWidth, modelHeight, capped}, computed by the node's own functions. `setRegion` writes x, y, width and
    // height as ONE undo step (WidgetEdit::setRegion), snapped when `snap`; false when nothing was written (no valid
    // target, a number linked to another node, or the same numbers).
    Q_INVOKABLE QVariantMap snapRegion(const QVariantMap &rect, bool keepSize = false) const;
    Q_INVOKABLE QVariantMap previewRegion(const QVariantMap &rect) const;
    Q_INVOKABLE bool setRegion(const QVariantMap &rect, bool snap = false);
    // Reads the store again now (the watch does this on its own; a test and a person's refresh ask).
    Q_INVOKABLE void refresh();

signals:
    void followChanged();
    void choicesChanged();
    void pictureChanged();
    void regionChanged();

private:
    void rebuild();
    void updateRegion();
    void watch();

    WidgetEdit *m_edits = nullptr;
    QString m_follow;
    bool m_overlay = true;
    QVariantList m_choices, m_earlier;
    QUrl m_headSource;
    QSize m_headSize;
    QVariantMap m_region;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
    QMetaObject::Connection m_undoLink;
};

} // namespace fugue
