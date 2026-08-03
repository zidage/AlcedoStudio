import QtQuick
import QtQuick.Layouts

// Frameless window caption buttons (minimize / maximize-restore / close).
// CaptionButton stays a nested inline component — file-level inline
// components are rejected by qmlcachegen (see InspectorPanel.qml).
Row {
    id: root
    objectName: "windowCaptionButtons"
    property var theme: null
    property var host: null
    Layout.preferredHeight: 56
    spacing: 0

    component CaptionButton: Rectangle {
        id: capBtn
        property string iconName: "minimize"
        property color hoverColor: root.theme ? root.theme.colHover : Qt.rgba(1, 1, 1, 0.07)
        property color iconColor: root.theme ? root.theme.colText : "#F5F1EA"
        signal activated()
        width: 46
        height: 56
        color: capMA.containsMouse
               ? hoverColor
               : "transparent"

        Canvas {
            id: captionIcon
            anchors.centerIn: parent
            width: 16
            height: 16
            antialiasing: true

            Connections {
                target: capBtn
                function onIconNameChanged() { captionIcon.requestPaint() }
                function onIconColorChanged() { captionIcon.requestPaint() }
            }

            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.strokeStyle = capBtn.iconColor
                ctx.lineWidth = 1.35
                ctx.lineCap = "square"
                ctx.lineJoin = "miter"

                if (capBtn.iconName === "minimize") {
                    ctx.beginPath()
                    ctx.moveTo(4, 8)
                    ctx.lineTo(12, 8)
                    ctx.stroke()
                } else if (capBtn.iconName === "maximize") {
                    ctx.strokeRect(4.5, 4.5, 7, 7)
                } else if (capBtn.iconName === "restore") {
                    ctx.strokeRect(6.5, 4.5, 6, 6)
                    ctx.strokeRect(3.5, 7.5, 6, 6)
                } else if (capBtn.iconName === "close") {
                    ctx.lineCap = "round"
                    ctx.beginPath()
                    ctx.moveTo(4.5, 4.5)
                    ctx.lineTo(11.5, 11.5)
                    ctx.moveTo(11.5, 4.5)
                    ctx.lineTo(4.5, 11.5)
                    ctx.stroke()
                }
            }
        }

        MouseArea {
            id: capMA
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.ArrowCursor
            onClicked: capBtn.activated()
        }
    }

    CaptionButton {
        iconName: "minimize"
        onActivated: if (root.host) root.host.minimizeWindow()
    }
    CaptionButton {
        iconName: root.host && root.host.windowMaximized ? "restore" : "maximize"
        onActivated: if (root.host) root.host.toggleMaximizeAnimated()
    }
    CaptionButton {
        iconName: "close"
        hoverColor: root.theme ? root.theme.colDanger : "#C04A4A"
        onActivated: if (root.host) root.host.close()
    }
}