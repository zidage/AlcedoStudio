import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared numeric adjustment slider + field + reset (Phase 6A). Binds to an
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
// Visuals use appTheme tokens only (DESIGN.md). Invalid input recolors the
// track, field border, and text to dangerColor; the model's valid flag is the
// source of truth and is cleared by the next valid editValue/setValue.
Item {
    id: root
    objectName: "adjustmentSlider"

    property var model: null

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colAccent: appTheme.accentColor
    readonly property color colDanger: appTheme.dangerColor
    readonly property color colTrack: appTheme.dividerColor
    readonly property color colHandle: appTheme.cardSurfaceColor
    readonly property color colFieldBorder: (root.model && !root.model.valid)
                                             ? appTheme.dangerColor
                                             : appTheme.dividerColor

    implicitHeight: sliderRow.implicitHeight
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

    RowLayout {
        id: sliderRow
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Label {
            Layout.preferredWidth: 76
            text: root.model ? root.model.label : ""
            color: root.colText
            font.pixelSize: appTheme.fontSizeBody
            elide: Text.ElideRight
            visible: root.model && root.model.label && root.model.label.length > 0
            Accessible.name: root.model ? root.model.label : ""
        }

        Slider {
            id: slider
            objectName: "adjustmentSliderHandle"
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            enabled: root.model && root.model.enabled
            from: root.model ? root.model.minimum : 0
            to: root.model ? root.model.maximum : 1
            stepSize: root.model ? root.model.step : 0
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
                x: slider.leftPadding
                y: slider.topPadding + slider.availableHeight / 2 - 2
                width: slider.availableWidth
                height: 4
                radius: 2
                color: root.colTrack
            }
            contentItem: Rectangle {
                x: slider.leftPadding
                y: slider.topPadding + slider.availableHeight / 2 - 2
                width: slider.visualPosition * slider.availableWidth
                height: 4
                radius: 2
                color: (root.model && !root.model.valid) ? root.colDanger : root.colAccent
            }
            handle: Rectangle {
                x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                width: 14
                height: 14
                radius: 7
                color: root.colHandle
                border.width: 1
                border.color: root.colMuted
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

        TextField {
            id: valueField
            objectName: "adjustmentSliderField"
            Layout.preferredWidth: 64
            text: ""
            color: (root.model && !root.model.valid) ? root.colDanger : root.colText
            font.pixelSize: appTheme.fontSizeBody
            horizontalAlignment: TextInput.AlignRight
            selectByMouse: true
            enabled: root.model && root.model.enabled
            activeFocusOnTab: true
            Accessible.role: Accessible.EditableText
            Accessible.name: root.model ? root.model.label : ""

            background: Rectangle {
                radius: appTheme.controlRadiusSmall
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: root.colFieldBorder
            }

            Component.onCompleted: root.syncField()
            onActiveFocusChanged: {
                if (activeFocus) {
                    selectAll()
                } else if (root.model) {
                    // Revert display if the user defocused without committing.
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
                function onValueChanged() {
                    root.syncField()
                }
                function onValidChanged() {
                    root.syncField()
                }
            }
        }

        AdjustmentResetButton {
            model: root.model
        }
    }
}