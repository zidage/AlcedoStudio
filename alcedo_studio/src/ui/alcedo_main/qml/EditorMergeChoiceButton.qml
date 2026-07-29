import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Button {
    id: root

    property string actionObjectName: ""
    property string choiceKind: "current"
    property bool selected: false
    property string label: ""
    property string accessibleLabel: label
    property color mutedColor: appTheme.textMutedColor

    readonly property color choiceColor: choiceKind === "incoming"
                                        ? appTheme.mergeIncomingColor
                                        : appTheme.mergeCurrentColor
    readonly property color choiceFillColor: choiceKind === "incoming"
                                             ? appTheme.mergeIncomingFillColor
                                             : appTheme.mergeCurrentFillColor

    signal choiceSelected(string choice)

    objectName: root.actionObjectName
    Layout.fillWidth: true
    Layout.preferredHeight: appTheme.spaceXl * 2
    text: root.label
    hoverEnabled: true
    activeFocusOnTab: true
    Accessible.name: root.accessibleLabel
    onClicked: root.choiceSelected(root.choiceKind)

    contentItem: Label {
        text: root.text
        color: root.selected ? root.choiceColor : root.mutedColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        font.weight: appTheme.fontWeightStrong
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: appTheme.controlRadiusSmall
        color: root.selected
               ? root.choiceFillColor
               : (root.hovered ? appTheme.buttonHoveredFillColor : appTheme.cardSurfaceColor)
        border.width: 1
        border.color: root.selected ? root.choiceColor : appTheme.cardBorderColor
    }
}
