import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Bottom filmstrip dock for EditorWorkspace.
// Phase 1B defines collapse geometry, persistent handle accessibility, and
// empty-state presentation. Thumbnail model/selection arrives in Phase 6C.
Item {
    id: root
    objectName: "editorFilmstrip"

    property var theme: null
    property var editorSession: null
    property bool collapsed: editorSession ? editorSession.filmstripCollapsed : false
    property real expandedHeight: editorSession ? editorSession.filmstripExpandedHeight : 128
    property int currentIndex: 0
    property int totalCount: 0
    property bool saveInProgress: false
    property bool hasImage: editorSession ? editorSession.hasImage : false

    readonly property real handleHeight: 28
    readonly property real dockHeight: collapsed ? handleHeight : expandedHeight
    readonly property color colPanel: theme ? theme.colGlassPanel : "#1C1C1D"
    readonly property color colStroke: theme ? theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : "#457B9D"
    readonly property color colHover: theme ? theme.colHover : Qt.rgba(1, 1, 1, 0.07)
    readonly property int panelRadius: theme ? theme.panelRadius : 12

    signal expandRequested()
    signal collapseRequested()
    signal toggleRequested()
    signal imageActivated(int index)

    // Layout.preferredHeight binds to dockHeight so collapse releases vertical space
    // to the viewport without destroying the filmstrip identity or model later.
    implicitHeight: dockHeight
    focus: false
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Editor filmstrip")
    Accessible.description: collapsed
                            ? qsTr("Collapsed filmstrip handle")
                            : qsTr("Expanded filmstrip dock")

    function toggleCollapsed() {
        if (!editorSession) {
            return
        }
        editorSession.filmstripCollapsed = !editorSession.filmstripCollapsed
        if (editorSession.filmstripCollapsed) {
            collapseRequested()
        } else {
            expandRequested()
        }
        toggleRequested()
    }

    function focusHandle() {
        collapseHandle.forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        radius: root.panelRadius
        color: root.colPanel
        border.width: 1
        border.color: root.colStroke
        clip: true

        // Persistent focusable handle — remains keyboard- and pointer-accessible
        // when the dock is collapsed so the released height returns to the viewport.
        Item {
            id: collapseHandle
            objectName: "editorFilmstripHandle"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: root.handleHeight
            focus: true
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: root.collapsed ? qsTr("Expand filmstrip") : qsTr("Collapse filmstrip")
            Accessible.description: qsTr("Image %1 of %2").arg(Math.max(0, root.currentIndex))
                                                         .arg(Math.max(0, root.totalCount))
            Accessible.onPressAction: root.toggleCollapsed()

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                        || event.key === Qt.Key_Enter) {
                    root.toggleCollapsed()
                    event.accepted = true
                } else if (event.key === Qt.Key_Up && root.collapsed) {
                    root.toggleCollapsed()
                    event.accepted = true
                } else if (event.key === Qt.Key_Down && !root.collapsed) {
                    root.toggleCollapsed()
                    event.accepted = true
                }
            }

            Rectangle {
                anchors.fill: parent
                color: handleMouse.containsMouse || collapseHandle.activeFocus
                       ? root.colHover
                       : "transparent"
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10

                // Drag/affordance chevron
                Canvas {
                    id: chevron
                    Layout.preferredWidth: 14
                    Layout.preferredHeight: 14
                    Layout.alignment: Qt.AlignVCenter
                    antialiasing: true
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        ctx.strokeStyle = root.colMuted
                        ctx.lineWidth = 1.5
                        ctx.lineCap = "round"
                        ctx.lineJoin = "round"
                        ctx.beginPath()
                        if (root.collapsed) {
                            ctx.moveTo(3, 9)
                            ctx.lineTo(7, 5)
                            ctx.lineTo(11, 9)
                        } else {
                            ctx.moveTo(3, 5)
                            ctx.lineTo(7, 9)
                            ctx.lineTo(11, 5)
                        }
                        ctx.stroke()
                    }
                    Connections {
                        target: root
                        function onCollapsedChanged() { chevron.requestPaint() }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    text: {
                        if (root.totalCount <= 0) {
                            return qsTr("No images in filmstrip")
                        }
                        return qsTr("%1 / %2").arg(Math.max(1, root.currentIndex)).arg(root.totalCount)
                    }
                    color: root.colText
                    font.pixelSize: 12
                    font.weight: 600
                    elide: Text.ElideRight
                }

                Label {
                    visible: root.saveInProgress
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("Saving…")
                    color: root.colAccent
                    font.pixelSize: 11
                }

                Label {
                    Layout.alignment: Qt.AlignVCenter
                    text: root.collapsed ? qsTr("Expand") : qsTr("Collapse")
                    color: root.colMuted
                    font.pixelSize: 11
                }
            }

            MouseArea {
                id: handleMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.toggleCollapsed()
            }
        }

        // Expanded body placeholder — model-backed thumbnails land in Phase 6C.
        Item {
            id: filmstripBody
            objectName: "editorFilmstripBody"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: collapseHandle.bottom
            anchors.bottom: parent.bottom
            visible: !root.collapsed
            opacity: root.collapsed ? 0 : 1
            clip: true

            Label {
                anchors.centerIn: parent
                visible: root.totalCount <= 0
                text: qsTr("Filmstrip will show album or search results here")
                color: root.colMuted
                font.pixelSize: 12
            }

            // Keep a stable visual strip region so collapse geometry can release
            // height to the viewport without destroying dock identity.
            Rectangle {
                anchors.fill: parent
                anchors.margins: 8
                anchors.topMargin: 4
                visible: root.totalCount > 0
                radius: 8
                color: "transparent"
                border.width: 1
                border.color: root.colStroke

                Label {
                    anchors.centerIn: parent
                    text: root.hasImage
                          ? qsTr("Focused image %1").arg(root.currentIndex)
                          : qsTr("Select an image")
                    color: root.colMuted
                    font.pixelSize: 12
                }
            }
        }
    }
}
