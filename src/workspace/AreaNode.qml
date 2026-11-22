pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// One node of an area tree (#217, spec 12.16; the rules are workspace/area_tree.h), drawn: a branch lays its
// children out by their shares, each in an AreaNode of its own, and a leaf shows its view.
//
// It is not an item. Every view goes straight into `host`, at the rectangle of its node, so an area adds no
// item of its own: the window with its one area has exactly the items it had before areas (the item budget,
// test_canvas_window), and a resize moves views without reparenting them. A node of the same shape (the same
// areas, views and axes: AreaLayout.shapeOf) only moves its views when it changes; another shape makes them again.
//
// A view comes from `views` when the window already has it (Main.qml declares the Graph, and its area adopts that
// item: never made, moved out of host or destroyed here), else it is made from the registry's component, or it is
// a placeholder (AreaPlaceholder.qml) for a type this build does not register. With no `host`, nothing is made or
// moved: the window's one area is the Graph where the window's layout puts it (slice 0). A split tree needs a host.
QtObject {
    id: areaNode

    // The node, as AreaTree::toJson writes it: {"type": "leaf", "area", "view", "state"} or
    // {"type": "branch", "axis", "children": [{"size", "node"}, ...]}.
    property var node: ({})
    property Item host: null
    // This node's place in `host`.
    property rect area: Qt.rect(0, 0, 0, 0)
    property ViewRegistry registry: null
    // Area id (as text) to the item that already is that area's view; it must be a child of `host`.
    property var views: ({})
    property bool rightToLeft: false

    readonly property bool leaf: node?.type === "leaf"
    readonly property int areaId: leaf ? node.area : 0
    readonly property string viewType: leaf ? node.view : ""
    // Read only: a leaf's view, adopted or made; a branch's child nodes.
    property Item view: null
    property bool adopted: false
    property var childNodes: []

    property string shape: ""
    property bool ready: false
    property Component placeholder: Component {
        AreaPlaceholder {}
    }

    // The view of area `id` in this node, or null.
    function viewOf(id: int): Item {
        if (leaf)
            return areaId === id ? view : null
        for (const child of childNodes) {
            const found = (child as AreaNode).viewOf(id)
            if (found)
                return found
        }
        return null
    }
    // The items this node and its children made (adopted views are not counted): none for the window's one area.
    function madeItems(): int {
        if (leaf)
            return view && !adopted ? 1 : 0
        let count = 0
        for (const child of childNodes)
            count += (child as AreaNode).madeItems()
        return count
    }

    function clear() {
        // The children's views first, here: a node's own destruction comes later, maybe with its context gone.
        for (const child of childNodes) {
            (child as AreaNode).clear()
            child.destroy()
        }
        childNodes = []
        if (view && !adopted)
            view.destroy()
        view = null
        adopted = false
    }
    // These read `node` itself: the bindings above may not have caught up with it inside onNodeChanged.
    function makeView() {
        const type = node.view
        const existing = views[String(node.area)] ?? null
        if (existing) {
            view = existing
            adopted = true
            return
        }
        if (!host)
            return
        let component = null
        if (registry && registry.contains(type)) {
            component = Qt.createComponent(registry.component(type))
            if (component.status === Component.Error) {
                console.warn(component.errorString())
                component = null
            }
        }
        view = component ? component.createObject(host) as Item
                         : placeholder.createObject(host, {viewType: type}) as Item
    }
    // A child's node as a plain object of its own: a part of this node read from a property is a reference into
    // it, which a new tree for this node would change under the child.
    function childNode(index: int): var {
        return JSON.parse(JSON.stringify(node.children[index].node))
    }
    function makeChildren() {
        const component = Qt.createComponent("AreaNode.qml")
        const made = []
        for (let i = 0; i < node.children.length; ++i)
            made.push(component.createObject(areaNode, {node: childNode(i), host: host, registry: registry, views: views,
                                                        rightToLeft: rightToLeft}))
        childNodes = made
    }
    function relayout() {
        if (!ready || !host)
            return
        if (node?.type === "leaf") {
            if (view) {
                view.x = area.x
                view.y = area.y
                view.width = area.width
                view.height = area.height
            }
            return
        }
        const rects = AreaLayout.childRects(node, area, registry, rightToLeft)
        for (let i = 0; i < childNodes.length && i < rects.length; ++i) {
            const child = childNodes[i] as AreaNode
            child.node = childNode(i)
            child.rightToLeft = rightToLeft
            child.area = rects[i]
        }
    }
    function rebuild() {
        if (!ready)
            return
        const next = AreaLayout.shapeOf(node ?? {})
        if (next !== shape || next === "") {
            clear()
            shape = next
            if (next === "") {
                if (node && Object.keys(node).length > 0)
                    console.warn("AreaNode: not an area tree node:", JSON.stringify(node))
            } else if (node.type === "leaf")
                makeView()
            else
                makeChildren()
        }
        relayout()
    }

    onNodeChanged: rebuild()
    onAreaChanged: relayout()
    onRightToLeftChanged: relayout()
    Component.onCompleted: {
        ready = true
        rebuild()
    }
    Component.onDestruction: clear()
}
