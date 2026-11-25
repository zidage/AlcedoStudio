import QtQuick

// The blurred backdrop around a full-window dialog's card (Settings, Welcome):
// a drag there moves the window and a double-click toggles maximize, as on the
// toolbar (issue 1140). Presses on the card pass by: the containment mask holds
// only the backdrop. Inactive where the system draws the caption (macOS).
Item {
    id: root
    property var host: null
    property Item card: null
    anchors.fill: parent
    enabled: root.host !== null && root.host.nativeTrafficLightsEnabled !== true

    containmentMask: QtObject {
        function contains(point: point): bool {
            const card = root.card
            if (!card)
                return true
            return point.x < card.x || point.y < card.y
                    || point.x >= card.x + card.width || point.y >= card.y + card.height
        }
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.DragThreshold
        onDoubleTapped: if (root.host) root.host.toggleMaximizeAnimated()
    }
    DragHandler {
        target: null
        onActiveChanged: if (active && root.host) root.host.startSystemMove()
    }
}
