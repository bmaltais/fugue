import QtQuick
import QtQuick.Shapes
import Fugue

// The padded area a Canvas Region cuts, drawn over a picture (#228, the cohesion audit's C9): accent dashes on a 3px
// dark halo, so the box reads on a light or an accent-colored picture and is not taken for the white solid frame of
// the rectangle (CropFrame). The region picker (RegionDialog.qml) and the Canvas view draw it. `box` is in the item's
// own coordinates; it never takes the pointer.
Shape {
    id: shape
    required property CanvasLook look
    property rect box: Qt.rect(0, 0, 0, 0)
    // The halo is the dark CropFrame draws its handles' edge with over any picture.
    ShapePath {
        strokeColor: Qt.rgba(0, 0, 0, 0.6)
        strokeWidth: 3
        fillColor: "transparent"
        startX: shape.box.left; startY: shape.box.top
        PathLine { x: shape.box.right; y: shape.box.top }
        PathLine { x: shape.box.right; y: shape.box.bottom }
        PathLine { x: shape.box.left; y: shape.box.bottom }
        PathLine { x: shape.box.left; y: shape.box.top }
    }
    ShapePath {
        strokeColor: shape.look.accent
        strokeWidth: 1.5
        strokeStyle: ShapePath.DashLine
        dashPattern: [4, 3]
        fillColor: "transparent"
        startX: shape.box.left; startY: shape.box.top
        PathLine { x: shape.box.right; y: shape.box.top }
        PathLine { x: shape.box.right; y: shape.box.bottom }
        PathLine { x: shape.box.left; y: shape.box.bottom }
        PathLine { x: shape.box.left; y: shape.box.top }
    }
}
