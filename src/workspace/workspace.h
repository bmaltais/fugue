#pragma once

// The window's layout of areas (#217 slice 1, spec 12.16): the area tree it shows, the area that is active, the
// commands that split, close and change areas, and the file that keeps the layout. Plain rules over
// workspace/area_tree.h and workspace/view_registry.h, with no window: tests/workspace/test_workspace.cpp drives it
// alone, and Main.qml draws `tree` (AreaNode.qml) and calls back here.
//
// Any area may show any registered view, the Graph's too (Blender's editor-type menu). The Graph and the App are one per
// window: choosing one in another area moves it there (the two areas trade what they show). The Graph may have no area
// (graphArea() is 0): it is then hidden but alive, its commands are off and the window's Main.qml draws it nowhere; closing
// its area just closes it, and choosing Graph in any area (Ctrl+Alt+G) brings it back there. A split's new area shows the
// view of the area split when that view may be shown twice, else the first registered view that may (the Canvas).
//
// Kept in `workspaces.json` in the root's state folder (engine/paths.h stateDir), format 1 of AreaTree::toJson,
// written whole (engine/atomic_file.h) a moment after a change, so a drag is one write, and at quit. A layout of
// one Graph area is the default window: it is never written, and reaching it again removes the file, so the
// default window reads nothing and stays what it was. A file that does not read (not JSON, not format 1, a tree
// that does not validate) gives the default layout, is kept beside as `workspaces.json.bad`, and the window's
// notice line says so once. A layout without the Graph stays without it; a second Graph area shows a Canvas.
//
// The Queue is a view of its own (#648, spec 12.16): one per window, like the Graph. panel.queue (Alt+Q, context `app`) toggles it:
// with no Queue area it splits the active area along its bottom and shows the Queue there (a share of kQueueShare of the area,
// never under the Queue's minimum), refused with a message when the area is too small; with one it closes that area (the only
// area, if it is the Queue, shows the Graph again). Closing it remembers its place (axis, side, share of its and its neighbour's
// room, the neighbour's area id); the next press restores that when the neighbour is still there and the tree fits, else the bottom split. The keyboard goes to the new Queue and it becomes the active area, except
// when the area split shows the App (its keys follow the active area): see queueOpened.
//
// Commands (context `app`): area.split-row (Ctrl+\), area.split-column (Ctrl+Alt+\), area.close (Ctrl+Alt+W),
// area.view-graph (Ctrl+Alt+G), area.view-canvas (Ctrl+Alt+C), each on the active area, and view.reset-layout.

#include <QJsonObject>
#include <QObject>
#include <QSizeF>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "workspace/area_tree.h"
#include "workspace/view_registry.h"

namespace fugue {

class CommandRegistry;

// Registers the area commands' specs, without handlers (registerMenuCommands calls it, so every menu item is a
// registered command); a command already registered is left as it is.
void addAreaCommands(CommandRegistry &commands);

class Workspace : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    // The tree, as AreaTree::toJson writes it: {"format": 1, "root": NODE}.
    Q_PROPERTY(QVariantMap tree READ tree NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    // The area the commands act on: the one last pressed or given the keyboard focus.
    Q_PROPERTY(int active READ active WRITE setActive NOTIFY activeChanged)
    // The area showing the Graph, or 0 when none does (the Graph is hidden).
    Q_PROPERTY(int graphArea READ graphArea NOTIFY changed)
    // The area showing the Queue, or 0 when none does.
    Q_PROPERTY(int queueArea READ queueArea NOTIFY changed)
    Q_PROPERTY(fugue::ViewRegistry *views READ views CONSTANT)
    // The size the window gives the areas, set by the window: a split that would not fit it is refused. Empty
    // (no window): every split fits.
    Q_PROPERTY(QSizeF bodySize READ bodySize WRITE setBodySize NOTIFY bodySizeChanged)

public:
    static constexpr int kSaveDelayMs = 400;
    static constexpr qint64 kMaxFileSize = 1 << 20;
    // How much of the active area's height panel.queue gives the Queue.
    static constexpr double kQueueShare = 0.3;

    // `path` is the file the layout is kept in (empty: not kept).
    explicit Workspace(QString path = {}, QObject *parent = nullptr);
    ~Workspace() override; // writes a change still waiting

    // Reads the kept layout, once at start (main.cpp, after the notice line is connected).
    void restore();
    // Writes a change still waiting now (at quit).
    void flush();
    QString path() const { return m_path; }

    const AreaTree &areaTree() const { return m_tree; }
    QVariantMap tree() const;
    int count() const { return m_tree.count(); }
    int active() const { return m_active; }
    void setActive(int id);
    int graphArea() const;
    int queueArea() const { return areaOf(ViewRegistry::kQueue); }
    ViewRegistry *views() { return &m_views; }
    QSizeF bodySize() const { return m_bodySize; }
    void setBodySize(const QSizeF &size);

    // Splits area `id` along "row" (side by side) or "column" (top and bottom); the new area comes after it and
    // becomes the active one. Its id, or 0 when refused (see canSplit).
    Q_INVOKABLE int splitArea(int id, const QString &axis);
    // False for an unknown area or axis, kMaxAreas reached, no view to show twice, or a tree that would not fit
    // bodySize.
    Q_INVOKABLE bool canSplit(int id, const QString &axis) const;
    // Closes area `id`; the area that takes its place becomes the active one. False for the only area.
    Q_INVOKABLE bool closeArea(int id);
    // Area `id` shows `view` now (the Graph moves here from its area, which shows what this one did). False when
    // it cannot (canShow) or already does.
    Q_INVOKABLE bool showView(int id, const QString &view);
    // True when area `id` may show `view`: a registered view.
    Q_INVOKABLE bool canShow(int id, const QString &view) const;
    // The view-type menu of area `id`: one map per registered type, in registry order: id, name (translated),
    // glyph, current, enabled, moves (it is shown in another area and moves here).
    Q_INVOKABLE QVariantList viewChoices(int id) const;
    Q_INVOKABLE QString viewOf(int id) const;
    // The area showing `view` (the first), or 0.
    Q_INVOKABLE int areaOf(const QString &view) const;
    // view.toggle-app in a split (#450): the active area shows the other of the App and the Graph. An area showing the App takes the
    // Graph (the two areas trade); the Graph's area takes the App from its area, or, with none, gives it to the next area; any other
    // area shows the App. False when nothing changed.
    Q_INVOKABLE bool toggleApp();
    // The App view, wanted shown (true) or the graph wanted (false), for what asks for one view (the builder, the shelf, Show node):
    // the App is brought to the active area (or the next one when that is the Graph's) unless some area shows it; the Graph is brought
    // to the active area when that shows the App. False when nothing changed.
    Q_INVOKABLE bool requestApp(bool app);
    // panel.queue: closes the Queue's area, or splits the active area along its bottom and shows the Queue in the new part. False when
    // nothing changed; a split that does not fit says so on `message`.
    Q_INVOKABLE bool toggleQueue();
    // Moves the border after child `border` of the branch at `path` (child indexes from the root) by `delta`
    // pixels along a branch `length` pixels long, each side kept at its views' minimum.
    Q_INVOKABLE bool resizeBorder(const QVariantList &path, int border, double delta, double length);
    // The two children beside that border get equal shares.
    Q_INVOKABLE bool equalizeBorder(const QVariantList &path, int border);
    // The state of area `id`'s current view (a Canvas area's {"canvasId"}); kept with the layout.
    Q_INVOKABLE bool setAreaState(int id, const QVariantMap &state);
    Q_INVOKABLE QVariantMap areaState(int id) const;
    // Back to the default layout: one area showing the Graph; the file goes.
    Q_INVOKABLE void reset();

    // The commands' specs (addAreaCommands) and their handlers, each on the active area.
    void registerCommands(CommandRegistry &commands);

signals:
    void changed();
    void activeChanged();
    void bodySizeChanged();
    // panel.queue made the Queue's area `area`; `focus`: the keyboard goes to it (it became the active area), false when the area it
    // was split from shows the App, which stays the active one. The window gives the Queue's view the focus.
    void queueOpened(int area, bool focus);
    // A line for the window's notice line (a layout that could not be read).
    void message(const QString &text);

private:
    AreaTree::MinimumSize minimum() const;
    QString newViewFor(int id) const;
    void commit(const AreaTree &next);
    void keepGraph(AreaTree &tree) const;
    // The Graph's area, else the first area: what the active area falls back to.
    int fallbackArea() const;
    void save();
    void rememberQueue(int queue);
    bool openQueueWhereItWas(int *made);

    QString m_path;
    ViewRegistry m_views;
    AreaTree m_tree;
    int m_active = 1;
    QSizeF m_bodySize;
    QTimer m_saveTimer;
    // Where the Queue was when panel.queue last closed it (queuePlace in the layout file, spec 12.16): {axis, end, share, from}, or empty.
    QJsonObject m_queuePlace;
};

} // namespace fugue
