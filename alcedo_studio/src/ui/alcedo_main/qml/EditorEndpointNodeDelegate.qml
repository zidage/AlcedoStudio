import QtQuick
import QtQuick.Controls

import QuickQanava 2.0 as Qan

// Compact Develop / DRT/Post node. Fixed name and real ports only.
// No Mask drawer, Locked badge, status dot, or persistent action row.
Qan.NodeItem {
    id: root
    objectName: "qan::NodeItem"

    property string nodeKind: "develop"
    property var masks: []

    readonly property string displayName: node ? node.label : ""
    readonly property bool isDevelop: root.nodeKind === "develop"

    resizable: false
    minimumSize: Qt.size(appTheme.graphNodeWidth, appTheme.graphEndpointHeight)
    width: appTheme.graphNodeWidth
    height: Math.max(appTheme.graphEndpointHeight, nameLabel.implicitHeight + appTheme.spaceXs)
    activeFocusOnTab: false

    Accessible.role: Accessible.Grouping
    Accessible.name: root.displayName
    Accessible.description: root.isDevelop
                            ? qsTr("Develop cannot be renamed or deleted")
                            : qsTr("DRT/Post cannot be renamed or deleted")

    onWidthChanged: setDefaultBoundingShape()
    onHeightChanged: setDefaultBoundingShape()
    Component.onCompleted: setDefaultBoundingShape()

    Rectangle {
        id: card
        objectName: "editorEndpointNodeCard"
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        color: appTheme.cardSurfaceColor
        border.width: appTheme.graphSelectionOutlineWidth
        border.color: root.selected ? appTheme.graphSelectionOutlineColor
                                    : appTheme.graphNodeBorderColor

        Label {
            id: nameLabel
            objectName: "editorEndpointNodeName"
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
            horizontalAlignment: Text.AlignHCenter
            Accessible.ignored: true
        }
    }
}
