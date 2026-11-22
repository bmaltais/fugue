import QtQuick

// What a view made for an area is (#217, spec 12.16): the root of every registry component but the Graph, which
// the window declares itself and its area adopts. AreaNode.qml makes it with the area's id and the state its type
// has in that area (the area tree keeps a state per view type), and a view says what to keep with
// `areaStateEdited`: on a user's change, never in answer to the state it was given.
FocusScope {
    property int areaId: 0
    property var areaState: ({})
    signal areaStateEdited(var state)
}
