import QtQuick

// Frameless-window resize handles (native DWM resize via startSystemResize).
// `active` is driven by the host so this file does not reference the Window
// enum directly.
Item {
    id: root
    property var host: null
    property bool active: false
    anchors.fill: parent
    visible: root.active
    z: 1000

    readonly property int edge: 4
    readonly property int corner: 10

    MouseArea {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: root.edge
        cursorShape: Qt.SizeVerCursor
        onPressed: if (root.host) root.host.startSystemResize(Qt.TopEdge)
    }
    MouseArea {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: root.edge
        cursorShape: Qt.SizeVerCursor
        onPressed: if (root.host) root.host.startSystemResize(Qt.BottomEdge)
    }
    MouseArea {
        anchors { top: parent.top; bottom: parent.bottom; left: parent.left }
        width: root.edge
        cursorShape: Qt.SizeHorCursor
        onPressed: if (root.host) root.host.startSystemResize(Qt.LeftEdge)
    }
    MouseArea {
        anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
        width: root.edge
        cursorShape: Qt.SizeHorCursor
        onPressed: if (root.host) root.host.startSystemResize(Qt.RightEdge)
    }
    MouseArea {
        anchors { top: parent.top; left: parent.left }
        width: root.corner; height: root.corner
        cursorShape: Qt.SizeFDiagCursor
        onPressed: if (root.host) root.host.startSystemResize(Qt.TopEdge | Qt.LeftEdge)
    }
    MouseArea {
        anchors { top: parent.top; right: parent.right }
        width: root.corner; height: root.corner
        cursorShape: Qt.SizeBDiagCursor
        onPressed: if (root.host) root.host.startSystemResize(Qt.TopEdge | Qt.RightEdge)
    }
    MouseArea {
        anchors { bottom: parent.bottom; left: parent.left }
        width: root.corner; height: root.corner
        cursorShape: Qt.SizeBDiagCursor
        onPressed: if (root.host) root.host.startSystemResize(Qt.BottomEdge | Qt.LeftEdge)
    }
    MouseArea {
        anchors { bottom: parent.bottom; right: parent.right }
        width: root.corner; height: root.corner
        cursorShape: Qt.SizeFDiagCursor
        onPressed: if (root.host) root.host.startSystemResize(Qt.BottomEdge | Qt.RightEdge)
    }
}