#pragma once

// What the side panels (#56, ComfyUI's sidebar tabs) remember and answer to: which one is open
// (none, the node library or the workflows panel), on which side of the canvas it docks, and how
// wide it is. One panel is open at a time, as ComfyUI's sidebar shows one tab at a time; opening
// the other replaces it. The dock container (src/panels/SidePanel.qml) and the two panels'
// contents (NodeLibraryPanel.qml, WorkflowsPanel.qml) are QML that know nothing of this;
// Main.qml binds them to App.sidePanels.
//
// Which panel is open is the visibility registry's (ids "node-library" and "workflows-panel", one of
// them at a time; panels/chrome_visibility.h), so focus mode hides the panel and brings it back and
// the registry's file remembers it: a window starts with the panel it closed with. A "tab" in this
// file from before is read once, when the registry has nothing for the panels.
//
// Kept in a small file of the state folder: {"dock": "left"|"right", "width"}.
//
// Commands: panel.nodes (N) and panel.workflows (W), each toggling its panel and checked in the
// View menu while it is the open one.

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

class ChromeVisibility;
class CommandRegistry;

class SidePanels : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // "", "nodes" or "workflows".
    Q_PROPERTY(QString tab READ tab WRITE setTab NOTIFY tabChanged)
    // "left" or "right": the side of the window it sits on (the canvas never mirrors, so neither does this).
    Q_PROPERTY(QString dock READ dock WRITE setDock NOTIFY dockChanged)
    Q_PROPERTY(int width READ width WRITE setWidth NOTIFY widthChanged)
    Q_PROPERTY(int minWidth READ minWidth CONSTANT)
    Q_PROPERTY(int maxWidth READ maxWidth CONSTANT)

public:
    static constexpr int kMinWidth = 200;
    static constexpr int kMaxWidth = 640;
    static constexpr int kDefaultWidth = 288;

    // `path` is the file it is kept in (empty: not kept). `chrome` is the registry the panels
    // register with; none: a registry of its own, which keeps nothing.
    explicit SidePanels(QString path, ChromeVisibility *chrome = nullptr);

    QString tab() const; // "" while none is shown (focus mode included)
    QString dock() const { return m_dock; }
    int width() const { return m_width; }
    int minWidth() const { return kMinWidth; }
    int maxWidth() const { return kMaxWidth; }
    void setTab(const QString &tab); // anything but "nodes" and "workflows" is none
    void setDock(const QString &dock); // anything but "right" is "left"
    void setWidth(int width);          // kept between kMinWidth and kMaxWidth

    // Opens `tab`, or closes it when it is the open one. Opening asks for the focus.
    Q_INVOKABLE void toggle(const QString &tab);

    void registerCommands(CommandRegistry &commands);

signals:
    // The panel was opened by its command (keys): it takes the focus, so the keys reach it.
    void focusRequested();
    void tabChanged();
    void dockChanged();
    void widthChanged();

private:
    void save() const;

    QString m_path;
    ChromeVisibility *m_chrome;
    QString m_shownTab;
    QString m_dock = QStringLiteral("left");
    int m_width = kDefaultWidth;
};

} // namespace fugue
