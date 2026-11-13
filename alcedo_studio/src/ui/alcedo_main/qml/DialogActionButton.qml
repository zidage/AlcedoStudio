import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// kind: "normal" (raised dark control), "light" (the main action of a neutral page or dialog:
// light fill, dark text; DESIGN.md "Neutral actions"), "accent", "danger", "warning".
// iconSource: an optional monochrome SVG before the text, tinted with the text color.
Button {
    id: control

    property string kind: "normal"
    property int buttonWidth: 168
    property int buttonHeight: 46
    property int buttonRadius: 10
    property url iconSource: ""
    property int iconSize: 16
    readonly property bool hasIcon: iconSource.toString().length > 0

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    function baseColor() {
        if (kind === "accent") {
            return appTheme.accentColor
        }
        if (kind === "danger") {
            return "#7A2532"
        }
        if (kind === "warning") {
            return appTheme.dangerColor
        }
        if (kind === "light") {
            return appTheme.textColor
        }
        return appTheme.controlFillColor
    }

    implicitWidth: buttonWidth
    implicitHeight: Math.max(buttonHeight,
                             buttonLabel.implicitHeight + appTheme.spaceMd)
    Layout.preferredWidth: buttonWidth
    Layout.preferredHeight: implicitHeight
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0
    padding: 0
    hoverEnabled: true
    font.family: appTheme.uiFontFamily
    font.pixelSize: appTheme.fontSizeSection
    font.weight: appTheme.fontWeightStrong

    // With an icon, the label keeps the icon space as left padding and the icon stands left of
    // the text, so the icon and the text are centered together.
    contentItem: Item {
        readonly property real iconSpace: control.hasIcon ? control.iconSize + appTheme.spaceSm : 0
        readonly property color inkColor: !control.enabled
                                          ? appTheme.textMutedColor
                                          : (control.kind === "light" ? appTheme.bgCanvasColor
                                                                       : "#FFFFFF")

        implicitWidth: buttonLabel.implicitWidth
        implicitHeight: buttonLabel.implicitHeight

        Label {
            id: buttonLabel
            anchors.fill: parent
            leftPadding: parent.iconSpace
            text: control.text
            color: parent.inkColor
            font: control.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }

        ColorImage {
            visible: control.hasIcon
            width: control.iconSize
            height: control.iconSize
            x: parent.iconSpace + (parent.width - parent.iconSpace - buttonLabel.contentWidth) / 2
               - parent.iconSpace
            anchors.verticalCenter: parent.verticalCenter
            source: control.iconSource
            sourceSize.width: control.iconSize * 2
            sourceSize.height: control.iconSize * 2
            color: parent.inkColor
        }
    }

    background: Rectangle {
        radius: control.buttonRadius
        color: {
            const base = control.baseColor()
            if (!control.enabled) {
                return control.withAlpha(base, 0.38)
            }
            if (control.kind === "normal") {
                // Raised control fill: reads apart from card and panel surfaces.
                if (control.down) {
                    return appTheme.controlPressedFillColor
                }
                return control.hovered ? appTheme.controlHoveredFillColor : base
            }
            if (control.down) {
                return Qt.darker(base, 1.14)
            }
            if (control.hovered) {
                // A light fill is already near white: hover steps it down instead.
                return control.kind === "light" ? Qt.darker(base, 1.06) : Qt.lighter(base, 1.08)
            }
            return base
        }
        border.width: control.kind === "normal" ? 1 : 0
        border.color: appTheme.controlBorderColor
    }
}
