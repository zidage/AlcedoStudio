import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Button {
    id: control

    property string kind: "normal"
    property int buttonWidth: 168
    property int buttonHeight: 46
    property int buttonRadius: 10

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
        return appTheme.bgBaseColor
    }

    implicitWidth: buttonWidth
    implicitHeight: buttonHeight
    Layout.preferredWidth: buttonWidth
    Layout.preferredHeight: buttonHeight
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0
    padding: 0
    hoverEnabled: true
    font.family: appTheme.uiFontFamily
    font.pixelSize: appTheme.fontSizeSection
    font.weight: appTheme.fontWeightHeading

    contentItem: Label {
        text: control.text
        color: control.enabled ? "#FFFFFF" : appTheme.textMutedColor
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: control.buttonRadius
        color: {
            const base = control.baseColor()
            if (!control.enabled) {
                return control.withAlpha(base, 0.38)
            }
            if (control.down) {
                return Qt.darker(base, 1.14)
            }
            if (control.hovered) {
                return Qt.lighter(base, control.kind === "normal" ? 1.16 : 1.08)
            }
            return base
        }
        border.width: 0
    }
}
