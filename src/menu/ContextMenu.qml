pragma ComponentBehavior: Bound
import QtQuick
import Fugue
import "../canvas"

// The canvas's context menus (#50, spec 12.3: right-click opens it): on a node the "node"
// menu, on a Frame the "frame" one, elsewhere the "canvas" one, from the command registry's
// trees (menus.h). A right-click on a node outside the selection selects it first, as in
// ComfyUI, so every command acts on the selection; inside the selection it is kept. The
// menu's commands see what it was opened on (CommandRegistry::target): {frame: id} on a
// Frame, so its colors apply there, and the world point, where Add node opens search.
// Shift+F10 or the Menu key (app.context-menu) opens it from the keys: on the selection
// when there is one, else at the pointer.
Item {
    id: root
    objectName: "contextMenu"

    required property FugueCanvas canvas
    property string kind: ""     // canvas, node or frame, the last one opened
    // Made from the file the first time it opens, so opening the window does not compile Menu's QML.
    property var menu: null

    // The topmost Frame under a world point, or "".
    function frameAt(x, y) {
        const frames = App.canvasModel.frames
        for (let i = frames.length - 1; i >= 0; --i) {
            const f = frames[i]
            if (x >= f.x && x <= f.x + f.width && y >= f.y && y <= f.y + f.height)
                return f.id
        }
        return ""
    }

    // Opens the menu `kind` (menus.h) at the canvas point (px, py) for the selection, as the
    // selection toolbar's "…" and color buttons do (#74).
    function openMenu(kind, px, py) {
        if (!root.menu)
            root.menu = Qt.createComponent("CommandMenu.qml").createObject(root, {objectName: "contextMenuPopup"})
        const menu = root.menu
        const selection = App.canvasModel.selection
        const target = selection.length > 0 ? {node: selection[0]} : {}
        root.kind = kind === "colors" ? "node" : kind
        menu.close()
        menu.target = target
        menu.items = App.commands.menu(kind, target)
        const p = canvas.mapToItem(root, px, py)
        menu.byKey = false
        menu.popupFit(root, p.x, p.y, 0, 0)
    }

    // Opens the menu for what is at the canvas point (px, py).
    function openAt(px, py, byKey) {
        if (!root.menu)
            root.menu = Qt.createComponent("CommandMenu.qml").createObject(root, {objectName: "contextMenuPopup"})
        const menu = root.menu
        const w = canvas.toWorld(px, py)
        const node = App.canvasModel.nodeAt(w.x, w.y)
        const target = {x: w.x, y: w.y}
        // On a port: the commands that act on one (expose it on the group, 5.4) see which.
        const port = byKey ? ({}) : canvas.portUnder(px, py)
        if (port.node !== undefined) {
            target.port = port.port
            target.output = port.output
        }
        if (node !== "" || port.node !== undefined) {
            const hit = node !== "" ? node : port.node
            if (App.canvasModel.selection.indexOf(hit) < 0)
                App.canvasModel.select([hit], "replace")
            kind = "node"
            target.node = hit
        } else {
            const frame = frameAt(w.x, w.y)
            if (frame !== "")
                target.frame = frame
            kind = frame !== "" ? "frame" : "canvas"
        }
        menu.close()
        menu.target = target
        menu.items = App.commands.menu(kind, target)
        const p = canvas.mapToItem(root, px, py)
        menu.byKey = byKey
        menu.popupFit(root, p.x, p.y, 0, 0)
    }

    Connections {
        target: App.commands
        function onWindowRequested(id) {
            if (id !== "app.context-menu")
                return
            const selection = App.canvasModel.selection
            let p
            if (selection.length > 0) {
                // Just inside the first selected node's header.
                const n = App.canvasModel.position(selection[0])
                p = Qt.point(root.canvas.panX + (n.x + 8) * root.canvas.zoom,
                             root.canvas.panY + (n.y + 8) * root.canvas.zoom)
            } else {
                const w = root.canvas.pointerWorld()
                p = Qt.point(root.canvas.panX + w.x * root.canvas.zoom, root.canvas.panY + w.y * root.canvas.zoom)
            }
            root.openAt(p.x, p.y, true)
        }
    }
}
