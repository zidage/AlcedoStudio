import QtQuick

import QuickQanava 2.0 as Qan

// Scene-image port. The visible control is a small square; the item is a larger
// hit target. A port is a connection control, not a status dot. Qan dock
// placement keeps the output port on the current node bottom.
Qan.PortItem {
    id: portItem
    objectName: "editorNodePort"

    resizable: false
    minimumSize: Qt.size(appTheme.graphPortHitSize, appTheme.graphPortHitSize)
    width: appTheme.graphPortHitSize
    height: appTheme.graphPortHitSize

    Accessible.role: Accessible.Button
    Accessible.name: portItem.type === Qan.PortItem.Out ? qsTr("Output") : qsTr("Input")

    Rectangle {
        id: visiblePort
        objectName: "editorNodePortSquare"
        anchors.centerIn: parent
        width: appTheme.graphPortSize
        height: appTheme.graphPortSize
        radius: 0
        color: appTheme.graphPortFillColor
        border.width: 1.5
        border.color: appTheme.graphPortBorderColor
    }
}
