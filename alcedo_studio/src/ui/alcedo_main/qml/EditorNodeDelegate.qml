import QtQuick
import QtQuick.Controls

import QuickQanava 2.0 as Qan

// Color Grade node card: display name and a default-open Mask drawer.
// Does not show topology numbers, status, On/Off, adjustments, Mask counts,
// or persistent action rows. Rename and Delete stay on the shared menu.
Qan.NodeItem {
    id: root
    objectName: "qan::NodeItem"

    property string nodeKind: "colorGrade"
    property var masks: []
    property bool drawerOpen: true

    readonly property string displayName: node ? node.label : ""

    resizable: false
    minimumSize: Qt.size(appTheme.graphNodeWidth, appTheme.graphNameRowHeight
                         + appTheme.graphMaskDrawerHeaderHeight)
    width: appTheme.graphNodeWidth
    height: nameRow.height + divider.height + maskDrawer.height

    Accessible.role: Accessible.Grouping
    Accessible.name: root.displayName

    onWidthChanged: setDefaultBoundingShape()
    onHeightChanged: setDefaultBoundingShape()
    Component.onCompleted: setDefaultBoundingShape()

    Rectangle {
        id: card
        objectName: "editorNodeCard"
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        color: appTheme.cardSurfaceColor
        border.width: appTheme.graphSelectionOutlineWidth
        border.color: root.selected ? appTheme.graphSelectionOutlineColor
                                    : appTheme.cardBorderColor

        Column {
            id: column
            objectName: "editorNodeColumn"
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0

            Item {
                id: nameRow
                objectName: "editorNodeNameRow"
                width: parent.width
                height: appTheme.graphNameRowHeight

                Label {
                    id: nameLabel
                    objectName: "editorNodeName"
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceSm
                    anchors.rightMargin: appTheme.spaceSm
                    text: root.displayName
                    color: appTheme.textColor
                    font.pixelSize: appTheme.fontSizeTitle
                    font.weight: appTheme.fontWeightStrong
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    Accessible.ignored: true
                }
            }

            Rectangle {
                id: divider
                objectName: "editorNodeDrawerDivider"
                width: parent.width
                height: 1
                color: appTheme.cardBorderColor
            }

            EditorNodeMaskDrawer {
                id: maskDrawer
                width: parent.width
                masks: root.masks
                expanded: root.drawerOpen
                surfaceColor: appTheme.cardSurfaceColor
                onToggled: function (open) {
                    root.drawerOpen = open
                }
            }
        }
    }

    onDrawerOpenChanged: {
        if (maskDrawer.expanded !== root.drawerOpen) {
            maskDrawer.expanded = root.drawerOpen
        }
    }
}
