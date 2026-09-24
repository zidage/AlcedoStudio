import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks where a new Mask goes when the selected Color Grade is locked
// (deletion-protected). The default Color Grade carries the application's
// default adjustments and is locked, so a Mask drawn there would also confine
// those defaults to the masked area. User-added layers are not locked and never
// reach this prompt.
//
// Choices: create a new layer above the stack and draw there, draw on the
// current node, or cancel. "Don't ask again" (checked by default) stores the
// chosen action through EditorBehaviorPreferences; Cancel never stores it.
// The same choice is editable later under Settings > Default Behavior.
Popup {
    id: root
    objectName: "lockedNodeMaskPromptDialog"

    property var theme: null
    /// EditorBehaviorPreferences (appModules.editorBehavior); may be null in
    /// isolated harnesses, in which case the choice is not persisted.
    property var editorBehavior: null
    /// False while the node graph has an unfinished draft; a new layer cannot
    /// be inserted then.
    property bool canCreateLayer: true
    /// Mask tool that opened the prompt: "radial" or "linear".
    property string toolKind: ""

    signal newLayerRequested(string toolKind)
    signal currentNodeRequested(string toolKind)

    parent: Overlay.overlay
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: parent
    width: parent ? Math.min(parent.width - (appTheme.spaceXl * 2), 480) : 480
    padding: appTheme.spaceLg

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor

    function openForTool(kind) {
        root.toolKind = kind
        dontAskAgain.checked = true
        root.open()
    }

    function resolve(action) {
        const kind = root.toolKind
        if (dontAskAgain.checked && root.editorBehavior)
            root.editorBehavior.setLockedNodeMaskAction(action)
        root.close()
        if (action === "newLayer")
            root.newLayerRequested(kind)
        else
            root.currentNodeRequested(kind)
    }

    Overlay.modal: Rectangle {
        color: root.theme ? root.theme.colOverlay : appTheme.overlayColor
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: root.theme ? root.theme.colBgPanel : appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.cardBorderColor
    }

    contentItem: ColumnLayout {
        spacing: appTheme.spaceMd

        Label {
            objectName: "lockedNodeMaskPromptTitle"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Add Mask to a Locked Node?")
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightHeading
            color: root.colText
        }

        Label {
            objectName: "lockedNodeMaskPromptBody"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("The default adjustment node applies Alcedo's default adjustments, so it is locked. "
                       + "A Mask drawn on it also limits those default adjustments to the masked area, "
                       + "which differs from traditional editing software.\n\n"
                       + "Create a new layer above it and draw the Mask there?")
            color: root.colMuted
            font.pixelSize: appTheme.fontSizeBody
            font.family: appTheme.uiFontFamily
        }

        ThemeCheckBox {
            id: dontAskAgain
            objectName: "lockedNodeMaskPromptDontAskAgain"
            Layout.fillWidth: true
            checked: true
            alwaysPrimaryText: true
            text: qsTr("Don't ask again")
            onToggled: function(value) { dontAskAgain.checked = value }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("You can change this later in Settings > Default Behavior.")
            color: root.colMuted
            font.pixelSize: appTheme.fontSizeCaption
            font.family: appTheme.uiFontFamily
        }

        Flow {
            Layout.fillWidth: true
            Layout.topMargin: appTheme.spaceSm
            layoutDirection: Qt.RightToLeft
            spacing: appTheme.spaceSm

            DialogActionButton {
                objectName: "lockedNodeMaskPromptNewLayerButton"
                text: qsTr("New Layer, Then Draw")
                kind: "accent"
                enabled: root.canCreateLayer
                buttonWidth: Math.max(appTheme.spaceXl * 5, implicitContentWidth + appTheme.spaceLg * 2)
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.weight: appTheme.fontWeightRegular
                Accessible.name: text
                onClicked: root.resolve("newLayer")
            }

            DialogActionButton {
                objectName: "lockedNodeMaskPromptCurrentNodeButton"
                text: qsTr("Draw on Current Node")
                kind: "normal"
                buttonWidth: Math.max(appTheme.spaceXl * 5, implicitContentWidth + appTheme.spaceLg * 2)
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.weight: appTheme.fontWeightRegular
                Accessible.name: text
                onClicked: root.resolve("currentNode")
            }

            DialogActionButton {
                objectName: "lockedNodeMaskPromptCancelButton"
                text: qsTr("Cancel")
                kind: "normal"
                buttonWidth: appTheme.spaceXl * 4
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.weight: appTheme.fontWeightRegular
                Accessible.name: text
                onClicked: root.close()
            }
        }
    }
}
