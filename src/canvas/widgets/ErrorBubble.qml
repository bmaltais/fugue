import QtQuick

// Why the text in a widget's field is not a value (it is not written until it is one).
Rectangle {
    id: bubble
    objectName: "widgetError"
    required property InputWidget host

    visible: host.error !== ""
    z: 3
    height: message.implicitHeight + 8
    radius: host.smallCorner
    color: host.look.popup
    border.width: 1
    border.color: host.look.error

    Text {
        id: message
        x: 6
        y: 4
        width: bubble.width - 12
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: bubble.host.error
        color: bubble.host.look.text
        font.pixelSize: bubble.host.look.labelSize
    }
}
