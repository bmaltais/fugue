#pragma once

// What the run toolbar and its queue panel remember and answer to (#60): whether the toolbar is
// shown, which edge of the canvas it sits on, the batch count, and whether the queue panel is open.
// The toolbar and the panel are QML (src/run/RunToolbar.qml, RunQueuePanel.qml) that know
// nothing of this; Main.qml binds them to App.runBar, App.runs and App.runQueue.
//
// Kept in a small file of the state folder: {"visible", "dock": "top"|"bottom", "batch"}. The
// panel being open is not kept: a window starts with it closed.
//
// Commands: view.toggle-run-toolbar (shown, and its check in the View menu) and panel.queue (Q,
// the queue panel, as ComfyUI's `q`). The toolbar's own buttons call the run.* commands.

#include <QObject>
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
    // "top" or "bottom": the edge of the canvas the toolbar floats at, centered.
    Q_PROPERTY(QString dock READ dock WRITE setDock NOTIFY dockChanged)
    Q_PROPERTY(bool queueOpen READ queueOpen WRITE setQueueOpen NOTIFY queueOpenChanged)

public:
    // `path` is the file it is kept in; the batch count is `runs`' (set from the file, saved when
    // it changes).
    RunBar(CanvasRuns *runs, QString path, QObject *parent = nullptr);

    bool visible() const { return m_visible; }
    QString dock() const { return m_dock; }
    bool queueOpen() const { return m_queueOpen; }
    void setVisible(bool visible);
    void setDock(const QString &dock); // anything but "bottom" is "top"
    void setQueueOpen(bool open);

    void registerCommands(CommandRegistry &commands);

signals:
    // The queue panel was opened by its command (keys): it takes the focus, so the keys reach it.
    void queueFocusRequested();
    void visibleChanged();
    void dockChanged();
    void queueOpenChanged();

private:
    void save() const;

    CanvasRuns *m_runs;
    QString m_path;
    bool m_visible = true;
    QString m_dock = QStringLiteral("top");
    bool m_queueOpen = false;
};

} // namespace fugue
