pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Fugue

// The two small questions of Select > Modify and Transform selection (#251). Modify asks one pixel amount (Expand, Contract,
// Feather, Border, Smooth: the title and the hint change, the field is the same); Transform asks Scale %, Rotate degrees
// and Move X, Y. Enter in a field answers it; the answer is a signal, so the view does the work as one undo step.
// There is no live preview: the result is one step that Undo takes back.
Item {
    id: root
    objectName: "canvasSelectionDialogs"
    readonly property CanvasLook look: App.theme.look
    readonly property real u: look.unit
    visible: false

    // `kind`: "expand", "contract", "feather", "border" or "smooth".
    signal modifyAsked(string kind, int amount)
    signal transformAsked(real dx, real dy, real scaleX, real scaleY, real degrees)

    property string kind: "expand"
    readonly property var kinds: ({
        expand: {title: qsTr("Expand selection"), field: qsTr("Expand by"), hint: qsTr("Grows the selection by this many pixels on every side.")},
        contract: {title: qsTr("Contract selection"), field: qsTr("Contract by"), hint: qsTr("Shrinks the selection by this many pixels on every side.")},
        feather: {title: qsTr("Feather selection"), field: qsTr("Feather radius"), hint: qsTr("Softens the edge: it fades over about this many pixels each side.")},
        border: {title: qsTr("Border selection"), field: qsTr("Width"), hint: qsTr("Keeps a band of this width along the edge, half inside and half outside.")},
        smooth: {title: qsTr("Smooth selection"), field: qsTr("Sample radius"), hint: qsTr("Rounds corners and drops specks within this many pixels.")}
    })
    readonly property var info: kinds[kind]
    // What the last answer was, so the next time opens with it (as Photoshop does).
    property var last: ({expand: 4, contract: 4, feather: 8, border: 8, smooth: 4})

    function askModify(which) {
        kind = which
        amountField.text = String(last[which])
        modifyAsk.open()
    }
    function askTransform() {
        scaleField.text = "100"
        turnField.text = "0"
        moveXField.text = "0"
        moveYField.text = "0"
        transformAsk.open()
    }
    // A field's typed number (TypedNumber: a comma is a decimal point only where the language writes one) rounded to one
    // decimal, as the fields' old DoubleValidator allowed; empty or not a number is `fallback`. Apply and Enter wait until
    // every field is `inRange`, so nothing here is clamped.
    function number(field, fallback) {
        const v = TypedNumber.parse(field.text)
        return isNaN(v) ? fallback : Math.round(v * 10) / 10
    }
    // Whether a field holds a whole number from `bottom` to `top` (empty or half-typed is not).
    function inRange(field, bottom, top) {
        const v = TypedNumber.parse(field.text)
        return !isNaN(v) && v >= bottom && v <= top
    }
    readonly property bool transformReady: inRange(scaleField, 1, 1000) && inRange(turnField, -360, 360)
                                           && inRange(moveXField, -99999, 99999) && inRange(moveYField, -99999, 99999)
    function answerModify() {
        const amount = Math.round(number(amountField, 0))
        if (amount < 1)
            return
        last[kind] = amount
        modifyAsk.close()
        modifyAsked(kind, amount)
    }
    function answerTransform() {
        if (!transformReady)
            return
        const scale = number(scaleField, 100) / 100
        transformAsk.close()
        transformAsked(number(moveXField, 0), number(moveYField, 0), scale, scale, number(turnField, 0))
    }

    Question {
        id: modifyAsk
        objectName: "canvasModifyAsk"
        heading: root.info.title
        onOpened: { amountField.forceActiveFocus(); amountField.selectAll() }
        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * root.u
            RowLayout {
                spacing: 2 * root.u
                Label { text: root.info.field }
                ThemedTextField {
                    id: amountField
                    objectName: "canvasModifyAmount"
                    Layout.preferredWidth: 16 * root.u
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { bottom: 1; top: 250 }
                    selectByMouse: true
                    onAccepted: if (modifyOk.enabled) root.answerModify()
                }
                Label { text: qsTr("pixels") }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                text: root.info.hint
            }
            QuestionAnswers {
                ThemedButton { text: qsTr("Cancel"); onClicked: modifyAsk.close() }
                ThemedButton {
                    id: modifyOk
                    objectName: "canvasModifyOk"
                    text: qsTr("Apply")
                    highlighted: true
                    enabled: amountField.acceptableInput
                    onClicked: root.answerModify()
                }
            }
        }
    }

    Question {
        id: transformAsk
        objectName: "canvasTransformAsk"
        heading: qsTr("Transform selection")
        onOpened: { scaleField.forceActiveFocus(); scaleField.selectAll() }
        ColumnLayout {
            anchors.fill: parent
            spacing: 3 * root.u
            GridLayout {
                columns: 3
                columnSpacing: 2 * root.u
                rowSpacing: 2 * root.u
                Label { text: qsTr("Scale") }
                ThemedTextField {
                    id: scaleField
                    objectName: "canvasTransformScale"
                    Layout.preferredWidth: 16 * root.u
                    validator: RegularExpressionValidator { regularExpression: TypedNumber.pattern }
                    selectByMouse: true
                    // Out of range or half-typed: the text in the error colour, and Apply waits.
                    readonly property bool ready: root.inRange(scaleField, 1, 1000)
                    color: ready ? root.look.text : root.look.error
                    ToolTip.text: qsTr("From %1 to %2").arg(1).arg(1000)
                    ToolTip.visible: hovered
                    ToolTip.delay: root.look.tooltipDelay
                    Accessible.description: ToolTip.text
                    onAccepted: root.answerTransform() // only when every field is ready
                }
                Label { text: "%" }
                Label { text: qsTr("Rotate") }
                ThemedTextField {
                    id: turnField
                    objectName: "canvasTransformRotate"
                    Layout.preferredWidth: 16 * root.u
                    validator: RegularExpressionValidator { regularExpression: TypedNumber.pattern }
                    selectByMouse: true
                    // Out of range or half-typed: the text in the error colour, and Apply waits.
                    readonly property bool ready: root.inRange(turnField, -360, 360)
                    color: ready ? root.look.text : root.look.error
                    ToolTip.text: qsTr("From %1 to %2").arg(-360).arg(360)
                    ToolTip.visible: hovered
                    ToolTip.delay: root.look.tooltipDelay
                    Accessible.description: ToolTip.text
                    onAccepted: root.answerTransform() // only when every field is ready
                }
                Label { text: qsTr("degrees") }
                Label { text: qsTr("Move right") }
                ThemedTextField {
                    id: moveXField
                    objectName: "canvasTransformMoveX"
                    Layout.preferredWidth: 16 * root.u
                    validator: RegularExpressionValidator { regularExpression: TypedNumber.pattern }
                    selectByMouse: true
                    // Out of range or half-typed: the text in the error colour, and Apply waits.
                    readonly property bool ready: root.inRange(moveXField, -99999, 99999)
                    color: ready ? root.look.text : root.look.error
                    ToolTip.text: qsTr("From %1 to %2").arg(-99999).arg(99999)
                    ToolTip.visible: hovered
                    ToolTip.delay: root.look.tooltipDelay
                    Accessible.description: ToolTip.text
                    onAccepted: root.answerTransform() // only when every field is ready
                }
                Label { text: qsTr("pixels") }
                Label { text: qsTr("Move down") }
                ThemedTextField {
                    id: moveYField
                    objectName: "canvasTransformMoveY"
                    Layout.preferredWidth: 16 * root.u
                    validator: RegularExpressionValidator { regularExpression: TypedNumber.pattern }
                    selectByMouse: true
                    // Out of range or half-typed: the text in the error colour, and Apply waits.
                    readonly property bool ready: root.inRange(moveYField, -99999, 99999)
                    color: ready ? root.look.text : root.look.error
                    ToolTip.text: qsTr("From %1 to %2").arg(-99999).arg(99999)
                    ToolTip.visible: hovered
                    ToolTip.delay: root.look.tooltipDelay
                    Accessible.description: ToolTip.text
                    onAccepted: root.answerTransform() // only when every field is ready
                }
                Label { text: qsTr("pixels") }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                text: qsTr("Scales and turns about the selection's center, then moves it. What leaves the picture is lost.")
            }
            QuestionAnswers {
                ThemedButton { text: qsTr("Cancel"); onClicked: transformAsk.close() }
                ThemedButton {
                    id: transformOk
                    objectName: "canvasTransformOk"
                    text: qsTr("Apply")
                    highlighted: true
                    enabled: root.transformReady
                    onClicked: root.answerTransform()
                }
            }
        }
    }
}
