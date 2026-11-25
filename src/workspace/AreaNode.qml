pragma ComponentBehavior: Bound
import QtQuick
import Fugue

// One node of an area tree (#217, spec 12.16; the rules are workspace/area_tree.h), drawn: a branch lays its
// children out by their shares, each in an AreaNode of its own, and a leaf shows its view.
//
// It is not an item. Every view goes straight into `host`, at the rectangle of its node, so an area adds no
// item of its own: the window with its one area has exactly the items it had before areas (the item budget,
// test_canvas_window), and a resize moves views without reparenting them. A node of the same shape (the same
// areas, views and axes: AreaLayout.shapeOf) only moves its views when it changes; another shape makes the nodes
// again, and a view already made for the same area and view type is kept and moved (a split or a close leaves the
// other areas' views as they were: their zoom, their scroll).
//
// A view comes from `views` when the window already has it (Main.qml declares the Graph, and its area adopts that
// item: never made, moved out of host or destroyed here), else it is made from the registry's component, an
// AreaView (AreaView.qml: made with its area's id and the state its type has there, and what it says to keep goes
// to `controller.setAreaState`), or it is a placeholder (AreaPlaceholder.qml) for a type this build does not
// register. With no `host`, nothing is made or moved: the window's one area is the Graph where the window's layout
// puts it. A split tree needs a host.
//
// With a host, a leaf also gets a `header` (made with {areaId, viewType}; `headerHeight` tall at the top of the
// area, the view under it; hidden at 0) and a branch a `handle` per border between two children (AreaHandle.qml,
// centered on the border, across the whole branch), whose requests go to `controller.resizeBorder(path, border,
// delta, length)` and `controller.equalizeBorder(path, border)`. Views sit at z 0, handles at 1, headers at 2, so a
// header's buttons win over the handle that reaches into its ends.
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
    // What the handles and the views ask for (App.workspace in the window); none: nothing is asked.
    property var controller: null
    property Component header: null
    property real headerHeight: 0
    // How far the window's own furniture reaches into the top start corner of the areas (the auto-hide menu
    // bar's handle): the header of the area there keeps its menu clear of it.
    property real leadingClear: 0
    property Component handle: null
    // The outline of each card (AreaFrame.qml), made with the header; null: none.
    property Component frame: null
    // The gap of window background between two cards, in px (the Cards layout; 0 for Panes): each card keeps half of it off
    // every side that borders another area. A side on the host's own edge is left to the host's margin.
    property real gap: 0
    // This node's place in the tree: the child indexes from the root.
    property var path: []
    // The tree's root node (null: this one), which keeps the views a rebuild may use again.
    property AreaNode top: null
    // The root's: during a rebuild, the views made for the old shape, by "<area>:<view type>".
    property var spare: null

    readonly property bool leaf: node?.type === "leaf"
    readonly property int areaId: leaf ? node.area : 0
    readonly property string viewType: leaf ? node.view : ""
    // Read only: a leaf's view, adopted or made, and its header; a branch's child nodes and border handles.
    property Item view: null
    property bool adopted: false
    property string viewKey: ""
    property Item headerItem: null
    property Item frameItem: null
    property var handleItems: []
    property var childNodes: []

    property string shape: ""
    property bool ready: false
    property Component placeholder: Component {
        AreaPlaceholder {}
    }

    function topNode(): AreaNode {
        return top ?? areaNode
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
    // The area at a point of `host`, or 0.
    function areaAt(x: real, y: real): int {
        if (leaf)
            return x >= area.x && x < area.x + area.width && y >= area.y && y < area.y + area.height ? areaId : 0
        for (const child of childNodes) {
            const found = (child as AreaNode).areaAt(x, y)
            if (found)
                return found
        }
        return 0
    }
    // The area whose view or header `item` is, or 0.
    function areaOfItem(item: Item): int {
        if (leaf)
            return item !== null && (item === view || item === headerItem) ? areaId : 0
        for (const child of childNodes) {
            const found = (child as AreaNode).areaOfItem(item)
            if (found)
                return found
        }
        return 0
    }
    // The items this node and its children made (adopted views are not counted): none for the window's one area.
    function madeItems(): int {
        let count = (view && !adopted ? 1 : 0) + (headerItem ? 1 : 0) + (frameItem ? 1 : 0) + handleItems.length
        for (const child of childNodes)
            count += (child as AreaNode).madeItems()
        return count
    }

    // The views this node and its children show now.
    function shownViews(into: var) {
        if (view)
            into.push(view)
        for (const child of childNodes)
            (child as AreaNode).shownViews(into)
    }
    // The views made here and below, by "<area>:<view type>", taken out of the nodes so clear() keeps them.
    function harvest(into: var) {
        for (const child of childNodes)
            (child as AreaNode).harvest(into)
        if (view && !adopted && viewKey !== "") {
            into[viewKey] = view
            view = null
        }
    }
    function clear() {
        // The children's views first, here: a node's own destruction comes later, maybe with its context gone.
        for (const child of childNodes) {
            (child as AreaNode).clear()
            child.destroy()
        }
        childNodes = []
        for (const made of handleItems)
            (made as Item).destroy()
        handleItems = []
        if (headerItem)
            headerItem.destroy()
        headerItem = null
        if (frameItem)
            frameItem.destroy()
        frameItem = null
        if (view && !adopted)
            view.destroy()
        view = null
        adopted = false
        viewKey = ""
    }
    // These read `node` itself: the bindings above may not have caught up with it inside onNodeChanged.
    function makeView() {
        const type = node.view
        const id = node.area
        const existing = views[String(id)] ?? null
        if (existing) {
            view = existing
            adopted = true
        } else if (host) {
            const key = id + ":" + type
            const pool = topNode().spare
            const kept = pool ? pool[key] ?? null : null
            if (kept) {
                view = kept
            } else {
                let component = null
                if (registry && registry.contains(type)) {
                    component = Qt.createComponent(registry.component(type))
                    if (component.status === Component.Error) {
                        console.warn(component.errorString())
                        component = null
                    }
                }
                view = component ? component.createObject(host, {areaId: id, areaState: node.state?.[type] ?? {}}) as Item
                                 : placeholder.createObject(host, {viewType: type}) as Item
                const made = view as AreaView
                const root = topNode()
                if (made)
                    made.areaStateEdited.connect(state => root.controller?.setAreaState(id, state))
            }
            viewKey = key
        }
        if (host && header)
            headerItem = header.createObject(host, {areaId: id, viewType: type, areaView: view, z: 2}) as Item
        if (host && frame)
            frameItem = frame.createObject(host, {areaId: id, z: 3}) as Item
    }
    // A child's node as a plain object of its own: a part of this node read from a property is a reference into
    // it, which a new tree for this node would change under the child.
    function childNode(index: int): var {
        return JSON.parse(JSON.stringify(node.children[index].node))
    }
    function makeChildren() {
        const component = Qt.createComponent("AreaNode.qml")
        const root = topNode()
        const made = []
        for (let i = 0; i < node.children.length; ++i)
            made.push(component.createObject(areaNode, {node: childNode(i), host: host, registry: registry, views: views,
                                                        rightToLeft: rightToLeft, controller: controller, header: header,
                                                        headerHeight: headerHeight, leadingClear: leadingClear,
                                                        handle: handle, frame: frame, gap: gap, path: path.concat([i]), top: root}))
        childNodes = made
        const handles = []
        if (host && handle) {
            for (let border = 0; border + 1 < node.children.length; ++border) {
                const one = handle.createObject(host, {axis: node.axis, rightToLeft: rightToLeft, z: 1}) as AreaHandle
                const index = border
                one.moveRequested.connect(delta => root.controller?.resizeBorder(areaNode.path, index, delta, areaNode.length()))
                one.equalizeRequested.connect(() => root.controller?.equalizeBorder(areaNode.path, index))
                handles.push(one)
            }
        }
        handleItems = handles
    }
    // A branch's length along its axis.
    function length(): real {
        return node?.axis === "column" ? area.height : area.width
    }
    // The leaf's card: its area less half the gap on each side that borders another area (an outer side is the host's own edge).
    function cardRect(): rect {
        const half = gap / 2
        if (half <= 0)
            return area
        const outer = topNode().area
        const edge = 0.5
        const left = area.x > outer.x + edge ? half : 0
        const top = area.y > outer.y + edge ? half : 0
        const right = area.x + area.width < outer.x + outer.width - edge ? half : 0
        const bottom = area.y + area.height < outer.y + outer.height - edge ? half : 0
        return Qt.rect(area.x + left, area.y + top, Math.max(0, area.width - left - right), Math.max(0, area.height - top - bottom))
    }
    function relayout() {
        if (!ready || !host)
            return
        if (node?.type === "leaf") {
            const card = cardRect()
            const shown = headerItem !== null && headerHeight > 0
            const above = shown ? Math.min(headerHeight, card.height) : 0
            if (headerItem) {
                headerItem.visible = shown
                headerItem.x = card.x
                headerItem.y = card.y
                headerItem.width = card.width
                headerItem.height = above
            }
            if (frameItem) {
                frameItem.x = card.x
                frameItem.y = card.y
                frameItem.width = card.width
                frameItem.height = card.height
            }
            const bar = headerItem as AreaHeader
            if (bar)
                bar.leadingClear = area.x <= 0.5 && area.y <= 0.5 ? leadingClear : 0
            if (view) {
                view.x = card.x
                view.y = card.y + above
                view.width = card.width
                view.height = card.height - above
            }
            return
        }
        const rects = AreaLayout.childRects(node, area, registry, rightToLeft)
        for (let i = 0; i < childNodes.length && i < rects.length; ++i) {
            const child = childNodes[i] as AreaNode
            child.node = childNode(i)
            child.rightToLeft = rightToLeft
            child.headerHeight = headerHeight
            child.leadingClear = leadingClear
            child.gap = gap
            child.area = rects[i]
        }
        const row = node.axis !== "column"
        for (let i = 0; i < handleItems.length && i + 1 < rects.length; ++i) {
            const one = handleItems[i] as AreaHandle
            const r = rects[i]
            const t = one.thickness
            one.rightToLeft = rightToLeft
            if (row) {
                const border = rightToLeft ? r.x : r.x + r.width
                one.x = border - t / 2
                one.y = area.y
                one.width = t
                one.height = area.height
            } else {
                one.x = area.x
                one.y = r.y + r.height - t / 2
                one.width = area.width
                one.height = t
            }
        }
    }
    function rebuild() {
        if (!ready)
            return
        const next = AreaLayout.shapeOf(node ?? {})
        if (next !== shape || next === "") {
            // The root keeps the views made for the old shape, gives them to the nodes of the new one that show the
            // same view in the same area, and drops the rest.
            const root = top === null
            if (root) {
                spare = {}
                harvest(spare)
            }
            clear()
            shape = next
            if (next === "") {
                if (node && Object.keys(node).length > 0)
                    console.warn("AreaNode: not an area tree node:", JSON.stringify(node))
            } else if (node.type === "leaf")
                makeView()
            else
                makeChildren()
            if (root) {
                const shown = []
                shownViews(shown)
                for (const key of Object.keys(spare))
                    if (shown.indexOf(spare[key]) < 0)
                        (spare[key] as Item).destroy()
                spare = null
            }
        }
        relayout()
    }

    onNodeChanged: rebuild()
    onAreaChanged: relayout()
    onRightToLeftChanged: relayout()
    onHeaderHeightChanged: relayout()
    onLeadingClearChanged: relayout()
    onGapChanged: relayout()
    Component.onCompleted: {
        ready = true
        rebuild()
    }
    Component.onDestruction: clear()
}
