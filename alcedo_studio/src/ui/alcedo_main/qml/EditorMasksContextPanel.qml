import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Color Grade Mask context. Identifies Masks on the selected node without
// viewer authoring controls.
Item {
    id: root
    objectName: "editorAdjustmentPanel_masks"

    property var theme: null
    property var editorSession: null
    property var nodeController: null
    property bool controlsEnabled: true

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property var masks: {
        if (!root.nodeController)
            return []
        return root.nodeController.selectedNodeMasks || []
    }

    function loadFromSnapshot(snapshot) {
        // Mask identity is selection-owned. Snapshot values are not submitted.
        void snapshot
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Label {
            Layout.fillWidth: true
            text: qsTr("Masks")
            color: root.colText
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightHeading
        }

        Label {
            objectName: "editorMasksContextEmpty"
            Layout.fillWidth: true
            visible: root.masks.length === 0
            wrapMode: Text.WordWrap
            text: qsTr("This Color Grade has no Masks.")
            color: root.colMuted
            font.pixelSize: appTheme.fontSizeCaption
        }

        Repeater {
            model: root.masks
            delegate: EditorNodeMaskTypeRow {
                required property var modelData
                Layout.fillWidth: true
                sourceKind: String(modelData.sourceKind || "")
                maskId: String(modelData.maskId || "")
            }
        }

        Item { Layout.fillHeight: true }
    }
}
