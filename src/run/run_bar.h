#pragma once

// What the run toolbar remembers (#60): whether it is shown, where on the canvas it sits and the
// batch count. The toolbar is QML (src/run/RunToolbar.qml) that knows nothing of this; Main.qml
// binds it to App.runBar, App.runs and App.runQueue. The queue is an area view (#648,
// workspace/QueueArea.qml), opened by panel.queue (workspace/workspace.h), not the toolbar's.
//
// Whether the toolbar is shown is the visibility registry's (id "run-toolbar", panels/chrome_visibility.h):
// the Bars and panels menu, focus mode and the file that remembers it are its. A "visible" in this
// file from before is read once, when the registry has nothing for the toolbar.
//
// Kept in a small file of the state folder: {"place": {"x", "y"}, "batch"}. The place is
// the bar's, as fractions (0 to 1) of the room it has in the window (src/engine/float_place.h), so a
// resized window keeps it sensible: {0.5, 0} is centered at the top, {0.5, 1} at the bottom. An
// older file has "dock": "top"|"bottom" instead, read as those two places and dropped at the next
// save.
//
// Command: view.toggle-run-toolbar (shown, and its check in the View menu; the registry's). The
// toolbar's own buttons call the run.* commands, and its queue button panel.queue.

#include <QObject>
#include <QPointF>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class CanvasRuns;
class ChromeVisibility;

class RunBar : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(bool visible READ visible WRITE setVisible NOTIFY visibleChanged)
    // Where the toolbar floats, as fractions (0 to 1) of the room it has in the canvas.
    Q_PROPERTY(QPointF place READ place WRITE setPlace NOTIFY placeChanged)

public:
    // `path` is the file it is kept in; the batch count is `runs`' (set from the file, saved when
    // it changes). `chrome` is the registry the toolbar registers with; none: a registry of its own,
    // which keeps nothing.
    RunBar(CanvasRuns *runs, QString path, ChromeVisibility *chrome = nullptr);

    static constexpr const char *kChromeId = "run-toolbar";

    bool visible() const;
    QPointF place() const { return m_place; }
    void setVisible(bool visible);
    void setPlace(const QPointF &place); // held to 0..1 on each axis

signals:
    void visibleChanged();
    void placeChanged();

private:
    void save() const;

    CanvasRuns *m_runs;
    QString m_path;
    ChromeVisibility *m_chrome;
    QPointF m_place{0.5, 0};
};

} // namespace fugue
