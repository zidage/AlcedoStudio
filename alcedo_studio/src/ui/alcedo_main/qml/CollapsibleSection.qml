import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared collapsible section for editor adjustment groups and similar folds.
// Motion contract (DESIGN.md):
//   - Logical expanded flips immediately (session/state).
//   - foldProgress (0 collapsed → 1 expanded) drives height and opacity.
//   - Persistent header/trigger stays put; body clips intermediate content.
//   - Opening uses motionFoldOpenMs + OutCubic; closing uses motionFoldCloseMs.
//   - reduceMotion snaps; driveFoldProgress() lets tests set intermediate points
//     without wall-clock sleeps.
Item {
    id: root
    objectName: "collapsibleSection"

    property string title: ""
    property bool expanded: true
    property bool controlsEnabled: true
    // Phase 4D: opaque surface colors (no parent-shell opacity for disabled).
    property color surfaceColor: appTheme.cardSurfaceColor
    property color disabledSurfaceColor: appTheme.disabledSurfaceColor
    property color borderColor: appTheme.cardBorderColor
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property color hoverColor: appTheme.hoverColor
    property color accentColor: appTheme.accentColor
    // Optional default body height when the loader content has no implicit size.
    property real bodyContentHeight: 120

    // ── Fold driver (test-visible) ──────────────────────────────────────────
    property real foldProgress: expanded ? 1 : 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs

    readonly property real headerHeight: appTheme.iconButtonHitSizeCompact
    readonly property real bodyHeight: Math.max(0, bodyContentHeight) * foldProgress
    readonly property real sectionHeight: headerHeight + bodyHeight

    implicitHeight: sectionHeight
    implicitWidth: 200
    Layout.fillWidth: true
    Layout.preferredHeight: sectionHeight
    clip: true

    signal toggled(bool expanded)

    function toggle() {
        root.expanded = !root.expanded
        toggled(root.expanded)
    }

    // Test / driver API: pin progress without Behavior, assert geometry at any t.
    function driveFoldProgress(value) {
        foldManualDrive = true
        foldProgress = Math.max(0, Math.min(1, value))
    }

    function endFoldDrive() {
        foldManualDrive = false
        foldProgress = expanded ? 1 : 0
    }

    onExpandedChanged: {
        _foldDuration = expanded ? appTheme.motionFoldOpenMs : appTheme.motionFoldCloseMs
        if (!foldManualDrive) {
            foldProgress = expanded ? 1 : 0
        }
    }

    Component.onCompleted: {
        foldProgress = expanded ? 1 : 0
        _motionArmed = true
    }

    Behavior on foldProgress {
        enabled: root._motionArmed && !root.foldManualDrive
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : root._foldDuration
            easing.type: Easing.OutCubic
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        // Phase 4D: opaque disabled surface (was opacity: 0.55 on the shell).
        color: root.controlsEnabled ? root.surfaceColor : root.disabledSurfaceColor
        border.width: 1
        border.color: root.borderColor

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Persistent header — never moves with the fold.
            Item {
                id: header
                objectName: "collapsibleSectionHeader"
                Layout.fillWidth: true
                Layout.preferredHeight: root.headerHeight
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: root.expanded
                                 ? qsTr("Collapse %1").arg(root.title)
                                 : qsTr("Expand %1").arg(root.title)
                Accessible.onPressAction: root.toggle()

                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                            || event.key === Qt.Key_Enter) {
                        root.toggle()
                        event.accepted = true
                    }
                }

                // Phase 4D: explicit opaque idle color (was "transparent").
                Rectangle {
                    anchors.fill: parent
                    color: headerMouse.containsMouse || header.activeFocus
                           ? root.hoverColor
                           : root.surfaceColor
                    radius: appTheme.controlRadiusSmall
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceSm
                    anchors.rightMargin: appTheme.spaceSm
                    spacing: appTheme.spaceSm

                    // Chevron affordance (canvas keeps asset set small).
                    Canvas {
                        id: chevron
                        Layout.preferredWidth: 12
                        Layout.preferredHeight: 12
                        Layout.alignment: Qt.AlignVCenter
                        antialiasing: true
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            ctx.strokeStyle = root.mutedColor
                            ctx.lineWidth = 1.5
                            ctx.lineCap = "round"
                            ctx.lineJoin = "round"
                            ctx.beginPath()
                            if (root.expanded) {
                                ctx.moveTo(2, 4)
                                ctx.lineTo(6, 8)
                                ctx.lineTo(10, 4)
                            } else {
                                ctx.moveTo(4, 2)
                                ctx.lineTo(8, 6)
                                ctx.lineTo(4, 10)
                            }
                            ctx.stroke()
                        }
                        Connections {
                            target: root
                            function onExpandedChanged() { chevron.requestPaint() }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        text: root.title
                        color: root.textColor
                        font.pixelSize: appTheme.fontSizeTitle
                        font.weight: appTheme.fontWeightStrong
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    id: headerMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.toggle()
                }
            }

            // Folding body — height and opacity track foldProgress together.
            Item {
                id: body
                objectName: "collapsibleSectionBody"
                Layout.fillWidth: true
                Layout.preferredHeight: root.bodyHeight
                visible: root.foldProgress > 0.001
                opacity: root.foldProgress
                clip: true

                // Callers inject content via default property alias.
                default property alias contentData: bodyContent.data
                Item {
                    id: bodyContent
                    anchors.fill: parent
                    anchors.margins: appTheme.spaceSm
                }
            }
        }
    }
}
