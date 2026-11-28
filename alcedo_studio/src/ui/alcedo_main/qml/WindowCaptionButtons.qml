import QtQuick

// Drawn caption buttons (minimize / maximize-restore / close) in the window's
// top-right corner, for platforms that do not keep native window controls.
// WindowCaptionLayer.qml hosts them above every popup (issue 1140); macOS uses
// the system traffic lights instead (see Main.qml nativeTrafficLightsEnabled).
//
// Each button paints a rounded tile as tall as the toolbar, with the icon on
// the toolbar's centre line. Its press area is taller: it reaches the window's
// top edge, and for close also the right edge, so the pointer can be thrown
// into the screen corner of a maximized window.
// CaptionButton stays a nested inline component — file-level inline
// components are rejected by qmlcachegen (see InspectorPanel.qml).
Row {
    id: root
    objectName: "windowCaptionButtons"
    property var theme: null
    property var host: null
    // Band between the window's top edge and the toolbar, and the toolbar height.
    property real topBand: appTheme.spaceMd
    property real toolbarHeight: appTheme.iconButtonHitSizeCompact + appTheme.spaceSm
    // Edge strips left to the window resize handles: no press is taken there.
    property real inputTopInset: 0
    property real inputRightInset: 0
    // Gap between the close tile and the window's right edge, the same as the
    // window margin around the toolbar.
    property real edgeGap: appTheme.spaceMd
    readonly property int buttonWidth: 42
    readonly property int iconSize: 10
    height: topBand + toolbarHeight
    spacing: 0

    component CaptionButton: Item {
        id: capBtn
        property string iconName: "minimize"
        property bool danger: false
        // Press area to the right of the tile (close reaches the window edge).
        property real trailingInput: 0
        readonly property bool hovered: capMA.containsMouse
        readonly property bool pressed: capMA.pressed
        readonly property color hoverColor: root.theme ? root.theme.colHover : "#1A1A1A"
        readonly property color dangerColor: root.theme ? root.theme.colDanger : "#8A3A3A"
        readonly property color iconColor: capBtn.danger && (capBtn.hovered || capBtn.pressed)
                                           ? "#FFFFFF"
                                           : (root.theme ? root.theme.colText : "#E0E0E0")
        signal activated()
        width: root.buttonWidth + capBtn.trailingInput
        height: root.height

        Rectangle {
            id: tile
            y: root.topBand
            width: root.buttonWidth
            height: root.toolbarHeight
            radius: root.theme ? root.theme.panelRadius : 10
            color: capBtn.pressed
                   ? (capBtn.danger ? Qt.darker(capBtn.dangerColor, 1.15)
                                    : Qt.lighter(capBtn.hoverColor, 1.3))
                   : capBtn.hovered
                     ? (capBtn.danger ? capBtn.dangerColor : capBtn.hoverColor)
                     : "transparent"
        }

        Canvas {
            id: captionIcon
            x: Math.round((tile.width - width) / 2)
            y: Math.round(tile.y + (tile.height - height) / 2)
            width: root.iconSize
            height: root.iconSize
            antialiasing: true

            Connections {
                target: capBtn
                function onIconNameChanged() { captionIcon.requestPaint() }
                function onIconColorChanged() { captionIcon.requestPaint() }
            }

            // 1 px lines on half-pixel coordinates stay on the pixel grid.
            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.strokeStyle = capBtn.iconColor
                ctx.lineWidth = 1
                ctx.lineCap = "butt"
                ctx.lineJoin = "miter"

                if (capBtn.iconName === "minimize") {
                    ctx.beginPath()
                    ctx.moveTo(0, 5.5)
                    ctx.lineTo(10, 5.5)
                    ctx.stroke()
                } else if (capBtn.iconName === "maximize") {
                    ctx.strokeRect(0.5, 0.5, 9, 9)
                } else if (capBtn.iconName === "restore") {
                    ctx.strokeRect(0.5, 2.5, 7, 7)
                    ctx.beginPath()
                    ctx.moveTo(2.5, 2.5)
                    ctx.lineTo(2.5, 0.5)
                    ctx.lineTo(9.5, 0.5)
                    ctx.lineTo(9.5, 7.5)
                    ctx.lineTo(7.5, 7.5)
                    ctx.stroke()
                } else if (capBtn.iconName === "close") {
                    ctx.beginPath()
                    ctx.moveTo(0.5, 0.5)
                    ctx.lineTo(9.5, 9.5)
                    ctx.moveTo(9.5, 0.5)
                    ctx.lineTo(0.5, 9.5)
                    ctx.stroke()
                }
            }
        }

        MouseArea {
            id: capMA
            anchors.fill: parent
            anchors.topMargin: root.inputTopInset
            anchors.rightMargin: capBtn.trailingInput > 0 ? root.inputRightInset : 0
            hoverEnabled: true
            cursorShape: Qt.ArrowCursor
            onClicked: capBtn.activated()
        }
    }

    CaptionButton {
        objectName: "windowCaptionMinimize"
        iconName: "minimize"
        onActivated: if (root.host) root.host.minimizeWindow()
    }
    CaptionButton {
        objectName: "windowCaptionMaximize"
        iconName: root.host && root.host.windowMaximized ? "restore" : "maximize"
        onActivated: if (root.host) root.host.toggleMaximizeAnimated()
    }
    CaptionButton {
        objectName: "windowCaptionClose"
        iconName: "close"
        danger: true
        trailingInput: root.edgeGap
        onActivated: if (root.host) root.host.close()
    }
}
