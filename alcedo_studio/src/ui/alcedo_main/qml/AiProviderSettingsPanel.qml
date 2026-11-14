import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Controls.impl
import QtQuick.Layouts
import QtQuick.Effects

// Saved AI Keys settings page (issue #710). Advanced Content Analysis is removed; this page lists
// the provider profiles that the user saved for it and deletes their API keys and their Codex
// OAuth login from the OS credential store. The next release removes this page.
Item {
    id: panel
    objectName: "savedAiKeysPanel"

    property var profileController: null       // appModules.aiProviderProfiles
    property color primaryAccent: "#457B9D"
    property color secondaryAccent: "#9FC7D8"
    property color textColor: "#F5F1EA"
    property color mutedTextColor: "#B6B0A7"
    property color dividerColor: Qt.rgba(1, 1, 1, 0.08)
    property color dangerColor: "#D96C75"
    property string dataFontFamily: appTheme.dataFontFamily
    property Item backgroundSource: null
    property string deletingProfileId: ""
    readonly property bool hasProfilesController: profileController !== null
    readonly property var profiles: hasProfilesController ? profileController.profiles : []
    readonly property bool hasSavedCredentials: hasProfilesController
                                                && profileController.hasSavedCredentials

    // Reusable rounded-rectangle action button. Danger = destructive bg with white text,
    // otherwise gray bg with white text. Content-sized so labels never elide.
    component KeyButton: Button {
        id: keyBtn
        property bool danger: false

        Layout.preferredHeight: Math.max(36, implicitHeight)
        font.pixelSize: appTheme.fontSizeTitle
        font.weight: appTheme.fontWeightStrong
        hoverEnabled: true
        leftPadding: 16
        rightPadding: 16
        topPadding: 6
        bottomPadding: 6
        contentItem: Label {
            text: keyBtn.text
            font: keyBtn.font
            color: panel.textColor
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 10
            color: keyBtn.danger
                   ? (keyBtn.down
                      ? Qt.darker(panel.dangerColor, 1.16)
                      : (keyBtn.hovered ? Qt.lighter(panel.dangerColor, 1.06) : panel.dangerColor))
                   : (keyBtn.down
                      ? Qt.rgba(1, 1, 1, 0.06)
                      : (keyBtn.hovered ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(1, 1, 1, 0.10)))
            border.width: 1
            border.color: keyBtn.danger
                          ? Qt.rgba(panel.dangerColor.r, panel.dangerColor.g, panel.dangerColor.b, 0.30)
                          : Qt.rgba(1, 1, 1, 0.12)
            opacity: keyBtn.enabled ? 1.0 : 0.45
        }
    }

    // Blurred modal backdrop shared by the page dialogs; matches the MultiEffect and
    // overlayColor of SettingDialog.
    component BlurOverlay: Item {
        anchors.fill: parent

        MultiEffect {
            visible: panel.backgroundSource !== null
            anchors.fill: parent
            source: panel.backgroundSource
            blurEnabled: true
            blur: 0.6
            blurMax: 64
            saturation: -0.2
            brightness: -0.08
        }

        Rectangle {
            anchors.fill: parent
            color: appTheme.overlayColor
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
        }
    }

    component ConfirmDialog: Dialog {
        id: confirm
        property string titleText: ""
        property string bodyText: ""
        property string confirmText: ""
        default property alias extraContent: extraColumn.data
        signal confirmed()

        parent: Overlay.overlay
        modal: true
        focus: true
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0
        width: Math.min((parent ? parent.width : 520) - 72, 460)
        padding: 24

        Overlay.modal: Component { BlurOverlay {} }

        background: Rectangle {
            radius: 14
            color: appTheme.toneGraphite
            border.width: 0
        }

        contentItem: ColumnLayout {
            spacing: 14

            Label {
                Layout.fillWidth: true
                text: confirm.titleText
                color: appTheme.textColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: appTheme.fontSizeHeadline
                font.weight: appTheme.fontWeightStrong
            }

            Label {
                Layout.fillWidth: true
                text: confirm.bodyText
                color: appTheme.textMutedColor
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightRegular
                wrapMode: Text.WordWrap
            }

            ColumnLayout {
                id: extraColumn
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Item { Layout.fillWidth: true }

                KeyButton {
                    text: qsTr("Cancel")
                    onClicked: confirm.close()
                }

                KeyButton {
                    objectName: "confirmDeleteButton"
                    danger: true
                    text: confirm.confirmText
                    onClicked: {
                        confirm.confirmed()
                        confirm.close()
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 34
        anchors.rightMargin: 34
        anchors.topMargin: 26
        anchors.bottomMargin: 26
        spacing: 16

        AlertBadge {
            objectName: "savedAiKeysNotice"
            Layout.fillWidth: true
            text: qsTr("Advanced Content Analysis is removed from this version. The next version also removes this page. Delete your saved API keys and the Codex login now. After that, Alcedo Studio can no longer delete them for you.")
        }

        Label {
            objectName: "savedAiKeysEmptyLabel"
            Layout.fillWidth: true
            visible: !panel.hasSavedCredentials
            text: qsTr("No saved keys")
            color: panel.mutedTextColor
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightStrong
            wrapMode: Text.Wrap
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: profileList
                objectName: "savedAiKeysList"
                anchors.fill: parent
                clip: true
                spacing: 12
                boundsBehavior: Flickable.StopAtBounds
                model: panel.profiles

                delegate: Rectangle {
                    id: profileRow
                    required property var modelData
                    objectName: "savedAiKeyRow_" + modelData.uuid

                    width: ListView.view.width
                    height: rowLayout.implicitHeight + appTheme.spaceMd * 2
                    radius: 8
                    color: Qt.rgba(0, 0, 0, 0.16)
                    border.width: 1
                    border.color: Qt.rgba(1, 1, 1, 0.08)

                    RowLayout {
                        id: rowLayout
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 14
                        anchors.topMargin: appTheme.spaceMd
                        anchors.bottomMargin: appTheme.spaceMd
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 5

                            Label {
                                Layout.fillWidth: true
                                text: profileRow.modelData.displayName
                                color: panel.textColor
                                font.pixelSize: appTheme.fontSizeTitle
                                font.weight: appTheme.fontWeightStrong
                                wrapMode: Text.Wrap
                            }

                            Label {
                                Layout.fillWidth: true
                                text: profileRow.modelData.isCodexOAuth
                                      ? qsTr("ChatGPT / Codex OAuth")
                                      : profileRow.modelData.baseUrl
                                color: panel.mutedTextColor
                                font.family: panel.dataFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                                wrapMode: Text.Wrap
                            }

                            Label {
                                Layout.fillWidth: true
                                text: profileRow.modelData.credentialAvailable
                                      ? (profileRow.modelData.maskedKeyLabel.length > 0
                                         ? profileRow.modelData.maskedKeyLabel
                                         : qsTr("Key saved"))
                                      : qsTr("No key saved")
                                color: profileRow.modelData.credentialAvailable
                                       ? panel.secondaryAccent : panel.mutedTextColor
                                font.family: panel.dataFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                wrapMode: Text.Wrap
                            }
                        }

                        KeyButton {
                            objectName: "deleteKeyButton"
                            text: profileRow.modelData.isCodexOAuth ? qsTr("Disconnect") : qsTr("Delete Key")
                            enabled: panel.hasProfilesController
                                     && profileRow.modelData.credentialAvailable
                            onClicked: panel.profileController.DeleteApiKey(profileRow.modelData.uuid)
                        }

                        KeyButton {
                            objectName: "deleteProfileButton"
                            danger: true
                            text: qsTr("Delete")
                            enabled: panel.hasProfilesController
                            onClicked: {
                                panel.deletingProfileId = profileRow.modelData.uuid
                                wipeKeyCheck.checked = true
                                deleteDialog.open()
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: panel.profiles.length > 0

            KeyButton {
                objectName: "deleteAllSavedKeysButton"
                danger: true
                text: qsTr("Delete All Saved Keys")
                enabled: panel.hasSavedCredentials
                onClicked: deleteAllDialog.open()
            }

            Item { Layout.fillWidth: true }
        }
    }

    ConfirmDialog {
        id: deleteDialog
        objectName: "deleteProfileDialog"
        titleText: qsTr("Delete provider")
        bodyText: qsTr("Delete this provider profile?")
        confirmText: qsTr("Delete")
        onConfirmed: {
            if (panel.hasProfilesController && panel.deletingProfileId.length > 0) {
                panel.profileController.DeleteProfile(panel.deletingProfileId, wipeKeyCheck.checked)
            }
            panel.deletingProfileId = ""
        }

        CheckBox {
            id: wipeKeyCheck
            checked: true
            text: qsTr("Delete saved key")
            Material.foreground: appTheme.textColor
        }
    }

    ConfirmDialog {
        id: deleteAllDialog
        objectName: "deleteAllSavedKeysDialog"
        titleText: qsTr("Delete All Saved Keys")
        bodyText: qsTr("Delete every saved API key and the Codex login? The provider profiles stay in the list.")
        confirmText: qsTr("Delete All")
        onConfirmed: {
            if (panel.hasProfilesController) {
                panel.profileController.DeleteAllApiKeys()
            }
        }
    }
}
