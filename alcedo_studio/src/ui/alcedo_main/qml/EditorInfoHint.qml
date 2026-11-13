import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl

// Small "i" glyph that shows an explanatory tooltip on hover or click.
// The tooltip uses the app theme (card surface, theme text, divider border),
// the same recipe as the SettingDialog info badge. It sits in the window
// overlay, next to the glyph: below it and right-aligned to it, flipped above
// or clamped when the window edge is near.
Item {
    id: root

    property string text: ""
    property color iconColor: appTheme.textMutedColor
    property int iconSize: 14
    property int popupWidth: 280
    // Click pins the tooltip open for pointer devices without hover.
    property bool pinned: false

    readonly property int popupGap: 6
    readonly property int popupMargin: 8

    implicitWidth: iconSize + 4
    implicitHeight: iconSize + 4

    function popupX() {
        const overlay = Overlay.overlay
        if (overlay === null)
            return 0
        const iconRight = root.mapToItem(overlay, root.width, 0).x
        const maxX = overlay.width - tip.width - root.popupMargin
        return Math.max(root.popupMargin, Math.min(iconRight - tip.width, maxX))
    }

    function popupY() {
        const overlay = Overlay.overlay
        if (overlay === null)
            return 0
        const iconTop = root.mapToItem(overlay, 0, 0).y
        const below = iconTop + root.height + root.popupGap
        if (below + tip.height <= overlay.height - root.popupMargin)
            return below
        return Math.max(root.popupMargin, iconTop - tip.height - root.popupGap)
    }

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
        parent: Overlay.overlay
        visible: (hover.hovered || root.pinned) && Overlay.overlay !== null
        delay: root.pinned ? 0 : 260
        timeout: -1
        text: root.text
        // `visible` in the binding re-maps the glyph position each time the tip opens,
        // so a scrolled panel still places the tip next to the glyph.
        x: visible ? root.popupX() : 0
        y: visible ? root.popupY() : 0
        width: root.popupWidth
        padding: 12
        contentItem: Label {
            text: tip.text
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            wrapMode: Text.WordWrap
            lineHeight: 1.22
        }
        background: Rectangle {
            color: Qt.rgba(appTheme.cardSurfaceColor.r, appTheme.cardSurfaceColor.g,
                           appTheme.cardSurfaceColor.b, 0.98)
            radius: appTheme.controlRadius
            border.width: 1
            border.color: appTheme.dividerColor
        }
    }

    Accessible.role: Accessible.StaticText
    Accessible.name: root.text
}
