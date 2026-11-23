.pragma library

// The rows of a menu of canvases (#211; one function since #228, the cohesion audit's C6): the canvas picker's on a
// node (InputWidget.qml) and the Canvas view's (CanvasView.qml) list the same rows the same way, as WidgetMenu entries.
// `rows` are canvasRows (canvas/canvas_choices.h): one row per canvas, newest first, its name, its size and last change
// at the right ("1024 × 768 · 22 Nov 14:03"), its id as the second line and a search keyword. `current` is checked.
// `missing`, when not empty, is a stored id this root has none of: it heads the list, checked and dimmed. Then the two
// ways to make one, a chunk of their own under a hairline, kept while searching: New canvas… and Import picture…,
// whose values are {action: "new"} and {action: "import"}; a canvas row's value is {id}. `withActive` (a node's picker,
// #246) puts "Active canvas" first, checked while `current` is empty: the node reads the canvas last used in a Canvas view,
// and a canvas row picked instead pins it; its value is {active: true}.

function entries(rows, current, missing, withActive) {
    const out = []
    if (withActive)
        out.push({ text: qsTr("Active canvas"), value: { active: true }, checked: current === "",
                   detail: qsTr("follows the Canvas view"),
                   note: qsTr("Reads the canvas you last used in the Canvas view, else the newest. Choosing a canvas below pins this node to it.") })
    if (missing !== "")
        out.push({ text: missing, detail: qsTr("Not in this root"), checked: true, dim: true,
                   note: qsTr("A canvas belongs to the Default or the project it was made in."),
                   value: { id: missing } })
    for (const row of rows)
        out.push({ text: row.name, value: { id: row.id }, checked: row.id === current,
                   detail: row.size === "" ? row.changedText : qsTr("%1 · %2").arg(row.size).arg(row.changedText),
                   note: row.id, keywords: row.id })
    if (rows.length === 0)
        out.push({ text: qsTr("No canvases yet"), enabled: false, note: "" })
    // The actions are a chunk of their own (docs/ui-ergonomics.md, Gaps), under a hairline.
    out.push({ separator: true, enabled: false })
    out.push({ text: qsTr("New canvas…"), value: { action: "new" }, keep: true })
    out.push({ text: qsTr("Import picture…"), value: { action: "import" }, keep: true })
    return out
}
