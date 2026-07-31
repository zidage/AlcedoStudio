pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root
    objectName: "acceleratorRestartDialog"

    property Item blurSource: null
    property string backendLabel: ""

    signal restartRequested()
    signal continueRequested()

    parent: Overlay.overlay
    modal: true
    focus: true
    title: ""
    padding: 0
    closePolicy: Popup.CloseOnEscape
    width: Math.min(parent ? parent.width - appTheme.spaceXl * 2
                           : appTheme.acceleratorRestartDialogWidth,
                    appTheme.acceleratorRestartDialogWidth)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    footer: Item {
        width: 1
        height: 0
    }

    onRejected: root.continueRequested()

    background: Rectangle {
        radius: appTheme.panelRadius
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.cardBorderColor
    }

    Overlay.modal: BlurredOverlay {
        anchors.fill: parent
        blurSource: root.blurSource
        overlayColor: appTheme.overlayColor
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.topMargin: appTheme.spaceLg
            Layout.bottomMargin: appTheme.spaceMd
            spacing: appTheme.spaceMd

            Rectangle {
                Layout.preferredWidth: appTheme.spaceXs
                Layout.preferredHeight: appTheme.spaceXl * 2
                Layout.alignment: Qt.AlignTop
                radius: appTheme.controlRadiusSmall
                color: appTheme.accentColor
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Restart required")
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeHeadline
                    font.weight: appTheme.fontWeightHeading
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Acceleration backend change")
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeTitle
                    font.weight: appTheme.fontWeightStrong
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: appTheme.dividerColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.topMargin: appTheme.spaceLg
            Layout.bottomMargin: appTheme.spaceMd
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: qsTr("Alcedo will use %1 after the next launch. Close the application and start it again to apply this change.").arg(root.backendLabel)
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightRegular
                lineHeight: appTheme.lineHeightBody
                wrapMode: Text.WordWrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: appTheme.dividerColor
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.topMargin: appTheme.spaceMd
            Layout.bottomMargin: appTheme.spaceLg
            spacing: appTheme.spaceSm

            Item {
                Layout.fillWidth: true
            }

            DialogActionButton {
                id: continueButton
                objectName: "acceleratorRestartContinueButton"
                kind: "normal"
                buttonWidth: appTheme.spaceXl * 7
                buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                text: qsTr("Continue for now")
                activeFocusOnTab: true
                Accessible.name: text
                onClicked: {
                    root.close()
                    root.continueRequested()
                }
            }

            DialogActionButton {
                id: exitButton
                objectName: "acceleratorRestartExitButton"
                kind: "accent"
                buttonWidth: appTheme.spaceXl * 7
                buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                text: qsTr("Exit Alcedo")
                activeFocusOnTab: true
                Accessible.name: text
                onClicked: root.restartRequested()
            }
        }
    }
}
