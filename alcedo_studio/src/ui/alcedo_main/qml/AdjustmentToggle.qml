import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Boolean adjustment toggle (Phase 6A). Binds to an EditorAdjustmentToggleModel
// and commits one settled transaction on toggle. The Switch follows model.value
// for programmatic loads; while the user holds the thumb (pressed) the Switch
// owns its checked state and onToggled pushes it to the model. Visuals use
// appTheme tokens (DESIGN.md).
Item {
    id: root
    objectName: "adjustmentToggle"

    property var model: null

    implicitHeight: toggleRow.implicitHeight
    Layout.fillWidth: true

    RowLayout {
        id: toggleRow
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Label {
            Layout.fillWidth: true
            text: root.model ? root.model.label : ""
            color: appTheme.textColor
            font.pixelSize: appTheme.fontSizeBody
            elide: Text.ElideRight
            visible: root.model && root.model.label && root.model.label.length > 0
            Accessible.name: root.model ? root.model.label : ""
        }

        Switch {
            id: sw
            objectName: "adjustmentToggleSwitch"
            enabled: root.model && root.model.enabled
            checked: root.model ? root.model.value : false
            activeFocusOnTab: true
            Accessible.role: Accessible.CheckBox
            Accessible.name: root.model ? root.model.label : ""
            onToggled: {
                if (root.model) {
                    root.model.commitValue(sw.checked)
                }
            }
            Connections {
                target: root.model
                function onValueChanged() {
                    if (root.model && !sw.pressed) {
                        sw.checked = root.model.value
                    }
                }
            }
        }

        AdjustmentResetButton {
            model: root.model
        }
    }
}