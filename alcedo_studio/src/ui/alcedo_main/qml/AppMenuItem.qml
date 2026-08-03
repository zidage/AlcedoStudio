import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared dark menu row for AppContextMenu (Library, filmstrip, toolbar menus).
// Visual identity: DESIGN.md "Context menus" — bgBase surface, hoverColor row
// wash, controlRadiusSmall highlight, textColor/textMutedColor ink.
MenuItem {
    id: control

    leftPadding: appTheme.spaceSm
    rightPadding: appTheme.spaceSm
    topPadding: appTheme.spaceXs
    bottomPadding: appTheme.spaceXs
    implicitWidth: Math.max(implicitContentWidth + leftPadding + rightPadding, 160)
    implicitHeight: implicitContentHeight + topPadding + bottomPadding

    contentItem: RowLayout {
        spacing: appTheme.spaceSm

        // Reserved state gutter (check marks) so rows align on one text edge.
        Label {
            Layout.preferredWidth: appTheme.spaceLg
            text: control.checked ? "✓" : ""
            color: control.enabled ? appTheme.textColor : appTheme.textMutedColor
            font.pixelSize: appTheme.fontSizeBody
            font.weight: appTheme.fontWeightStrong
        }

        Label {
            Layout.fillWidth: true
            text: control.text
            color: control.enabled ? appTheme.textColor : appTheme.textMutedColor
            font.pixelSize: appTheme.fontSizeBody
            font.weight: appTheme.fontWeightRegular
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        Label {
            visible: control.subMenu !== null
            text: "›"
            color: appTheme.textMutedColor
            font.pixelSize: appTheme.fontSizeTitle
            verticalAlignment: Text.AlignVCenter
        }
    }

    background: Rectangle {
        radius: appTheme.controlRadiusSmall
        color: control.highlighted && control.enabled ? appTheme.hoverColor : "transparent"
    }
}
