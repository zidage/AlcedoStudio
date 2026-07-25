import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared numeric adjustment slider + field. Binds to an
// EditorAdjustmentValueModel and drives its pointer-drag state:
//   - Handle drag only: beginDrag -> updateDrag (per move) -> finishDrag.
//   - Track click does NOT jump the value (no absolute seek).
//   - Double-click the slider (no drag movement): reset to default.
//   - Keyboard arrows on the slider: editValue (interactive + debounced settled).
//   - Field typing + Enter / focus-out: editValue + commitImmediately.
// One committed transaction per completed drag is guaranteed by the model.
//
// Visual language: monochrome (no blue accent fill). Larger handle + hit row
// for reliable pointer grabbing. No separate reset button — double-click resets.
//
// Optional `flickable`: when set, its `interactive` is cleared for the duration
// of a press so parent scroll areas cannot steal the drag.
Item {
    id: root
    objectName: "adjustmentSlider"

    property var model: null
    /// Optional parent Flickable/ListView to lock while the slider is pressed.
    property var flickable: null

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colDanger: appTheme.dangerColor
    readonly property color colTrack: "#2C2D2F"
    readonly property color colFill: "#D8D4CD"
    readonly property color colHandle: "#F5F1EA"
    readonly property color colHandleBorder: "#1A1B1C"
    readonly property color colZero: "#6A6761"
    readonly property bool centered: root.model
                                     && root.model.minimum < 0
                                     && root.model.maximum > 0

    readonly property int handleSize: 22
    readonly property int sliderRowHeight: 32
    /// Hit radius around the handle center for starting a drag (px).
    readonly property int handleHitPad: 12

    // Double-click detection without TapHandler (TapHandler steals the grab and
    // breaks continuous drag + real double-clicks on some styles).
    property bool _gestureMoved: false
    property double _pressValue: 0
    property double _lastClickMs: 0
    property var _savedFlickableInteractive: null
    property bool _handleDragging: false

    implicitHeight: sliderColumn.implicitHeight
    Layout.fillWidth: true

    function formatValue(v) {
        if (!root.model) {
            return ""
        }
        var s = Number(v).toFixed(root.model.precision)
        return root.model.suffix && root.model.suffix.length > 0 ? s + root.model.suffix : s
    }

    function parseField(text) {
        if (!root.model) {
            return { ok: false, value: 0 }
        }
        var s = text
        if (root.model.suffix && root.model.suffix.length > 0 && s.endsWith(root.model.suffix)) {
            s = s.substring(0, s.length - root.model.suffix.length)
        }
        s = s.trim()
        var n = parseFloat(s)
        if (isNaN(n) || !isFinite(n)) {
            return { ok: false, value: 0 }
        }
        if (n < root.model.minimum || n > root.model.maximum) {
            return { ok: false, value: 0, outOfRange: true }
        }
        return { ok: true, value: n }
    }

    function syncField() {
        if (root.model && !valueField.activeFocus) {
            valueField.text = formatValue(root.model.value)
        }
    }

    function lockFlickable(lock) {
        if (!root.flickable)
            return
        if (lock) {
            if (root._savedFlickableInteractive === null)
                root._savedFlickableInteractive = root.flickable.interactive
            root.flickable.interactive = false
        } else {
            if (root._savedFlickableInteractive !== null) {
                root.flickable.interactive = root._savedFlickableInteractive
                root._savedFlickableInteractive = null
            }
        }
    }

    function handleCenterX() {
        return slider.leftPadding
                + slider.visualPosition * (slider.availableWidth - root.handleSize)
                + root.handleSize * 0.5
    }

    function isNearHandle(localX) {
        return Math.abs(localX - handleCenterX())
                <= Math.max(root.handleSize * 0.5 + root.handleHitPad, 16)
    }

    function valueFromLocalX(localX) {
        var edge = root.handleSize * 0.5
        var trackW = Math.max(1e-6, slider.availableWidth - root.handleSize)
        var pos = (localX - slider.leftPadding - edge) / trackW
        pos = Math.max(0, Math.min(1, pos))
        var v = slider.from + pos * (slider.to - slider.from)
        if (root.model && root.model.step > 0) {
            v = Math.round(v / root.model.step) * root.model.step
        }
        if (root.model) {
            v = Math.max(root.model.minimum, Math.min(root.model.maximum, v))
        }
        return v
    }

    function finishPointerGesture() {
        if (!root.model) {
            root.lockFlickable(false)
            root._handleDragging = false
            return
        }
        // Double-click: second press/release without movement within 350ms.
        var now = Date.now()
        var isDouble = !root._gestureMoved
                && (now - root._lastClickMs) < 350
                && Math.abs(slider.value - root._pressValue)
                   <= Math.max(root.model.step * 0.5, 1e-9)
        root._lastClickMs = now
        root.lockFlickable(false)

        if (isDouble && root.model.enabled) {
            // Reset clears any open drag and owns the single settled commit.
            root.model.reset()
            slider.value = root.model.value
        } else if (root._handleDragging) {
            root.model.finishDrag()
            slider.value = root.model.value
        }
        // Track press without handle drag: no value change, no submit.
        root._handleDragging = false
    }

    onModelChanged: syncField()

    ColumnLayout {
        id: sliderColumn
        anchors.fill: parent
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 18
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: root.model ? root.model.label : ""
                color: root.colText
                font.pixelSize: appTheme.fontSizeCaption
                elide: Text.ElideRight
                visible: root.model && root.model.label && root.model.label.length > 0
                Accessible.name: root.model ? root.model.label : ""
            }

            TextField {
                id: valueField
                objectName: "adjustmentSliderField"
                Layout.preferredWidth: 56
                Layout.preferredHeight: 18
                leftPadding: 2
                rightPadding: 2
                topPadding: 0
                bottomPadding: 0
                text: ""
                color: (root.model && !root.model.valid) ? root.colDanger : root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
                horizontalAlignment: TextInput.AlignRight
                verticalAlignment: TextInput.AlignVCenter
                selectByMouse: true
                enabled: root.model && root.model.enabled
                activeFocusOnTab: true
                Accessible.role: Accessible.EditableText
                Accessible.name: root.model ? root.model.label : ""

                background: Rectangle {
                    color: "transparent"
                    border.width: valueField.activeFocus || (root.model && !root.model.valid) ? 1 : 0
                    border.color: (root.model && !root.model.valid)
                                  ? root.colDanger
                                  : root.colMuted
                    radius: appTheme.controlRadiusSmall
                }

                Component.onCompleted: root.syncField()
                onActiveFocusChanged: {
                    if (activeFocus) {
                        selectAll()
                    } else if (root.model) {
                        valueField.text = root.formatValue(root.model.value)
                    }
                }
                onEditingFinished: {
                    if (!root.model) {
                        return
                    }
                    var parsed = root.parseField(valueField.text)
                    if (!parsed.ok) {
                        root.model.setInvalid(parsed.outOfRange
                                              ? qsTr("Out of range")
                                              : qsTr("Invalid number"))
                        valueField.text = root.formatValue(root.model.value)
                    } else {
                        root.model.editValue(parsed.value)
                        root.model.commitImmediately()
                        valueField.text = root.formatValue(root.model.value)
                    }
                }

                Connections {
                    target: root.model
                    function onValueChanged() { root.syncField() }
                    function onValidChanged() { root.syncField() }
                }
            }
        }

        Slider {
            id: slider
            objectName: "adjustmentSliderHandle"
            Layout.fillWidth: true
            Layout.preferredHeight: root.sliderRowHeight
            enabled: root.model && root.model.enabled
            from: root.model ? root.model.minimum : 0
            to: root.model ? root.model.maximum : 1
            stepSize: root.model ? root.model.step : 0
            snapMode: Slider.SnapAlways
            live: true
            touchDragThreshold: 0
            value: root.model ? root.model.value : 0
            activeFocusOnTab: true
            padding: 0
            leftPadding: 0
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0
            // Swallow Controls.Slider's built-in absolute seek; pointer input is
            // owned by sliderInput (handle-drag only).
            hoverEnabled: false
            Accessible.role: Accessible.Slider
            Accessible.name: root.model ? root.model.label : ""
            Accessible.description: root.model
                ? qsTr("Drag the handle to adjust %1. Double-click to reset.").arg(root.model.label)
                : ""

            Keys.onLeftPressed: function (event) {
                if (root.model) {
                    var v = Math.max(root.model.minimum, slider.value - root.model.step)
                    root.model.editValue(v)
                }
                event.accepted = true
            }
            Keys.onRightPressed: function (event) {
                if (root.model) {
                    var v = Math.min(root.model.maximum, slider.value + root.model.step)
                    root.model.editValue(v)
                }
                event.accepted = true
            }

            background: Rectangle {
                readonly property real zeroPosition: root.centered
                    ? (-root.model.minimum / (root.model.maximum - root.model.minimum))
                    : 0
                readonly property real startPosition: root.centered ? zeroPosition : 0
                readonly property real edge: root.handleSize * 0.5

                x: slider.leftPadding + edge
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                width: Math.max(0, slider.availableWidth - root.handleSize)
                height: 6
                radius: 3
                color: root.colTrack

                Rectangle {
                    x: Math.min(parent.startPosition, slider.visualPosition) * parent.width
                    y: 0
                    width: Math.abs(slider.visualPosition - parent.startPosition) * parent.width
                    height: parent.height
                    radius: 3
                    color: (root.model && !root.model.valid) ? root.colDanger : root.colFill
                }

                Rectangle {
                    visible: root.centered
                    x: parent.zeroPosition * parent.width
                    y: 0
                    width: 1
                    height: parent.height
                    color: root.colZero
                }
            }
            handle: Rectangle {
                x: slider.leftPadding
                   + slider.visualPosition * (slider.availableWidth - width)
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                implicitWidth: root.handleSize
                implicitHeight: root.handleSize
                width: root.handleSize
                height: root.handleSize
                radius: width / 2
                color: root.colHandle
                border.width: 1
                border.color: root.colHandleBorder

                Rectangle {
                    anchors.centerIn: parent
                    width: root.handleSize + 10
                    height: root.handleSize + 10
                    radius: width / 2
                    color: "transparent"
                    border.width: slider.activeFocus || root._handleDragging ? 1 : 0
                    border.color: root.colFill
                }
            }

            // Owns pointer input so Controls.Slider cannot jump-to-click on the track.
            MouseArea {
                id: sliderInput
                objectName: "adjustmentSliderInput"
                anchors.fill: parent
                enabled: root.model && root.model.enabled
                preventStealing: true
                hoverEnabled: false
                acceptedButtons: Qt.LeftButton
                cursorShape: root._handleDragging ? Qt.ClosedHandCursor : Qt.ArrowCursor

                onPressed: function (mouse) {
                    if (!root.model) {
                        mouse.accepted = true
                        return
                    }
                    slider.forceActiveFocus()
                    root._gestureMoved = false
                    root._pressValue = slider.value
                    root.lockFlickable(true)
                    if (root.isNearHandle(mouse.x)) {
                        root._handleDragging = true
                        root.model.beginDrag()
                        // Keep value at press — no absolute jump toward click.
                    } else {
                        // Track press: swallow event, no beginDrag, no value change.
                        root._handleDragging = false
                    }
                    mouse.accepted = true
                }
                onPositionChanged: function (mouse) {
                    if (!root._handleDragging || !root.model)
                        return
                    root._gestureMoved = true
                    var v = root.valueFromLocalX(mouse.x)
                    slider.value = v
                    root.model.updateDrag(v)
                }
                onReleased: function (/*mouse*/) {
                    root.finishPointerGesture()
                }
                onCanceled: {
                    if (root._handleDragging && root.model)
                        root.model.finishDrag()
                    root._handleDragging = false
                    root.lockFlickable(false)
                }
            }

            Connections {
                target: root.model
                function onValueChanged() {
                    if (!root._handleDragging && root.model) {
                        slider.value = root.model.value
                    }
                }
            }
        }
    }
}
