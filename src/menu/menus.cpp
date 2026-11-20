#include "menu/menus.h"

#include <QCoreApplication>
#include <QVariantMap>

namespace fugue::menus {

namespace {

QVariantMap command(const char *id) { return {{"command", QString::fromLatin1(id)}}; }
QVariantMap separator() { return {{"separator", true}}; }
QVariantMap submenu(const char *title, const QVariantList &items) {
    return {{"title", QString::fromUtf8(title)}, {"items", items}};
}

QVariantMap colors() {
    QVariantList items;
    for (const QString &role : colorRoles())
        items.append(QVariantMap{{"command", QStringLiteral("node.color.") + role}, {"swatch", role}});
    return {{"title", QStringLiteral(QT_TRANSLATE_NOOP("Menu", "&Colors"))}, {"items", items}, {"swatches", true}};
}

QVariantList bar() {
    return {
        submenu(QT_TRANSLATE_NOOP("Menu", "&File"),
                {command("file.new"), command("file.open"), command("file.open-clipboard"), separator(),
                 command("file.save"), command("file.save-as"), command("file.export-share"),
                 command("file.export-compiled"), separator(), command("file.close"), command("app.quit")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Edit"),
                {command("edit.undo"), command("edit.redo"), separator(), command("edit.copy"), command("edit.paste"),
                 command("edit.paste-connected"), command("edit.clone"), command("edit.delete"), separator(),
                 command("edit.select-all"), command("edit.rename"), colors(), separator(), command("edit.clear"),
                 separator(), command("app.settings")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&View"),
                {command("view.fit"), command("view.zoom-in"), command("view.zoom-out"), separator(),
                 command("view.toggle-links"), separator(), command("panel.nodes"), command("panel.workflows")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Node"),
                {command("node.search"), separator(), command("node.bypass"), command("node.mute"),
                 command("node.collapse"), command("node.pin"), separator(), command("node.open-image"),
                 command("node.copy-image"), command("node.save-image"), separator(), command("frame.make"),
                 command("group.make"), command("group.ungroup"), command("group.enter"), command("group.leave"),
                 command("group.make-unique"), command("group.interface"), command("group.save-to-library"),
                 command("group.update")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Run"),
                {command("run.start"), command("run.start-first"), command("run.to-here"), separator(),
                 command("run.cancel")}),
        submenu(QT_TRANSLATE_NOOP("Menu", "&Help"),
                {command("app.shortcuts"), command("help.docs"), command("help.report-issue"), separator(),
                 command("help.about")}),
    };
}

// Right-click on empty canvas: what adds to it, then the view.
QVariantList canvas() {
    return {command("node.search"), command("edit.paste"), command("edit.paste-connected"), separator(),
            command("edit.select-all"), command("view.fit"), command("view.toggle-links"), separator(),
            command("frame.make"), command("group.make"), command("group.leave"), command("group.interface"),
            command("group.save-to-library")};
}

// Right-click on a node: it is selected first, so each command acts on the selection.
QVariantList node() {
    return {command("group.expose"), command("group.unexpose"), command("edit.rename"), separator(), command("node.bypass"), command("node.mute"),
            command("node.collapse"), command("node.pin"), colors(), separator(), command("node.open-image"),
            command("node.copy-image"), command("node.save-image"), separator(), command("edit.copy"),
            command("edit.clone"), separator(), command("frame.make"), command("group.make"),
            command("group.ungroup"), command("group.enter"), command("group.make-unique"),
            command("group.save-to-library"), command("group.update"), separator(),
            command("run.to-here"), separator(),
            command("edit.delete")};
}

// Right-click on a Frame (spec 5.1); its colors apply to that Frame.
QVariantList frame() {
    return {command("frame.rename"), colors(), command("frame.delete"), separator(), command("node.search"),
            command("edit.paste")};
}

} // namespace

QStringList names() { return {QStringLiteral("bar"), QStringLiteral("canvas"), QStringLiteral("node"), QStringLiteral("frame")}; }

QVariantList tree(const QString &name) {
    if (name == QLatin1String("bar"))
        return bar();
    if (name == QLatin1String("canvas"))
        return canvas();
    if (name == QLatin1String("node"))
        return node();
    if (name == QLatin1String("frame"))
        return frame();
    return {};
}

QStringList colorRoles() {
    return {QStringLiteral("none"),   QStringLiteral("accent"), QStringLiteral("muted"),
            QStringLiteral("red"),    QStringLiteral("orange"), QStringLiteral("yellow"),
            QStringLiteral("green"),  QStringLiteral("cyan"),   QStringLiteral("blue"),
            QStringLiteral("magenta")};
}

} // namespace fugue::menus
