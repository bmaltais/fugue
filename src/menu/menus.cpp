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

QVariantList bar() {
    return {
        submenu(QT_TRANSLATE_NOOP("Menu", "&File"),
                {command("file.new"), command("file.open"), command("file.open-clipboard"), command("file.templates"), command("file.projects"), separator(),
                 command("file.save"), command("file.save-as"), command("file.rename"), command("file.duplicate"),
                 command("file.export-share"), command("file.export-compiled"), separator(), command("file.close"),
                 command("file.close-others"), command("app.quit")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Edit"),
                {command("edit.undo"), command("edit.redo"), separator(), command("edit.copy"), command("edit.paste"),
                 command("edit.paste-connected"), command("edit.clone"), command("edit.delete"), separator(),
                 command("edit.select-all"), command("edit.rename"), colors(), separator(), command("edit.clear"),
                 separator(), command("cache.clear-previews"), separator(), command("app.settings")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&View"),
                {command("view.fit"), command("view.zoom-in"), command("view.zoom-out"), separator(),
                 command("view.toggle-links"), separator(), barsMenu(), command("view.focus-mode")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Node"),
                {command("node.search"), separator(), command("node.bypass"), command("node.mute"),
                 command("node.collapse"), command("node.pin"), align(false), distribute(false),
                 separator(), command("node.open-image"), command("node.copy-image"), command("node.save-image"), separator(), command("frame.make"),
                 command("group.make"), command("group.ungroup"), command("group.enter"), command("group.leave"),
                 command("group.make-unique"), command("group.interface"), command("group.save-to-library"),
                 command("group.update"), command("node.move-version")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Run"), runItems()),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Help"),
                {command("app.shortcuts"), command("help.docs"), command("help.report-issue"), separator(),
                 command("help.about")}),
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
            command("node.mute"), command("node.collapse"), command("node.pin"), colors(), align(true), distribute(true), separator(),
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
    return {QStringLiteral("bar"), QStringLiteral("canvas"), QStringLiteral("node"), QStringLiteral("frame"),
            QStringLiteral("tab"), QStringLiteral("colors"), QStringLiteral("run")};
}

QVariantList tree(const QString &name) {
    if (name == QLatin1String("bar"))
        return bar();
    if (name == QLatin1String("canvas"))
        return canvas();
    if (name == QLatin1String("node"))
        return node();
    if (name == QLatin1String("frame"))
        return frame();
    if (name == QLatin1String("tab"))
        return tab();
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
