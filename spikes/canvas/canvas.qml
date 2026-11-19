// PROTOTYPE (throwaway): Fugue canvas spike, ticket 05. Not production code.
// Question: does a hand-built QML canvas feel fast and light with hundreds of nodes and wires?
// Run:  qml canvas.qml            (then use the toolbar)
//       qml canvas.qml --grid 300 (start with 300 stress nodes)
// Keys/mouse: wheel = zoom at cursor; right or middle drag = pan; left drag a node = move it;
// drag from an output dot to an input dot = connect (invalid targets dim, the reason shows below).
import QtQuick
import QtQuick.Controls
import QtQuick.Shapes

ApplicationWindow {
    id: win
    width: 1500
    height: 900
    visible: true
    title: "Fugue canvas spike (throwaway prototype)"
    color: "#14161a"

    // ---- port types (open set: "upscale-model" is a resource type, ticket 06 Q1b) ----
    readonly property var typeColors: ({
        text: "#7fd1ff", number: "#ffd166", boolean: "#ef476f", image: "#06d6a0",
        video: "#c77dff", audio: "#ff9f1c", "upscale-model": "#b8b8ff"
    })
    function colorOf(t) { return typeColors[t] !== undefined ? typeColors[t] : "#aaaaaa" }
    function check(outT, inT) { return outT === inT ? "" : outT + " → " + inT + ": types differ" }

    // ---- model: nodes are items, links are plain objects ----
    property var nodeItems: []
    property var linkList: []
    property int wireMode: 0          // 0 one Shape, 1 one Shape per wire, 2 one Shape + CurveRenderer
    property var dragFrom: null       // {node, port, type} while connecting
    property point dragPoint
    property string hint: "wheel: zoom   right/middle drag: pan   drag output dot to input dot: connect"
    property string result: ""
    property int frames: 0
    property double lastSwap: 0
    property double maxGap: 0
    property int fps: 0
    property string benchName: ""
    property int benchFrames: 0
    property double benchStartMs: 0

    onFrameSwapped: {
        frames++
        const now = Date.now()
        if (lastSwap > 0 && now - lastSwap > maxGap) maxGap = now - lastSwap
        lastSwap = now
        if (benchName !== "") benchFrames++
    }
    Timer { interval: 1000; running: true; repeat: true; onTriggered: { win.fps = win.frames; win.frames = 0 } }

    function makeNode(title, ins, outs, px, py) {
        const n = nodeComp.createObject(world, { title: title, ins: ins, outs: outs, x: px, y: py })
        nodeItems.push(n)
        return n
    }
    function addLink(from, fp, to, tp) {
        linkList.push({ from: from, fromPort: fp, to: to, toPort: tp, type: from.outs[fp].type })
    }
    function rebuildWires() { const c = wires.sourceComponent; wires.sourceComponent = null; wires.sourceComponent = c }

    function demo() {
        const p = makeNode("Prompt", [], [{ name: "text", type: "text" }], 40, 80)
        const m = makeNode("Load Upscale Model", [], [{ name: "model", type: "upscale-model" }], 40, 260)
        const g = makeNode("Generate Image", [{ name: "prompt", type: "text" }, { name: "seed", type: "number" }],
                           [{ name: "image", type: "image" }, { name: "seed used", type: "number" }], 320, 60)
        const u = makeNode("Upscale (model)", [{ name: "image", type: "image" }, { name: "model", type: "upscale-model" }],
                           [{ name: "image", type: "image" }], 620, 120)
        const s = makeNode("Save Image", [{ name: "image", type: "image" }], [], 920, 120)
        addLink(p, 0, g, 0); addLink(g, 0, u, 0); addLink(m, 0, u, 1); addLink(u, 0, s, 0)
        rebuildWires()
    }

    function addGrid(count) {
        let cols = Math.ceil(Math.sqrt(count * 1.6)), rows = Math.ceil(count / cols)
        const baseX = 40, baseY = 420 + (nodeItems.length > 5 ? 0 : 0)
        const grid = []
        for (let r = 0; r < rows; r++) {
            for (let c = 0; c < cols; c++) {
                if (grid.length >= count) break
                const n = makeNode("Filter " + (grid.length + 1),
                                   [{ name: "image", type: "image" }, { name: "mask", type: "image" }],
                                   [{ name: "image", type: "image" }], baseX + c * 240, baseY + r * 150)
                grid.push({ n: n, r: r, c: c })
                if (c > 0) addLink(grid[grid.length - 2].n, 0, n, 0)
                if (r > 0) addLink(grid[grid.length - 1 - cols].n, 0, n, 1)
            }
        }
        rebuildWires()
    }
    function clearAll() {
        for (const n of nodeItems) n.destroy()
        nodeItems = []; linkList = []; rebuildWires()
    }

    // ---- connecting ----
    function beginLink(node, port, wp) {
        dragFrom = { node: node, port: port, type: node.outs[port].type }
        dragPoint = wp
    }
    function hitInput(wp) {
        for (const n of nodeItems) {
            for (let i = 0; i < n.ins.length; i++) {
                const q = n.inPos(i)
                if (Math.hypot(q.x - wp.x, q.y - wp.y) < 14) return { node: n, port: i }
            }
        }
        return null
    }
    function moveLink(wp) {
        dragPoint = wp
        const h = hitInput(wp)
        if (!h) { hint = "release on an input dot to connect"; return }
        if (h.node === dragFrom.node) { hint = "cannot connect a node to itself"; return }
        const why = check(dragFrom.type, h.node.ins[h.port].type)
        hint = why === "" ? "ok: " + dragFrom.type + " → " + h.node.ins[h.port].type : why
    }
    function endLink(wp) {
        const h = hitInput(wp)
        if (h && h.node !== dragFrom.node && check(dragFrom.type, h.node.ins[h.port].type) === "") {
            linkList = linkList.filter(l => !(l.to === h.node && l.toPort === h.port))
            addLink(dragFrom.node, dragFrom.port, h.node, h.port)
            linkList = linkList.slice()
            rebuildWires()
        }
        dragFrom = null
        hint = ""
    }

    // ---- benchmarks ----
    function benchBegin(name) {
        benchName = name; benchFrames = 0; maxGap = 0; lastSwap = 0; benchStartMs = Date.now()
    }
    function benchEnd() {
        const secs = (Date.now() - benchStartMs) / 1000
        result = benchName + ": " + nodeItems.length + " nodes, " + linkList.length + " wires, wire mode " + wireMode
                 + " → " + (benchFrames / secs).toFixed(1) + " fps avg, worst gap " + maxGap.toFixed(0) + " ms"
        console.log("RESULT " + result)
        benchName = ""
    }
    SequentialAnimation {
        id: panBench
        onStarted: win.benchBegin("pan")
        onFinished: { win.benchEnd(); win.afterBench() }
        NumberAnimation { target: world; property: "x"; to: world.x - 700; duration: 1500; easing.type: Easing.InOutSine }
        NumberAnimation { target: world; property: "x"; to: world.x; duration: 1500; easing.type: Easing.InOutSine }
    }
    property Item dragTarget: null
    property real dragBaseX: 0
    property real dragBaseY: 0
    SequentialAnimation {
        id: dragBench
        onStarted: win.benchBegin("drag one node (wires re-tessellate)")
        onFinished: { win.benchEnd(); win.afterBench() }
        NumberAnimation { target: win; property: "dragPhase"; from: 0; to: 6.2832 * 2; duration: 3000 }
    }
    property real dragPhase: 0
    onDragPhaseChanged: if (dragTarget) {
        dragTarget.x = dragBaseX + Math.cos(dragPhase) * 120
        dragTarget.y = dragBaseY + Math.sin(dragPhase) * 120
    }
    function startPanBench() { panBench.start() }
    function startDragBench() {
        if (nodeItems.length === 0) return
        dragTarget = nodeItems[Math.floor(nodeItems.length / 2)]
        dragBaseX = dragTarget.x; dragBaseY = dragTarget.y
        dragBench.start()
    }
    property int benchStep: 0
    function afterBench() {
        if (!autoBench) return
        benchStep++
        if (benchStep === 1) startDragBench()
        else Qt.quit()
    }
    property bool autoBench: false

    // ---- nodes ----
    Component {
        id: nodeComp
        Rectangle {
            id: n
            property string title: ""
            property var ins: []
            property var outs: []
            readonly property real headerH: 26
            readonly property real rowH: 22
            width: 190
            height: headerH + Math.max(ins.length, outs.length) * rowH + 44
            radius: 6
            color: "#23262d"
            border.width: 1
            border.color: "#3a3f4a"
            function inPos(i) { return Qt.point(x, y + headerH + i * rowH + rowH / 2) }
            function outPos(i) { return Qt.point(x + width, y + headerH + i * rowH + rowH / 2) }

            Rectangle {
                width: parent.width; height: n.headerH; radius: 6; color: "#2f3440"
                Rectangle { y: parent.height - 6; width: parent.width; height: 6; color: parent.color }
                Text { x: 10; anchors.verticalCenter: parent.verticalCenter; text: n.title; color: "#e6e8ee"; font.pixelSize: 12; font.bold: true }
            }
            DragHandler { acceptedButtons: Qt.LeftButton; target: n }
            Repeater {
                model: n.ins
                delegate: Item {
                    y: n.headerH + index * n.rowH; height: n.rowH; width: n.width
                    readonly property bool dim: win.dragFrom !== null && win.check(win.dragFrom.type, modelData.type) !== ""
                    opacity: dim ? 0.25 : 1
                    Rectangle { x: -5; y: 6; width: 10; height: 10; radius: 5; color: win.colorOf(modelData.type); border.width: 1; border.color: "#14161a" }
                    Text { x: 12; anchors.verticalCenter: parent.verticalCenter; text: modelData.name; color: "#aeb4c0"; font.pixelSize: 11 }
                }
            }
            Repeater {
                model: n.outs
                delegate: Item {
                    y: n.headerH + index * n.rowH; height: n.rowH; width: n.width
                    Text { anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: modelData.name; color: "#aeb4c0"; font.pixelSize: 11 }
                    Rectangle { x: n.width - 5; y: 6; width: 10; height: 10; radius: 5; color: win.colorOf(modelData.type); border.width: 1; border.color: "#14161a" }
                    MouseArea {
                        x: n.width - 12; y: 2; width: 24; height: 18
                        acceptedButtons: Qt.LeftButton
                        onPressed: mouse => win.beginLink(n, index, mapToItem(world, mouse.x, mouse.y))
                        onPositionChanged: mouse => win.moveLink(mapToItem(world, mouse.x, mouse.y))
                        onReleased: mouse => win.endLink(mapToItem(world, mouse.x, mouse.y))
                    }
                }
            }
            Rectangle {   // a stand-in for an image preview, so nodes cost something to paint
                x: 10; y: parent.height - 38; width: parent.width - 20; height: 28; radius: 3
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "#203040" }
                    GradientStop { position: 1; color: "#40306a" }
                }
            }
        }
    }

    // ---- wires ----
    Component {
        id: singleShape
        Shape {
            id: sh
            preferredRendererType: win.wireMode === 2 ? Shape.CurveRenderer : Shape.GeometryRenderer
            Instantiator {
                model: win.linkList
                delegate: ShapePath {
                    id: sp
                    property var link: modelData
                    property point a: link.from.outPos(link.fromPort)
                    property point b: link.to.inPos(link.toPort)
                    property real dx: Math.max(40, Math.abs(b.x - a.x) * 0.5)
                    strokeWidth: 2.5
                    strokeColor: win.colorOf(link.type)
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    startX: a.x; startY: a.y
                    PathCubic { x: sp.b.x; y: sp.b.y; control1X: sp.a.x + sp.dx; control1Y: sp.a.y; control2X: sp.b.x - sp.dx; control2Y: sp.b.y }
                }
                onObjectAdded: (i, o) => sh.data.push(o)
            }
        }
    }
    Component {
        id: perWireShape
        Item {
            Repeater {
                model: win.linkList
                delegate: Shape {
                    property var link: modelData
                    property point a: link.from.outPos(link.fromPort)
                    property point b: link.to.inPos(link.toPort)
                    property real dx: Math.max(40, Math.abs(b.x - a.x) * 0.5)
                    ShapePath {
                        strokeWidth: 2.5
                        strokeColor: win.colorOf(link.type)
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        startX: a.x; startY: a.y
                        PathCubic { x: b.x; y: b.y; control1X: a.x + dx; control1Y: a.y; control2X: b.x - dx; control2Y: b.y }
                    }
                }
            }
        }
    }

    // ---- viewport ----
    Item {
        id: viewport
        anchors.fill: parent
        clip: true
        Item {
            id: world
            transformOrigin: Item.TopLeft
            Loader { id: wires; z: -1; sourceComponent: win.wireMode === 1 ? perWireShape : singleShape }
            Shape {   // the wire being dragged
                visible: win.dragFrom !== null
                z: 5
                ShapePath {
                    strokeWidth: 2.5
                    strokeColor: win.dragFrom ? win.colorOf(win.dragFrom.type) : "white"
                    strokeStyle: ShapePath.DashLine
                    fillColor: "transparent"
                    startX: win.dragFrom ? win.dragFrom.node.outPos(win.dragFrom.port).x : 0
                    startY: win.dragFrom ? win.dragFrom.node.outPos(win.dragFrom.port).y : 0
                    PathLine { x: win.dragPoint.x; y: win.dragPoint.y }
                }
            }
        }
        DragHandler { target: world; acceptedButtons: Qt.RightButton | Qt.MiddleButton }
        WheelHandler {
            onWheel: event => {
                const f = event.angleDelta.y > 0 ? 1.12 : 1 / 1.12
                const s = Math.max(0.15, Math.min(3, world.scale * f))
                const px = event.x, py = event.y
                const wx = (px - world.x) / world.scale, wy = (py - world.y) / world.scale
                world.scale = s
                world.x = px - wx * s
                world.y = py - wy * s
            }
        }
    }

    // ---- toolbar ----
    Pane {
        id: bar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        padding: 8
        background: Rectangle { color: "#1c1f26"; opacity: 0.92 }
        Flow {
            width: parent.width
            spacing: 8
            Button { text: "+25 nodes"; onClicked: { win.addGrid(25) } }
            Button { text: "+100"; onClicked: { win.addGrid(100) } }
            Button { text: "+300"; onClicked: { win.addGrid(300) } }
            Button { text: "Clear"; onClicked: win.clearAll() }
            Button { text: "Demo graph"; onClicked: win.demo() }
            Button { text: "Reset view"; onClicked: { world.scale = 1; world.x = 0; world.y = 0 } }
            ComboBox {
                id: modeBox
                model: ["wires: one Shape", "wires: Shape per wire", "wires: one Shape + CurveRenderer"]
                currentIndex: win.wireMode
                onActivated: i => { win.wireMode = i; win.rebuildWires() }
                width: 290
            }
            Button { text: "Bench: pan"; enabled: win.benchName === ""; onClicked: win.startPanBench() }
            Button { text: "Bench: drag a node"; enabled: win.benchName === ""; onClicked: win.startDragBench() }
            Label { text: "FPS " + win.fps + "   worst gap " + win.maxGap.toFixed(0) + " ms   nodes " + win.nodeItems.length + "   wires " + win.linkList.length + "   zoom " + world.scale.toFixed(2); color: "#e6e8ee"; anchors.verticalCenter: undefined }
        }
    }
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 56
        color: "#1c1f26"
        opacity: 0.92
        Column {
            x: 12; y: 6
            Text { text: win.hint; color: "#ffd166"; font.pixelSize: 13 }
            Text { text: win.result; color: "#06d6a0"; font.pixelSize: 13 }
        }
    }

    // ---- start-up and headless checks ----
    Component.onCompleted: {
        const a = Qt.application.arguments
        console.log("ARGS " + JSON.stringify(a))
        demo(); console.log("DEMO nodes " + nodeItems.length + " links " + linkList.length)
        const gi = a.indexOf("--grid")
        if (gi >= 0) addGrid(parseInt(a[gi + 1]))
        const mi = a.indexOf("--mode")
        if (mi >= 0) { wireMode = parseInt(a[mi + 1]); rebuildWires() }
        if (a.indexOf("--bench") >= 0) { autoBench = true; startupBench.start() }
        const si = a.indexOf("--shot")
        if (si >= 0) { shotPath = a[si + 1]; shotTimer.start() }
    }
    property string shotPath: ""
    Timer { id: startupBench; interval: 800; onTriggered: win.startPanBench() }
    Timer {
        id: shotTimer
        interval: 1200
        onTriggered: viewport.grabToImage(r => { r.saveToFile(win.shotPath); console.log("SHOT " + win.shotPath); Qt.quit() })
    }
}
