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
    property string nodeId: ""
    property var masks: []
    property string selectedMaskId: ""
    property bool drawerOpen: true
    property var graphAdapter: null
    readonly property var resolvedAdapter: {
        if (root.graphAdapter)
            return root.graphAdapter
        return (root.graph && root.graph.alcedoQanGraph) ? root.graph.alcedoQanGraph : null
    }

    signal maskSelected(string nodeId, string maskId)
    signal maskDeleteRequested(string nodeId, string maskId)

    readonly property string displayName: node ? node.label : ""

    resizable: false
    minimumSize: Qt.size(appTheme.graphNodeWidth, appTheme.graphNameRowHeight
                         + appTheme.graphMaskDrawerHeaderHeight)
    width: appTheme.graphNodeWidth
    height: nameRow.height + divider.height + maskDrawer.height
    activeFocusOnTab: false

    Accessible.role: Accessible.Grouping
    Accessible.name: root.displayName
    Accessible.description: root.drawerOpen ? qsTr("Masks expanded") : qsTr("Masks collapsed")

    onWidthChanged: setDefaultBoundingShape()
    onHeightChanged: setDefaultBoundingShape()
    Component.onCompleted: setDefaultBoundingShape()

    Rectangle {
        id: card
        objectName: "editorNodeCard"
        // QuickQanava parents an invisible selection item at z=1 that covers the
        // whole node. Keep the card above it so Mask rows receive pointer input.
        z: 2
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        color: appTheme.cardSurfaceColor
        border.width: appTheme.graphSelectionOutlineWidth
        border.color: root.selected ? appTheme.graphSelectionOutlineColor
                                    : appTheme.graphNodeBorderColor

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
                height: Math.max(appTheme.graphNameRowHeight,
                                 nameLabel.implicitHeight + appTheme.spaceXs)

                Label {
                    id: nameLabel
                    objectName: "editorNodeName"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: appTheme.spaceSm
                    anchors.rightMargin: appTheme.spaceSm
                    text: root.displayName
                    color: appTheme.textColor
                    font.pixelSize: appTheme.fontSizeTitle
                    font.weight: appTheme.fontWeightStrong
                    elide: Text.ElideRight
                    wrapMode: Text.NoWrap
                    verticalAlignment: Text.AlignVCenter
                    Accessible.ignored: true
                }
            }

            Rectangle {
                id: divider
                objectName: "editorNodeDrawerDivider"
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width - 2 * appTheme.graphSelectionOutlineWidth
                height: appTheme.graphNameRowDividerHeight
                color: appTheme.cardBorderColor
            }

            EditorNodeMaskDrawer {
                id: maskDrawer
                width: parent.width
                nodeId: root.nodeId
                masks: root.masks
                selectedMaskId: root.selectedMaskId
                expanded: root.drawerOpen
                surfaceColor: appTheme.graphMaskDrawerSurfaceColor
                onToggled: function (open) {
                    root.drawerOpen = open
                }
                onMaskSelected: function (nodeId, maskId) {
                    if (root.resolvedAdapter)
                        root.resolvedAdapter.notifyMaskRowSelected(root, maskId)
                }
                onMaskDeleteRequested: function (nodeId, maskId) {
                    if (root.resolvedAdapter)
                        root.resolvedAdapter.notifyMaskRowDeleteRequested(root, maskId)
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
