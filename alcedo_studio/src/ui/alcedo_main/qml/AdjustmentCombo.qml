import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Enum adjustment combo (Phase 6A). Binds to an EditorAdjustmentEnumModel
// (entries: [{value, label}, ...]) and commits one settled transaction on
// selection. The ComboBox follows model.currentIndex for programmatic loads;
// onActivated pushes the user's choice to the model. Visuals use appTheme
// tokens (DESIGN.md).
Item {
    id: root
    objectName: "adjustmentCombo"

    property var model: null

    implicitHeight: comboRow.implicitHeight
    Layout.fillWidth: true

    RowLayout {
        id: comboRow
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Label {
            Layout.preferredWidth: 76
            text: root.model ? root.model.label : ""
            color: appTheme.textColor
            font.pixelSize: appTheme.fontSizeBody
            elide: Text.ElideRight
            visible: root.model && root.model.label && root.model.label.length > 0
            Accessible.name: root.model ? root.model.label : ""
        }

        ComboBox {
            id: combo
            objectName: "adjustmentCombo"
            Layout.fillWidth: true
            enabled: root.model && root.model.enabled
            model: root.model ? root.model.entries : []
            textRole: "label"
            currentIndex: root.model ? root.model.currentIndex : 0
            activeFocusOnTab: true
            Accessible.role: Accessible.ComboBox
            Accessible.name: root.model ? root.model.label : ""
            onActivated: function (index) {
                if (root.model) {
                    root.model.selectIndex(index)
                }
            }
            Connections {
                target: root.model
                function onCurrentIndexChanged() {
                    if (root.model) {
                        combo.currentIndex = root.model.currentIndex
                    }
                }
                function onEntriesChanged() {
                    if (root.model) {
                        combo.model = root.model.entries
                    }
                }
            }
        }

        AdjustmentResetButton {
            model: root.model
        }
    }
}