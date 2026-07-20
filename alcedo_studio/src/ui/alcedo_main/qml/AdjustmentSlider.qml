import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared numeric adjustment slider + field + reset. Binds to an
// EditorAdjustmentValueModel and drives its gesture lifecycle:
//   - Pointer drag: beginGesture → updateGesture (per move) → commitGesture.
//   - Keyboard arrows on the slider: editValue (interactive + debounced settled).
//   - Field typing + Enter / focus-out: editValue + commitImmediately.
//   - Reset button: reset (settled immediately).
// One committed transaction per completed gesture is guaranteed by the model.
//
// Value sync: the slider follows model.value, but while the user drags
// (pressed) the slider owns its value and onMoved pushes it to the model; the
// model→slider sync is suppressed during press to avoid fighting the drag. The
// field is managed imperatively (no text binding) so user typing and external
// loads do not feedback-loop.
//
// The two-line geometry mirrors the legacy QWidget editor: label/value above a
// full-width 22 px slider. The painted handle stays compact while the complete
// slider row remains draggable.
Item {
    id: root
    objectName: "adjustmentSlider"

    property var model: null

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colPositive: appTheme.editorSliderPositiveColor
    readonly property color colNegative: appTheme.editorSliderNegativeColor
    readonly property color colDanger: appTheme.dangerColor
    readonly property color colTrack: appTheme.editorSliderTrackColor
    readonly property color colHandle: appTheme.editorSliderHandleColor
    readonly property color colHandleBorder: appTheme.editorSliderHandleBorderColor
    readonly property bool centered: root.model
                                     && root.model.minimum < 0
                                     && root.model.maximum > 0

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

    onModelChanged: syncField()

    ColumnLayout {
        id: sliderColumn
        anchors.fill: parent
        spacing: 2

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

            AdjustmentResetButton {
                model: root.model
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
            }
        }

        Slider {
            id: slider
            objectName: "adjustmentSliderHandle"
            Layout.fillWidth: true
            Layout.preferredHeight: 22
            enabled: root.model && root.model.enabled
            from: root.model ? root.model.minimum : 0
            to: root.model ? root.model.maximum : 1
            stepSize: root.model ? root.model.step : 0
            snapMode: Slider.SnapAlways
            live: true
            touchDragThreshold: 0
            value: root.model ? root.model.value : 0
            activeFocusOnTab: true
            Accessible.role: Accessible.Slider
            Accessible.name: root.model ? root.model.label : ""
            Accessible.description: root.model
                ? qsTr("Adjust %1").arg(root.model.label)
                : ""

            onPressedChanged: {
                if (!root.model) {
                    return
                }
                if (pressed) {
                    root.model.beginGesture()
                } else {
                    root.model.commitGesture()
                }
            }
            onMoved: {
                if (root.model) {
                    root.model.updateGesture(slider.value)
                }
            }
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

                x: slider.leftPadding + 10
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                width: Math.max(0, slider.availableWidth - 20)
                height: 10
                radius: 5
                color: root.colTrack

                Rectangle {
                    x: Math.min(parent.startPosition, slider.visualPosition) * parent.width
                    y: 1
                    width: Math.abs(slider.visualPosition - parent.startPosition) * parent.width
                    height: parent.height - 2
                    radius: 4
                    color: (root.model && !root.model.valid)
                           ? root.colDanger
                           : (slider.value < 0 ? root.colNegative : root.colPositive)
                }

                Rectangle {
                    visible: root.centered
                    x: parent.zeroPosition * parent.width
                    y: 1
                    width: 1
                    height: parent.height - 2
                    color: root.colMuted
                    opacity: 0.45
                }
            }
            handle: Rectangle {
                x: slider.leftPadding + 2
                   + slider.visualPosition * (slider.availableWidth - width - 4)
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                width: 16
                height: 16
                radius: 8
                color: root.colHandle
                border.width: 1
                border.color: root.colHandleBorder

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    radius: width / 2
                    color: "transparent"
                    border.width: slider.activeFocus ? 1 : 0
                    border.color: slider.value < 0 ? root.colNegative : root.colPositive
                }
            }

            Connections {
                target: root.model
                function onValueChanged() {
                    // Re-sync the slider to the model when the model changes
                    // externally (load, reset, undo/redo). While the user is
                    // dragging (pressed) the slider owns its value.
                    if (!slider.pressed && root.model) {
                        slider.value = root.model.value
                    }
                }
            }
        }

    }
}
