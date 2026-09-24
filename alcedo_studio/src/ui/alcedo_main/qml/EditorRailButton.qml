import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl

// Labeled tool-rail action: SVG icon with its function name printed underneath,
// so the vertical editor rail reads without hovering for tooltips.
//
// Geometry (AppTheme tokens — do not hardcode px here):
//   width:   set by the caller (rail inner width); the whole tile is the hit
//            target, hover fill, and selected outline.
//   icon:    compact optical/source tokens inside a chromeSize well, matching
//            IconActionButton compact so rail glyphs keep the shared weight.
//   label:   fontSizeCaption, centered, up to two lines, elided beyond that.
//
// Root is an Item (not a Controls Button) for the same reason as
// IconActionButton: Material Button padding would distort the tile.
Item {
    id: control

    property bool selected: false
    property bool showFocusRing: true
    property string label: ""
    property string actionName: label
    property string toolTipText: actionName
    property url iconSrc: ""
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property color fillIdle: appTheme.cardSurfaceColor
    property color fillHover: appTheme.buttonHoveredFillColor
    property color fillPressed: appTheme.buttonPressedFillColor
    property color selectedOutlineColor: textColor
    property color focusRingColor: textColor

    readonly property int opticalSize: appTheme.iconOpticalSizeCompact
    readonly property int sourceSize: appTheme.iconSourceSizeCompact
    readonly property int chromeSize: appTheme.iconButtonHitSizeCompact - 8
    readonly property int tilePadding: appTheme.spaceXs
    readonly property Item iconItem: iconImage
    readonly property Item labelItem: labelText

    readonly property color _inkColor: {
        if (!control.enabled)
            return control.mutedColor
        return control.selected ? control.textColor : control.mutedColor
    }

    readonly property color fillColor: {
        if (!control.enabled || control.selected)
            return control.fillIdle
        if (pressArea.pressed)
            return control.fillPressed
        if (hover.hovered || pressArea.containsMouse)
            return control.fillHover
        return control.fillIdle
    }

    signal clicked()

    implicitWidth: appTheme.iconButtonHitSizeCompact + 2 * tilePadding
    implicitHeight: tilePadding + chromeSize + labelText.implicitHeight + 2 * tilePadding

    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: control.actionName
    Accessible.description: control.label
    Accessible.onPressAction: {
        if (control.enabled)
            control.clicked()
    }

    Keys.onPressed: function (event) {
        if (!control.enabled)
            return
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                || event.key === Qt.Key_Enter) {
            control.clicked()
            event.accepted = true
        }
    }

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        color: control.fillColor
        border.width: control.selected
                      || (control.showFocusRing && control.enabled && control.activeFocus) ? 1 : 0
        border.color: control.selected
                      ? control.selectedOutlineColor
                      : Qt.rgba(control.focusRingColor.r, control.focusRingColor.g,
                                control.focusRingColor.b, 0.60)
    }

    Item {
        id: iconWell
        anchors.top: parent.top
        anchors.topMargin: control.tilePadding
        anchors.horizontalCenter: parent.horizontalCenter
        width: control.chromeSize
        height: control.chromeSize

        ColorImage {
            id: iconImage
            anchors.centerIn: parent
            width: control.opticalSize
            height: control.opticalSize
            source: control.iconSrc
            sourceSize.width: control.sourceSize
            sourceSize.height: control.sourceSize
            fillMode: Image.PreserveAspectFit
            smooth: true
            visible: control.iconSrc.toString().length > 0
            color: control._inkColor
            opacity: control.enabled ? 1.0 : 0.55
        }
    }

    Text {
        id: labelText
        anchors.top: iconWell.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 2
        anchors.rightMargin: 2
        text: control.label
        color: control._inkColor
        opacity: control.enabled ? 1.0 : 0.55
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        font.weight: control.selected ? Font.DemiBold : Font.Normal
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
        lineHeight: 0.95
    }

    HoverHandler {
        id: hover
    }

    MouseArea {
        id: pressArea
        objectName: control.objectName.length > 0 ? control.objectName + "Input" : ""
        anchors.fill: parent
        hoverEnabled: true
        enabled: control.enabled
        cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onPressed: control.forceActiveFocus(Qt.MouseFocusReason)
        onClicked: control.clicked()
    }

    ToolTip.visible: (hover.hovered || pressArea.containsMouse) && control.toolTipText.length > 0
    ToolTip.text: control.toolTipText
    ToolTip.delay: 600
}
