import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

// Dedicated icon-only square button — kept separate from DialogActionButton
// (a text button) so the two styles don't share sizing/padding logic.
//
// Root is an Item (not a Button) so the square shape is exact: Button's
// Material style applies its own minimum padding/implicit sizing that tends
// to stretch a custom contentItem into a rectangle. An Item with explicit
// implicit + Layout sizes stays square in both anchored and Layout contexts.
//
// Two color schemes via `kind`: "accent" (primary action) and "normal"
// (secondary). Each falls back to the app theme color when its *Color
// override is left transparent, so callers can match a local palette.
Item {
    id: control

    // Defaults follow DESIGN.md structural geometry; callers may still override.
    property real buttonSize: appTheme.iconButtonHitSizeCompact
    property real buttonWidth: buttonSize
    property real buttonHeight: buttonSize
    property int buttonRadius: appTheme.controlRadius
    property int iconSize: appTheme.iconOpticalSizeCompact
    property string iconSrc: ""
    property string kind: "normal"           // "accent" | "normal"
    property color accentColor: "transparent"   // override; transparent => appTheme.accentColor
    property color normalColor: "transparent"   // override; transparent => appTheme.bgBaseColor
    property color iconColor: appTheme.iconColor
    property bool bordered: true
    property color borderColor: Qt.rgba(1, 1, 1, 0.12)
    property string tooltipText: ""
    signal clicked()

    implicitWidth: control.buttonWidth
    implicitHeight: control.buttonHeight
    Layout.preferredWidth: control.buttonWidth
    Layout.preferredHeight: control.buttonHeight
    Layout.alignment: Qt.AlignVCenter

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    function baseColor() {
        if (control.kind === "accent") {
            return control.accentColor.a > 0 ? control.accentColor : appTheme.accentColor
        }
        return control.normalColor.a > 0 ? control.normalColor : appTheme.bgBaseColor
    }

    ToolTip.text: control.tooltipText
    ToolTip.visible: hover.containsMouse && control.tooltipText.length > 0
    ToolTip.delay: 400

    Rectangle {
        id: bg
        anchors.centerIn: parent
        width: control.buttonWidth
        height: control.buttonHeight
        radius: control.buttonRadius
        color: {
            const base = control.baseColor()
            if (!control.enabled) {
                return control.withAlpha(base, 0.38)
            }
            if (hover.pressed) {
                return Qt.darker(base, 1.14)
            }
            if (hover.containsMouse) {
                return Qt.lighter(base, control.kind === "normal" ? 1.16 : 1.08)
            }
            return base
        }
        border.width: control.bordered ? 1 : 0
        border.color: control.borderColor
    }

    // Use the SVG only as an alpha mask. This makes currentColor-based SVGs
    // pick up appTheme.iconColor reliably instead of depending on SVG color
    // inheritance inside Qt's Image renderer.
    Image {
        id: iconMask
        anchors.centerIn: parent
        visible: false
        source: control.iconSrc
        sourceSize.width: control.iconSize
        sourceSize.height: control.iconSize
        width: control.iconSize
        height: control.iconSize
        fillMode: Image.Pad
        layer.enabled: true
    }

    Rectangle {
        anchors.centerIn: parent
        width: control.iconSize
        height: control.iconSize
        color: control.enabled ? control.iconColor : appTheme.textMutedColor
        visible: control.iconSrc.length > 0
        layer.enabled: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: iconMask
        }
    }

    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: {
            if (control.enabled) {
                control.clicked()
            }
        }
    }
}
