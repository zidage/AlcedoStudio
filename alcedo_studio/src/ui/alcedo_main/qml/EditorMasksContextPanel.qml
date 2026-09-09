import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Temporary Color Grade Masks body. Overlays the ordinary six-tab stack
// without writing "masks" into QSettings. Radial inner/outer values are
// percent of the base radius. Selection/load is adapter-owned.
Item {
    id: root
    objectName: "editorAdjustmentPanel_masks"

    property var theme: null
    property var editorSession: null
    property var nodeController: null
    property var maskCreation: null
    property bool controlsEnabled: true

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property var masks: {
        if (!root.nodeController)
            return []
        return root.nodeController.selectedNodeMasks || []
    }
    readonly property bool radialSelected: root.maskCreation
            && String(root.maskCreation.toolKind || "") === "radial"
            && String(root.maskCreation.selectedMaskId || "").length > 0

    function loadFromSnapshot(snapshot) {
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
                selected: root.maskCreation
                          && maskId === String(root.maskCreation.selectedMaskId || "")
                onClicked: {
                    if (root.maskCreation)
                        root.maskCreation.selectMask("", maskId)
                }
                onDeleteClicked: {
                    if (root.maskCreation)
                        root.maskCreation.removeMask("", maskId)
                }
            }
        }

        Label {
            objectName: "editorMasksInnerFeatherLabel"
            Layout.fillWidth: true
            visible: root.radialSelected
            text: qsTr("Inner feather")
            color: root.colText
            font.pixelSize: appTheme.fontSizeCaption
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Inner feather")
        }

        EditorMonoSlider {
            id: innerFeatherSlider
            objectName: "editorMasksInnerFeatherSlider"
            Layout.fillWidth: true
            visible: root.radialSelected
            enabled: root.controlsEnabled && root.radialSelected
            from: 0
            to: 100
            stepSize: 1
            pointerGain: 1
            rowHeight: 28
            handleSize: 18
            externalValue: root.maskCreation ? root.maskCreation.innerFeatherPercent : 0
            onBegin: {
                if (root.maskCreation)
                    root.maskCreation.beginInnerFeather()
            }
            onUpdate: function (v) {
                if (root.maskCreation)
                    root.maskCreation.updateInnerFeather(v)
            }
            onFinish: {
                if (root.maskCreation)
                    root.maskCreation.finishFeather()
            }
            onReset: {
                if (root.maskCreation) {
                    root.maskCreation.beginInnerFeather()
                    root.maskCreation.updateInnerFeather(0)
                    root.maskCreation.finishFeather()
                }
            }
        }

        Label {
            objectName: "editorMasksInnerFeatherValue"
            Layout.fillWidth: true
            visible: root.radialSelected
            text: qsTr("%1 percent").arg(
                      Math.round(root.maskCreation ? root.maskCreation.innerFeatherPercent : 0))
            color: root.colMuted
            font.pixelSize: appTheme.fontSizeCaption
        }

        Label {
            objectName: "editorMasksOuterFeatherLabel"
            Layout.fillWidth: true
            visible: root.radialSelected
            text: qsTr("Outer feather")
            color: root.colText
            font.pixelSize: appTheme.fontSizeCaption
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Outer feather")
        }

        EditorMonoSlider {
            id: outerFeatherSlider
            objectName: "editorMasksOuterFeatherSlider"
            Layout.fillWidth: true
            visible: root.radialSelected
            enabled: root.controlsEnabled && root.radialSelected
            from: 0
            to: 400
            stepSize: 1
            pointerGain: 1
            rowHeight: 28
            handleSize: 18
            externalValue: {
                if (!root.maskCreation)
                    return 0
                return Math.min(400, root.maskCreation.outerFeatherPercent)
            }
            onBegin: {
                if (root.maskCreation)
                    root.maskCreation.beginOuterFeather()
            }
            onUpdate: function (v) {
                if (root.maskCreation)
                    root.maskCreation.updateOuterFeather(v)
            }
            onFinish: {
                if (root.maskCreation)
                    root.maskCreation.finishFeather()
            }
            onReset: {
                if (root.maskCreation) {
                    root.maskCreation.beginOuterFeather()
                    root.maskCreation.updateOuterFeather(0)
                    root.maskCreation.finishFeather()
                }
            }
        }

        Label {
            objectName: "editorMasksOuterFeatherValue"
            Layout.fillWidth: true
            visible: root.radialSelected
            text: qsTr("%1 percent").arg(
                      Math.round(root.maskCreation ? root.maskCreation.outerFeatherPercent : 0))
            color: root.colMuted
            font.pixelSize: appTheme.fontSizeCaption
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            DialogActionButton {
                objectName: "editorMasksDoneButton"
                Layout.fillWidth: true
                buttonWidth: 0
                text: qsTr("Done")
                kind: "accent"
                enabled: root.controlsEnabled
                onClicked: {
                    if (root.maskCreation)
                        root.maskCreation.finishBody()
                }
            }

            DialogActionButton {
                objectName: "editorMasksCancelButton"
                Layout.fillWidth: true
                buttonWidth: 0
                text: qsTr("Cancel")
                enabled: root.controlsEnabled
                onClicked: {
                    if (root.maskCreation)
                        root.maskCreation.cancel()
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
