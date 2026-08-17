import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

// Application-close confirmation while the editor has an open image.
// Save explicitly finalizes the editor; ordinary Library routing preserves the
// session. The host waits on sessionState Leaving Saving like the filmstrip.
// Discard → Finalize(false). Cancel leaves the editor session alone.
Popup {
    id: root
    objectName: "editorCloseConfirmDialog"

    property var theme: null
    property var host: null
    property Item blurSource: null
    property bool busy: false

    signal saveRequested()
    signal discardRequested()
    signal cancelled()

    modal: true
    focus: true
    closePolicy: busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    anchors.centerIn: parent
    width: parent ? Math.min(parent.width - (appTheme.spaceXl * 2), 440) : 440
    padding: appTheme.spaceLg

    property bool _resolved: false

    Overlay.modal: Item {
        anchors.fill: parent

        MultiEffect {
            anchors.fill: parent
            source: root.blurSource
            blurEnabled: true
            blur: 0.6
            blurMax: 64
            saturation: -0.2
        }

        Rectangle {
            anchors.fill: parent
            color: root.theme ? root.theme.colOverlay : appTheme.overlayColor
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            enabled: !root.busy
        }
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: root.theme ? root.theme.colBgPanel : appTheme.cardSurfaceColor
        border.width: 0
    }

    onClosed: {
        if (!root._resolved) {
            root.cancelled()
        }
        root._resolved = false
        root.busy = false
    }

    function openForClose() {
        root._resolved = false
        root.busy = false
        root.open()
    }

    contentItem: ColumnLayout {
        id: contentCol
        width: root.availableWidth
        spacing: appTheme.spaceMd

        Label {
            objectName: "editorCloseConfirmTitle"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Unsaved Edits")
            font.family: root.theme ? root.theme.headlineFontFamily : appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightHeading
            color: root.theme ? root.theme.colText : appTheme.textColor
        }

        Label {
            objectName: "editorCloseConfirmBody"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.busy
                  ? qsTr("Saving current edits…")
                  : qsTr("Save edits for the current image before quitting, or discard them.")
            color: root.theme ? root.theme.colTextMuted : appTheme.textMutedColor
            font.pixelSize: appTheme.fontSizeBody
            font.family: root.theme ? root.theme.uiFontFamily : appTheme.uiFontFamily
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: appTheme.spaceSm
            spacing: appTheme.spaceSm

            Item { Layout.fillWidth: true }

            DialogActionButton {
                objectName: "editorCloseCancelButton"
                text: qsTr("Cancel")
                kind: "normal"
                enabled: !root.busy
                buttonWidth: appTheme.spaceXl * 4
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.weight: appTheme.fontWeightRegular
                Accessible.name: text
                onClicked: {
                    root._resolved = true
                    root.close()
                    root.cancelled()
                }
            }

            DialogActionButton {
                objectName: "editorCloseDiscardButton"
                text: qsTr("Discard")
                kind: "danger"
                enabled: !root.busy
                buttonWidth: appTheme.spaceXl * 4
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.weight: appTheme.fontWeightRegular
                Accessible.name: text
                onClicked: {
                    root._resolved = true
                    root.close()
                    root.discardRequested()
                }
            }

            DialogActionButton {
                objectName: "editorCloseSaveButton"
                text: qsTr("Save")
                kind: "accent"
                enabled: !root.busy
                buttonWidth: appTheme.spaceXl * 4
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.weight: appTheme.fontWeightRegular
                Accessible.name: text
                onClicked: {
                    root._resolved = true
                    root.saveRequested()
                }
            }
        }
    }
}
