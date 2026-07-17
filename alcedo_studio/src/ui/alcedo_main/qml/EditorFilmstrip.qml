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
    // dockExpandProgress drives the downward fold (0 collapsed -> 1 expanded).
    // collapsed flips immediately (persisted session state); only the visual
    // height animates so the handle stays stationary and state assertions hold.
    // _motionArmed suppresses the initial snap; reduceMotion snaps the fold.
    property real dockExpandProgress: 0
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs
    readonly property real dockHeight: handleHeight
                                       + (expandedHeight - handleHeight) * dockExpandProgress
    readonly property color colPanel: theme ? theme.colGlassPanel : "#1C1C1D"
    readonly property color colStroke: theme ? theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : "#457B9D"
    readonly property color colHover: theme ? theme.colHover : Qt.rgba(1, 1, 1, 0.07)
    readonly property color colCardSurface: theme ? theme.colCardSurface : "#161719"
    readonly property color colCardBorder: theme ? theme.colCardBorder : Qt.rgba(1, 1, 1, 0.08)
    readonly property int panelRadius: theme ? theme.panelRadius : 12

    onCollapsedChanged: {
        _foldDuration = collapsed ? appTheme.motionFoldCloseMs : appTheme.motionFoldOpenMs
        dockExpandProgress = collapsed ? 0 : 1
    }
    Component.onCompleted: {
        // Snap to the persisted collapse state on load (no open animation).
        dockExpandProgress = collapsed ? 0 : 1
        _motionArmed = true
    }
    Behavior on dockExpandProgress {
        enabled: root._motionArmed
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : root._foldDuration
            easing.type: Easing.OutCubic
        }
    }

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
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder
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
                anchors.leftMargin: appTheme.spaceMd
                anchors.rightMargin: appTheme.spaceMd
                spacing: appTheme.spaceMd

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
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
                    elide: Text.ElideRight
                }

                Label {
                    visible: root.saveInProgress
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("Saving…")
                    color: root.colAccent
                    font.pixelSize: appTheme.fontSizeCaption
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

        // Expanded body — model-backed thumbnails land in Phase 6C. Visibility
        // and opacity track dockExpandProgress so the body folds with the dock
        // while the handle stays stationary.
        Item {
            id: filmstripBody
            objectName: "editorFilmstripBody"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: collapseHandle.bottom
            anchors.bottom: parent.bottom
            visible: root.dockExpandProgress > 0.001
            opacity: root.dockExpandProgress
            clip: true

            Label {
                anchors.centerIn: parent
                visible: root.totalCount <= 0
                text: qsTr("No images")
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeBody
            }

            // Keep a stable visual strip region so collapse geometry can release
            // height to the viewport without destroying dock identity.
            Rectangle {
                anchors.fill: parent
                anchors.margins: appTheme.spaceSm
                anchors.topMargin: appTheme.spaceXs
                visible: root.totalCount > 0
                radius: appTheme.controlRadiusSmall
                color: "transparent"
                border.width: 1
                border.color: root.colCardBorder

                Label {
                    anchors.centerIn: parent
                    text: root.hasImage
                          ? qsTr("Focused image %1").arg(root.currentIndex)
                          : qsTr("Select an image")
                    color: root.colMuted
                    font.pixelSize: appTheme.fontSizeBody
                }
            }
        }
    }
}
