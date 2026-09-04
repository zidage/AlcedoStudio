import QtQuick
import QtQuick.Layouts

import QuickQanava 2.0 as Qan

// Horizontal port dock with zero outer margin so port squares sit directly
// against the node card edge instead of floating one row away from it.
//
// Edge items recompute their endpoints only when a port's own x/y/width/height
// change; a dock move (anchor state applying after creation, or the host node
// changing height when its Mask drawer folds) leaves the port's local position
// untouched, so connected edges would keep stale endpoints. Mirror the upstream
// VerticalDock fix (#145) and force a port edge refresh whenever the dock moves.
RowLayout {
    id: root

    property var hostNodeItem: undefined
    property int dockType: -1

    spacing: 0
    z: 1.5

    onXChanged: if (hostNodeItem) hostNodeItem.updatePortsEdges()
    onYChanged: if (hostNodeItem) hostNodeItem.updatePortsEdges()

    states: [
        State {
            name: "top"
            when: hostNodeItem && dockType === Qan.NodeItem.Top
            AnchorChanges {
                target: root
                anchors {
                    horizontalCenter: hostNodeItem.horizontalCenter
                    bottom: hostNodeItem.top
                }
            }
        },
        State {
            name: "bottom"
            when: hostNodeItem && dockType === Qan.NodeItem.Bottom
            AnchorChanges {
                target: root
                anchors {
                    horizontalCenter: hostNodeItem.horizontalCenter
                    top: hostNodeItem.bottom
                }
            }
        }
    ]
}
