import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A wide button of the settings pages that hands a folder to the system: the file manager's own
// icon (image://alcedo-system-icon) with a bundled folder icon as fallback.
Rectangle {
    id: systemButton

    property string text: ""

    signal clicked()

    implicitHeight: 44
    Layout.preferredHeight: 44
    radius: 10
    opacity: enabled ? 1.0 : 0.45
    color: systemMouse.pressed
           ? Qt.rgba(1, 1, 1, 0.06)
           : (systemMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 1, 1, 0.07))
    border.width: 1
    border.color: Qt.rgba(appTheme.textColor.r, appTheme.textColor.g, appTheme.textColor.b, 0.14)

    Row {
        anchors.centerIn: parent
        spacing: 10

        Item {
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20

            Image {
                id: systemIcon
                anchors.fill: parent
                source: "image://alcedo-system-icon/file-manager"
                sourceSize.width: 40
                sourceSize.height: 40
                smooth: true
                mipmap: true
            }
            Image {
                anchors.fill: parent
                // The provider answers 1x1 when the system has no icon.
                visible: systemIcon.status !== Image.Ready || systemIcon.implicitWidth <= 1
                source: "qrc:/panel_icons/folder-open.svg"
                sourceSize.width: 20
                sourceSize.height: 20
            }
        }
        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: systemButton.text
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightStrong
        }
    }

    MouseArea {
        id: systemMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: systemButton.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: systemButton.clicked()
    }
}
