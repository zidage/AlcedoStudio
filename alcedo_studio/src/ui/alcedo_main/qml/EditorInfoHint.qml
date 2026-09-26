import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl

// Small "i" glyph that shows an explanatory tooltip on hover or click.
Item {
    id: root

    property string text: ""
    property color iconColor: appTheme.textMutedColor
    property int iconSize: 14
    // Click pins the tooltip open for pointer devices without hover.
    property bool pinned: false

    implicitWidth: iconSize + 4
    implicitHeight: iconSize + 4

    ColorImage {
        anchors.centerIn: parent
        width: root.iconSize
        height: root.iconSize
        sourceSize.width: root.iconSize * 2
        sourceSize.height: root.iconSize * 2
        source: "qrc:/panel_icons/info.svg"
        color: hover.hovered || tip.visible ? appTheme.textColor : root.iconColor
    }

    HoverHandler {
        id: hover
        cursorShape: Qt.PointingHandCursor
    }

    TapHandler {
        onTapped: root.pinned = !root.pinned
    }

    ToolTip {
        id: tip
        visible: hover.hovered || root.pinned
        delay: root.pinned ? 0 : 300
        text: root.text
        width: Math.min(implicitWidth, 320)
        contentItem: Text {
            text: tip.text
            wrapMode: Text.WordWrap
            color: appTheme.textColor
            font.pixelSize: appTheme.fontSizeCaption
        }
    }

    Accessible.role: Accessible.StaticText
    Accessible.name: root.text
}
