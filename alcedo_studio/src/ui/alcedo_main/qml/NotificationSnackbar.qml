import QtQuick
import QtQuick.Controls

// Transient snackbar popup with auto-dismiss timer. The host forwards
// messages via show(); this component owns the popup, timer, and text.
Popup {
    id: root
    property var theme: null
    property var host: null
    property string message: ""
    parent: Overlay.overlay
    modal: false
    focus: false
    closePolicy: Popup.NoAutoClose
    padding: 12
    width: host ? Math.min(host.width - 24, 760) : 760
    x: host ? Math.round((host.width - width) / 2) : 0
    y: host ? host.height - height - 16 : 0

    background: Rectangle {
        radius: 10
        color: theme ? theme.colGlassPanel : "#1C1C1D"
        border.width: 1
        border.color: theme ? theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
    }

    contentItem: Label {
        text: root.message
        color: theme ? theme.colText : "#F5F1EA"
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 120 }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 120 }
    }

    readonly property int defaultDurationMs: 2600

    Timer {
        id: snackbarTimer
        interval: root.defaultDurationMs
        repeat: false
        onTriggered: root.close()
    }

    // durationMs: how long the message stays; the default when it is not a positive number.
    function show(messageText, durationMs) {
        if (!messageText || String(messageText).trim().length === 0) {
            return
        }
        root.message = String(messageText)
        snackbarTimer.interval = durationMs > 0 ? durationMs : root.defaultDurationMs
        snackbarTimer.restart()
        if (!root.opened) {
            root.open()
        }
    }
}