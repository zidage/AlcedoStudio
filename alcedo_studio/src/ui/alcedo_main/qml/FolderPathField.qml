import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A folder path shown in a field with a square folder button on its right,
// shared by the New Project storage location and the LUT library folder.
// The button only requests a folder; the caller opens its own FolderDialog.
// Callers reach the parts through `pathLabel` and `browseButton` (for example
// to set their objectName for tests).
RowLayout {
    id: field

    property string path: ""
    property string placeholderText: ""
    property int controlHeight: 56
    property int pathPixelSize: 19
    property int iconSize: 24
    property string iconSource: "qrc:/panel_icons/folder-open.svg"
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property string pathFontFamily: appTheme.headlineFontFamily
    property bool browseEnabled: true
    property string browseToolTip: ""
    readonly property alias pathLabel: pathText
    readonly property alias browseButton: browseButton

    signal browseRequested()

    spacing: Math.round(controlHeight * 0.28)

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(field.controlHeight, pathText.implicitHeight + appTheme.spaceMd)
        radius: 10
        color: Qt.rgba(1, 1, 1, 0.10)
        border.width: 1
        border.color: Qt.rgba(field.textColor.r, field.textColor.g, field.textColor.b, 0.12)

        Label {
            id: pathText
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            text: field.path.length > 0 ? field.path : field.placeholderText
            wrapMode: Text.WrapAnywhere
            verticalAlignment: Text.AlignVCenter
            color: field.path.length > 0 ? field.textColor : field.mutedTextColor
            font.family: field.pathFontFamily
            font.pixelSize: field.pathPixelSize
        }
    }

    Rectangle {
        id: browseButton

        signal clicked()

        Layout.preferredWidth: field.controlHeight
        Layout.preferredHeight: field.controlHeight
        Layout.alignment: Qt.AlignTop
        enabled: field.browseEnabled
        opacity: enabled ? 1.0 : 0.45
        radius: 10
        color: browseMouse.pressed
               ? Qt.rgba(1, 1, 1, 0.06)
               : (browseMouse.containsMouse
                  ? Qt.rgba(1, 1, 1, 0.12)
                  : Qt.rgba(1, 1, 1, 0.07))
        border.width: 1
        border.color: Qt.rgba(field.textColor.r, field.textColor.g, field.textColor.b, 0.14)

        ToolTip.text: field.browseToolTip
        ToolTip.visible: browseMouse.containsMouse && field.browseToolTip.length > 0
        ToolTip.delay: 400

        onClicked: field.browseRequested()

        Image {
            anchors.centerIn: parent
            width: field.iconSize
            height: field.iconSize
            source: field.iconSource
            sourceSize.width: field.iconSize
            sourceSize.height: field.iconSize
            asynchronous: true
        }

        MouseArea {
            id: browseMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: browseButton.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: browseButton.clicked()
        }
    }
}
