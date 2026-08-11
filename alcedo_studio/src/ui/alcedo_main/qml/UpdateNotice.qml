import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property var updates
    property bool autoCheck: false
    property bool showIdle: false

    readonly property bool hasMessage: updates
                                       && updates.state !== UpdateService.Disabled
                                       && (showIdle
                                           || updates.state === UpdateService.Available
                                           || updates.state === UpdateService.Downloading
                                           || updates.state === UpdateService.Ready
                                           || updates.state === UpdateService.Error)

    visible: hasMessage
    implicitHeight: visible ? content.implicitHeight + appTheme.spaceLg * 2 : 0
    radius: appTheme.controlRadius
    color: appTheme.cardSurfaceColor
    border.width: 1
    border.color: updates && updates.state === UpdateService.Error
                  ? appTheme.dangerColor
                  : appTheme.cardBorderColor

    Timer {
        interval: 700
        repeat: false
        running: root.autoCheck && root.updates && root.updates.enabled
                 && root.updates.state === UpdateService.Idle
        onTriggered: root.updates.CheckForUpdates()
    }

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: appTheme.spaceLg
        spacing: appTheme.spaceMd

        ColumnLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

            Label {
                Layout.fillWidth: true
                text: root.updates ? root.updates.statusText : ""
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightStrong
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: root.updates && root.updates.errorText.length > 0
                text: root.updates ? root.updates.errorText : ""
                color: appTheme.dangerColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: appTheme.spaceXs
                Layout.preferredHeight: 3
                visible: root.updates && root.updates.state === UpdateService.Downloading
                radius: appTheme.badgeRadius
                color: appTheme.dividerColor

                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, root.updates.progress))
                    height: parent.height
                    radius: parent.radius
                    color: appTheme.accentColor
                }
            }
        }

        Button {
            id: notesButton
            visible: root.updates && root.updates.notesUrl.toString().length > 0
                     && (root.updates.state === UpdateService.Available
                         || root.updates.state === UpdateService.Ready)
            text: qsTr("Release notes")
            flat: true
            onClicked: root.updates.OpenReleaseNotes()

            contentItem: Label {
                text: notesButton.text
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: appTheme.controlRadiusSmall
                color: notesButton.down ? appTheme.buttonPressedFillColor
                                        : (notesButton.hovered ? appTheme.buttonHoveredFillColor
                                                               : appTheme.buttonIdleFillColor)
            }
        }

        Button {
            id: actionButton
            visible: root.updates && root.updates.state !== UpdateService.Checking
                     && root.updates.state !== UpdateService.Downloading
                     && root.updates.state !== UpdateService.Installing
            text: {
                if (!root.updates) return ""
                if (root.updates.state === UpdateService.Available) return qsTr("Download update")
                if (root.updates.state === UpdateService.Ready) return qsTr("Install and restart")
                return qsTr("Check again")
            }
            onClicked: {
                if (root.updates.state === UpdateService.Available) {
                    root.updates.DownloadUpdate()
                } else if (root.updates.state === UpdateService.Ready) {
                    root.updates.InstallUpdate()
                } else {
                    root.updates.CheckForUpdates()
                }
            }

            contentItem: Label {
                text: actionButton.text
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: appTheme.controlRadiusSmall
                color: actionButton.down ? appTheme.buttonPressedFillColor
                                         : (actionButton.hovered ? appTheme.buttonHoveredFillColor
                                                                 : appTheme.buttonSelectedFillColor)
            }
        }
    }
}
