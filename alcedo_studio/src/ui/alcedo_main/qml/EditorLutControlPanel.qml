import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Small Editor LUT control: the `lut` page of the adjustment stack (LUT library
// plan L6A, section 6.5). Shows the selected Color Grade's LUT, its print, and
// the Missing state; adjusts the LUT strength (0-100 %); opens the LUT browser
// rail page; and removes the LUT. Choosing a LUT belongs to the browser.
//
// Loading: the association and strength come from the shared LutLibraryController
// (appModules.lutTarget), which reads the published document. Loading never
// submits. Strength writes go through EditorLutAdjustmentModel as strength-only
// `lut` writes on the existing adjustment path (interactive previews and one
// settled commit per drag).
Item {
    id: root
    objectName: "editorAdjustmentPanel_lut"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true
    property var target: (typeof appModules !== "undefined" && appModules && appModules.lutTarget)
                         ? appModules.lutTarget : null

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property bool canApply: !!target && target.canApply === true
    readonly property bool hasAssociation: !!target && target.hasAssociation === true

    // The strength follows the target; the stack's snapshot fan-out needs no work here.
    function loadFromSnapshot(snapshot) {
    }

    function openBrowser() {
        if (root.editorSession)
            root.editorSession.editorToolPanelPage = "luts"
    }

    EditorLutAdjustmentModel {
        id: strengthModel
        objectName: "editorLutStrengthModel"
        target: root.target ? root.target : null
        submitter: root.editorSession
        label: qsTr("Strength")
        enabled: root.controlsEnabled && root.canApply && root.hasAssociation
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: appTheme.spaceSm

        Label {
            objectName: "editorLutControlTitle"
            Layout.fillWidth: true
            text: qsTr("LUT")
            color: root.colText
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightHeading
        }

        Label {
            objectName: "editorLutControlTargetMessage"
            Layout.fillWidth: true
            visible: !root.canApply && text.length > 0
            text: root.target ? String(root.target.targetMessage) : ""
            color: root.colMuted
            wrapMode: Text.Wrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Label {
                objectName: "editorLutControlName"
                Layout.fillWidth: true
                text: root.hasAssociation ? String(root.target.associationName) : qsTr("No LUT applied")
                color: root.hasAssociation ? root.colText : root.colMuted
                wrapMode: Text.Wrap
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
            }

            Label {
                objectName: "editorLutControlPrint"
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.target ? String(root.target.associationPrintName || "") : ""
                color: root.colMuted
                wrapMode: Text.Wrap
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }

            Label {
                objectName: "editorLutControlMissing"
                Layout.fillWidth: true
                visible: !!root.target && root.target.associationMissing === true
                text: qsTr("The LUT file is missing. The photo renders without it until the file returns.")
                color: appTheme.dangerColor
                wrapMode: Text.Wrap
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }
        }

        AdjustmentSlider {
            objectName: "editorLutStrengthSlider"
            Layout.fillWidth: true
            visible: root.hasAssociation
            model: strengthModel
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            DialogActionButton {
                objectName: "editorLutBrowseButton"
                Layout.fillWidth: true
                buttonHeight: appTheme.iconButtonHitSizeCompact
                font.pixelSize: appTheme.fontSizeBody
                text: qsTr("Browse LUTs")
                enabled: !!root.editorSession
                onClicked: root.openBrowser()
            }

            DialogActionButton {
                objectName: "editorLutControlRemoveButton"
                Layout.fillWidth: true
                buttonHeight: appTheme.iconButtonHitSizeCompact
                font.pixelSize: appTheme.fontSizeBody
                visible: root.hasAssociation
                enabled: root.controlsEnabled && root.canApply
                text: qsTr("Remove")
                onClicked: root.target.clearAssociation()
            }
        }
    }
}
