#pragma once

// What the run toolbar and its queue panel remember and answer to (#60): whether the toolbar is
// shown, where on the canvas it sits, the batch count, and whether the queue panel is open.
// The toolbar and the panel are QML (src/run/RunToolbar.qml, RunQueuePanel.qml) that know
// nothing of this; Main.qml binds them to App.runBar, App.runs and App.runQueue.
//
// Kept in a small file of the state folder: {"visible", "place": {"x", "y"}, "batch"}. The place is
// the bar's, as fractions (0 to 1) of the room it has in the window (src/engine/float_place.h), so a
// resized window keeps it sensible: {0.5, 0} is centered at the top, {0.5, 1} at the bottom. An
// older file has "dock": "top"|"bottom" instead, read as those two places and dropped at the next
// save. The panel being open is not kept: a window starts with it closed.
//
// Commands: view.toggle-run-toolbar (shown, and its check in the View menu) and panel.queue (Q,
// the queue panel, as ComfyUI's `q`). The toolbar's own buttons call the run.* commands.

#include <QObject>
#include <QPointF>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class CanvasRuns;
class CommandRegistry;

class RunBar : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(bool visible READ visible WRITE setVisible NOTIFY visibleChanged)
    // Where the toolbar floats, as fractions (0 to 1) of the room it has in the canvas.
    Q_PROPERTY(QPointF place READ place WRITE setPlace NOTIFY placeChanged)
    Q_PROPERTY(bool queueOpen READ queueOpen WRITE setQueueOpen NOTIFY queueOpenChanged)

public:
    // `path` is the file it is kept in; the batch count is `runs`' (set from the file, saved when
    // it changes).
    RunBar(CanvasRuns *runs, QString path, QObject *parent = nullptr);

    bool visible() const { return m_visible; }
    QPointF place() const { return m_place; }
    bool queueOpen() const { return m_queueOpen; }
    void setVisible(bool visible);
    void setPlace(const QPointF &place); // held to 0..1 on each axis
    void setQueueOpen(bool open);

    void registerCommands(CommandRegistry &commands);

signals:
    // The queue panel was opened by its command (keys): it takes the focus, so the keys reach it.
    void queueFocusRequested();
    void visibleChanged();
    void placeChanged();
    void queueOpenChanged();

private:
    void save() const;

    CanvasRuns *m_runs;
    QString m_path;
    bool m_visible = true;
    QPointF m_place{0.5, 0};
    bool m_queueOpen = false;
};

} // namespace fugue
