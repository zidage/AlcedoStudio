import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Warning and error message (DESIGN.md "Alert badges"): white text on a
// `alertBadgeFillColor` badge. Every warning or error sentence uses this
// component instead of plain red text, which is hard to read on dark surfaces.
// The badge wraps its text and hides itself while the text is empty.
Rectangle {
    id: root

    property string text: ""

    visible: text.length > 0
    implicitWidth: label.implicitWidth + label.anchors.leftMargin + label.anchors.rightMargin
    implicitHeight: label.implicitHeight + label.anchors.topMargin + label.anchors.bottomMargin
    Layout.fillWidth: true
    radius: appTheme.badgeRadius
    color: appTheme.alertBadgeFillColor

    Accessible.role: Accessible.AlertMessage
    Accessible.name: text

    Label {
        id: label
        anchors.fill: parent
        anchors.leftMargin: appTheme.spaceSm
        anchors.rightMargin: appTheme.spaceSm
        anchors.topMargin: appTheme.spaceXs
        anchors.bottomMargin: appTheme.spaceXs
        text: root.text
        color: appTheme.alertBadgeTextColor
        wrapMode: Text.Wrap
        verticalAlignment: Text.AlignVCenter
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        font.weight: appTheme.fontWeightStrong
    }
}
