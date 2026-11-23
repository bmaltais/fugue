#include "menu/menus.h"

#include <QCoreApplication>
#include <QVariantMap>

namespace fugue::menus {

namespace {

QVariantMap command(const char *id) { return {{"command", QString::fromLatin1(id)}}; }
// A context menu's command that is left out where it does not apply (a group's on a plain node,
// an image's on a node with none), rather than shown off (#72).
QVariantMap ifOn(const char *id) { return {{"command", QString::fromLatin1(id)}, {"hideWhenOff", true}}; }
QVariantMap separator() { return {{"separator", true}}; }
// A feature that is planned and not built: a greyed row, never a command (no handler, no key), whose tooltip says which
// issue builds it (#232, docs/canvas-menus-research.md).
QVariantMap planned(const char *title, int issue) {
    return {{"placeholder", QString::fromUtf8(title)}, {"issue", issue}};
}
QVariantMap danger(const char *id) { return {{"command", QString::fromLatin1(id)}, {"danger", true}}; }
QVariantMap submenu(const char *title, const QVariantList &items) {
    return {{"title", QString::fromUtf8(title)}, {"items", items}};
}

QVariantMap colors() {
    QVariantList items;
    for (const QString &role : colorRoles())
        items.append(QVariantMap{{"command", QStringLiteral("node.color.") + role}, {"swatch", role}});
    // The color wheel (#52), after the roles, in a chunk of its own.
    items.append(QVariantMap{{"separator", true}});
    items.append(QVariantMap{{"command", QStringLiteral("node.color.custom")}});
    return {{"title", QStringLiteral(QT_TRANSLATE_NOOP("Menu", "&Colors"))}, {"items", items}, {"swatches", true}};
}

// Align and Distribute (#58), after the toggles in the Node menu and the node's menu. In the
// node's menu a command that does not apply is left out, and a submenu left empty with it.
QVariantMap arrangeMenu(const char *title, std::initializer_list<const char *> ids, bool hideWhenOff) {
    QVariantList items;
    for (const char *id : ids)
        items.append(hideWhenOff ? ifOn(id) : command(id));
    return {{"title", QString::fromUtf8(title)}, {"items", items}};
}

QVariantMap align(bool hideWhenOff) {
    return arrangeMenu(QT_TRANSLATE_NOOP("Menu", "&Align"),
                       {"node.align-left", "node.align-right", "node.align-top", "node.align-bottom"}, hideWhenOff);
}

QVariantMap distribute(bool hideWhenOff) {
    return arrangeMenu(QT_TRANSLATE_NOOP("Menu", "&Distribute"),
                       {"node.distribute-horizontal", "node.distribute-vertical"}, hideWhenOff);
}

// The picture mode (#263), a choice of three with a check on the one in force.
QVariantMap pictureMenu() {
    return submenu(QT_TRANSLATE_NOOP("Menu", "&Picture"),
                   {command("view.picture-smart"), command("view.picture-sinks"), command("view.picture-all")});
}

// The Run menu, and the menu behind the run toolbar's split button (#60): the same rows.
QVariantList runItems() {
    return {command("run.start"), command("run.start-first"), command("run.to-here"), separator(), command("run.cancel")};
}

// The Bars and panels submenu (#129), as Photoshop's Window menu: every bar and panel with a check
// while it is shown, then focus mode and the way back. In the View menu, and a submenu of the canvas's
// menu too, so a hidden menu bar can be found again with a right click. (A top menu of its own
// would add items to the window, which keeps a budget.)
QVariantList windowItems() {
    return {command("view.toggle-menu-bar"), command("view.toggle-tabs"), command("view.toggle-run-toolbar"),
            command("view.toggle-selection-toolbar"), command("view.toggle-breadcrumb"), command("view.toggle-run-status"),
            command("view.toggle-zoom-readout"), separator(),
            command("panel.nodes"), command("panel.workflows"), command("panel.queue"), separator(),
            command("view.focus-mode"), command("view.reset-bars")};
}

QVariantMap barsMenu() {
    return submenu(QT_TRANSLATE_NOOP("Menu", "&Bars and panels"), windowItems());
}

// The window's areas (#217, spec 12.16): split, close, the view the active area shows, reset. The tail of the Window menu
// (#262), after the bars and panels.
QVariantList areaItems() {
    return {command("area.split-row"), command("area.split-column"), command("area.close"), separator(),
            command("area.view-graph"), command("area.view-canvas"), separator(), command("view.reset-layout")};
}

// The Window menu (#262, Blender's): the window's own furniture, not a view's. Every bar and panel with a check, focus mode
// and the way back, then the areas.
QVariantList windowMenu() {
    QVariantList items = windowItems();
    items.append(separator());
    for (const QVariant &item : areaItems())
        items.append(item);
    return items;
}

// The window bar (#262, Blender's top bar): what is global to the window and its documents. Each view's own menus are in
// its area's header (graph() and canvasView()).
QVariantList bar() {
    return {
        submenu(QT_TRANSLATE_NOOP("Menu", "&File"),
                {command("file.new"), command("file.open"), command("file.open-clipboard"), command("file.templates"), command("file.projects"), command("canvas.new"), command("canvas.paste-new"), command("file.canvas-view"), command("file.canvas-history"), separator(),
                 command("file.save"), command("file.save-as"), command("file.rename"), command("file.duplicate"),
                 command("file.export-share"), command("file.export-compiled"), separator(), command("file.close"),
                 command("file.close-others"), command("app.quit")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Edit"),
                {command("edit.undo"), command("edit.redo"), separator(), command("edit.copy"), command("edit.paste"),
                 separator(), command("edit.clear"), separator(), command("cache.clear-previews"), separator(),
                 command("app.settings")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Run"), runItems()),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Window"), windowMenu()),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Help"),
                {command("app.shortcuts"), command("help.docs"), command("help.report-issue"), separator(),
                 command("help.about")}),
    };
}

// The Graph view's own menus (#262), in the header of the area that shows it, as Blender's Node Editor has View, Select, Add and
// Node. Run is not here: it is the window's (Blender's Render menu).
QVariantList graph() {
    return {
        submenu(QT_TRANSLATE_NOOP("Menu", "&View"),
                {command("view.fit"), command("view.zoom-in"), command("view.zoom-out"), separator(),
                 command("view.toggle-links"), pictureMenu()}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Select"), {command("edit.select-all")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Add"), {command("node.search"), command("edit.paste-connected")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Node"),
                {command("edit.rename"), colors(), command("edit.clone"), command("edit.delete"), separator(),
                 command("node.bypass"), command("node.mute"), command("node.collapse"), command("node.pin"), ifOn("node.picture-show"),
                 ifOn("node.picture-hide"), ifOn("node.picture-default"), align(false),
                 distribute(false), separator(), command("node.open-image"), command("node.copy-image"),
                 command("node.save-image"), separator(), command("frame.make"), command("group.make"),
                 command("group.ungroup"), command("group.enter"), command("group.leave"), command("group.make-unique"),
                 command("group.interface"), command("group.save-to-library"), command("group.update"),
                 command("node.move-version")}),
    };
}

// The Canvas view's own menu bar (#232 slice B), hosted in the view (CanvasView.qml): its File, Edit, Select, Layer and
// View menus. Every toolbar and tool-strip option of the view is a row here; rows of features not built yet are
// `planned` placeholders.
QVariantList canvasView() {
    return {
        submenu(QT_TRANSLATE_NOOP("Menu", "&Canvas"),
                {command("canvas-view.new"), command("canvas-view.open"), command("canvas-view.import"), separator(),
                 submenu(QT_TRANSLATE_NOOP("Menu", "&Export"),
                         {command("canvas-view.export"), separator(),
                          command("canvas-view.export-options"), command("canvas-view.export-layers"),
                          planned(QT_TRANSLATE_NOOP("Menu", "Export PSD…"), 227)}),
                 planned(QT_TRANSLATE_NOOP("Menu", "Place picture…"), 236),
                 command("canvas-view.share"),
                 planned(QT_TRANSLATE_NOOP("Menu", "Run an action on a folder…"), 250), separator(),
                 command("canvas-view.rename"), command("canvas-view.duplicate"),
                 planned(QT_TRANSLATE_NOOP("Menu", "Canvas size…"), 240),
                 planned(QT_TRANSLATE_NOOP("Menu", "Edit in an external editor…"), 215), separator(),
                 command("canvas-view.clear"), danger("canvas-view.delete"), separator(), command("canvas-view.close")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Edit"),
                {command("canvas-view.undo"), command("canvas-view.redo"), separator(),
                 planned(QT_TRANSLATE_NOOP("Menu", "Copy"), 236), planned(QT_TRANSLATE_NOOP("Menu", "Paste as layer"), 236),
                 planned(QT_TRANSLATE_NOOP("Menu", "Free transform"), 236)}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Select"),
                {command("canvas-view.select-all"), command("canvas-view.deselect"), command("canvas-view.invert-selection"),
                 separator(), command("canvas-view.quick-mask"), command("canvas-view.use-selection"), separator(),
                 planned(QT_TRANSLATE_NOOP("Menu", "Select subject…"), 238),
                 planned(QT_TRANSLATE_NOOP("Menu", "Modify selection…"), 251)}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Layer"),
                {command("canvas-view.add-mask"), command("canvas-view.mask-invert"), command("canvas-view.mask-overlay"),
                 command("canvas-view.mask-reveal"), command("canvas-view.mask-hide"), separator(),
                 command("canvas-view.new-layer"), command("canvas-view.group"), command("canvas-view.ungroup"), separator(),
                 command("canvas-view.merge-down"), command("canvas-view.flatten"), separator(), command("canvas-view.layer-properties")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&View"),
                {command("canvas-view.fit"), command("canvas-view.actual-size"), command("canvas-view.zoom-in"),
                 command("canvas-view.zoom-out"), separator(), command("canvas-view.history"), command("canvas-view.layers"),
                 command("canvas-view.compare"), command("canvas-view.details"),
                 command("canvas-view.snap"), separator(),
                 submenu(QT_TRANSLATE_NOOP("Menu", "&Tools"),
                         {command("canvas-view.tool-hand"), command("canvas-view.tool-zoom"), command("canvas-view.tool-region"),
                          command("canvas-view.tool-brush"), command("canvas-view.tool-eraser"), command("canvas-view.brush-swap"),
                          command("canvas-view.brush-smaller"), command("canvas-view.brush-larger")}),
                 separator(), planned(QT_TRANSLATE_NOOP("Menu", "Rulers, grid and guides"), 253)}),
    };
}

// Right-click on a workflow tab (#59): the tab is made current first, so each command acts on it.
QVariantList tab() {
    return {command("file.rename"), command("file.duplicate"), command("file.tab-color"), separator(),
            command("file.save"), command("file.save-as"), command("file.export-share"), separator(),
            command("file.close"), command("file.close-others")};
}

// Right-click on empty canvas: what adds to it, then the view.
QVariantList canvas() {
    return {command("node.search"), command("edit.paste"), command("edit.paste-connected"), separator(),
            command("edit.select-all"), command("view.fit"), command("view.toggle-links"),
            barsMenu(), separator(),
            command("frame.make"), command("group.make"), ifOn("group.leave"), ifOn("group.interface"),
            ifOn("group.save-to-library")};
}

// Right-click on a node: it is selected first, so each command acts on the selection.
QVariantList node() {
    return {ifOn("group.expose"), ifOn("group.unexpose"), command("edit.rename"), separator(), command("node.bypass"),
            command("node.mute"), command("node.collapse"), command("node.pin"), ifOn("node.picture-show"), ifOn("node.picture-hide"),
            ifOn("node.picture-default"), colors(), align(true), distribute(true), separator(),
            ifOn("node.open-image"), ifOn("node.copy-image"), ifOn("node.save-image"), ifOn("compare.copy-a"), ifOn("compare.copy-b"),
            ifOn("compare.copy-view"), separator(),
            command("edit.copy"),
            command("edit.clone"), separator(), command("frame.make"), command("group.make"),
            ifOn("group.ungroup"), ifOn("group.enter"), ifOn("group.interface"), ifOn("group.make-unique"),
            ifOn("group.save-to-library"), ifOn("group.update"), ifOn("node.move-version"), separator(),
            command("run.to-here"), separator(),
            command("edit.delete")};
}

// Right-click on a Frame (spec 5.1); its colors apply to that Frame.
QVariantList frame() {
    return {command("frame.rename"), colors(), command("frame.delete"), separator(), command("node.search"),
            command("edit.paste")};
}

} // namespace

QStringList names() {
    return {QStringLiteral("bar"), QStringLiteral("graph"), QStringLiteral("canvas"), QStringLiteral("node"), QStringLiteral("frame"),
            QStringLiteral("tab"), QStringLiteral("colors"), QStringLiteral("run"), QStringLiteral("canvas-view")};
}

QVariantList tree(const QString &name) {
    if (name == QLatin1String("bar"))
        return bar();
    if (name == QLatin1String("graph"))
        return graph();
    if (name == QLatin1String("canvas"))
        return canvas();
    if (name == QLatin1String("node"))
        return node();
    if (name == QLatin1String("frame"))
        return frame();
    if (name == QLatin1String("tab"))
        return tab();
    if (name == QLatin1String("canvas-view")) // the Canvas view's own menu bar (#232)
        return canvasView();
    if (name == QLatin1String("run")) // the run toolbar's split button (#60)
        return runItems();
    if (name == QLatin1String("colors")) // the selection toolbar's color button (#74)
        return colors().value("items").toList();
    return {};
}

QStringList colorRoles() {
    return {QStringLiteral("none"),   QStringLiteral("accent"), QStringLiteral("muted"),
            QStringLiteral("red"),    QStringLiteral("orange"), QStringLiteral("yellow"),
            QStringLiteral("green"),  QStringLiteral("cyan"),   QStringLiteral("blue"),
            QStringLiteral("magenta")};
}

} // namespace fugue::menus
