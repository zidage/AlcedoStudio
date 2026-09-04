import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Color Grade Mask drawer. Open/closed state is local UI layout state: it does
// not write PipelineDocument, create history, or start a photo render.
// Fold motion matches CollapsibleSection (DESIGN.md): logical expanded flips
// immediately; foldProgress drives height and opacity; the body clips; reduceMotion
// sets duration to zero. Tests can driveFoldProgress without wall-clock waits.
Item {
    id: root
    objectName: "editorNodeMaskDrawer"

    property var masks: []
    property bool expanded: true
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property color hoverColor: appTheme.hoverColor
    property color surfaceColor: appTheme.cardSurfaceColor

    property real foldProgress: expanded ? 1 : 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs

    readonly property real headerHeight: appTheme.graphMaskDrawerHeaderHeight
    readonly property real bodyContentHeight: {
        if (root.masks === undefined || root.masks === null) {
            return 0
        }
        const count = root.masks.length !== undefined ? root.masks.length : 0
        return count * appTheme.graphMaskRowHeight
    }
    readonly property real bodyHeight: Math.max(0, bodyContentHeight) * foldProgress

    implicitWidth: appTheme.graphNodeWidth
    implicitHeight: headerHeight + bodyHeight
    height: implicitHeight
    clip: true

    signal toggled(bool expanded)

    function toggle() {
        root.expanded = !root.expanded
        toggled(root.expanded)
    }

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
            easing.type: appTheme.motionEasing
        }
    }

    // Sunken well behind header and body so the drawer reads as an inset
    // section of the node card instead of a floating overlay. Inset margins
    // keep the card border visible; bottom corners follow the card radius.
    Rectangle {
        id: well
        objectName: "editorNodeMaskDrawerWell"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: appTheme.graphSelectionOutlineWidth
        anchors.rightMargin: appTheme.graphSelectionOutlineWidth
        anchors.bottomMargin: appTheme.graphSelectionOutlineWidth
        color: root.surfaceColor
        bottomLeftRadius: appTheme.controlRadiusSmall - appTheme.graphSelectionOutlineWidth
        bottomRightRadius: appTheme.controlRadiusSmall - appTheme.graphSelectionOutlineWidth
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        Item {
            id: header
            objectName: "editorNodeMaskDrawerHeader"
            width: parent.width
            height: root.headerHeight
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: root.expanded ? qsTr("Collapse Masks") : qsTr("Expand Masks")
            Accessible.onPressAction: root.toggle()

            Keys.onPressed: function (event) {
                if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                        || event.key === Qt.Key_Enter) {
                    root.toggle()
                    event.accepted = true
                }
            }

            // Hover/focus feedback stays inside the well: the bottom inset keeps
            // the card border visible, and whenever the header spans the whole
            // drawer (closed, or open with no Mask rows) its bottom corners
            // follow the well radius instead of painting square corners over
            // the card.
            Rectangle {
                id: headerWash
                objectName: "editorNodeMaskDrawerHeaderWash"
                anchors.fill: parent
                anchors.leftMargin: appTheme.graphSelectionOutlineWidth
                anchors.rightMargin: appTheme.graphSelectionOutlineWidth
                anchors.bottomMargin: appTheme.graphSelectionOutlineWidth
                color: headerMouse.containsMouse || header.activeFocus
                       ? root.hoverColor
                       : "transparent"
                bottomLeftRadius: header.height >= root.height - 0.5
                                  ? appTheme.controlRadiusSmall - appTheme.graphSelectionOutlineWidth
                                  : 0
                bottomRightRadius: bottomLeftRadius
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                spacing: appTheme.spaceSm

                Label {
                    objectName: "editorNodeMaskDrawerTitle"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("Masks")
                    color: root.textColor
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
                    elide: Text.ElideRight
                }

                Canvas {
                    id: chevron
                    objectName: "editorNodeMaskDrawerChevron"
                    Layout.preferredWidth: appTheme.spaceMd
                    Layout.preferredHeight: appTheme.spaceMd
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
                        function onExpandedChanged() {
                            chevron.requestPaint()
                        }
                    }
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

        Item {
            id: body
            objectName: "editorNodeMaskDrawerBody"
            width: parent.width
            height: root.bodyHeight
            visible: root.foldProgress > 0.001
            opacity: root.foldProgress
            clip: true

            Column {
                id: maskList
                objectName: "editorNodeMaskDrawerList"
                width: parent.width
                spacing: 0

                Repeater {
                    id: maskRepeater
                    objectName: "editorNodeMaskDrawerRepeater"
                    model: root.masks

                    EditorNodeMaskTypeRow {
                        width: maskList.width
                        sourceKind: modelData.sourceKind !== undefined ? String(modelData.sourceKind) : ""
                        maskId: modelData.maskId !== undefined ? String(modelData.maskId) : ""
                    }
                }
            }
        }
    }
}
