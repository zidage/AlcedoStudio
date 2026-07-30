import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Standalone numeric adjustment field + reset (Phase 6A). For numeric params
// that do not use a slider (e.g. precise numeric entry). Binds to an
// EditorAdjustmentValueModel and drives editValue + commitImmediately on
// Enter / focus-out, reset on the reset button. One committed transaction per
// committed edit. Visuals use appTheme tokens (DESIGN.md); invalid input
// recolors the border and text to dangerColor.
Item {
    id: root
    objectName: "adjustmentField"

    property var model: null

    readonly property color colText: appTheme.textColor
    readonly property color colDanger: appTheme.dangerColor
    readonly property color colFieldBorder: (root.model && !root.model.valid)
                                             ? appTheme.dangerColor
                                             : appTheme.dividerColor

    implicitHeight: fieldRow.implicitHeight
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
        id: fieldRow
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

        TextField {
            id: valueField
            objectName: "adjustmentField"
            Layout.fillWidth: true
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
        }
    }
}