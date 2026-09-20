import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// One Mask Group row inside EditorMaskGroupsPanel: chevron, composite preview
// slot, group name, and independent lock/delete actions. Expanded state comes
// from EditorNodeLayoutStore.drawerOpen(nodeId) — the same "Masks expanded"
// bit the Nodes drawer reads, so both projections agree.
//
// Reordering follows the Nodes card: the card itself owns unclaimed pointer
// input (the same role Qan.NodeItem plays for a Color Grade). Nested Mask
// rows and lock/delete buttons keep their own MouseAreas and consume those
// presses; name, chevron, preview, and empty drawer chrome bubble to the
// card. A sibling MouseArea behind the drawer never sees those events, and
// MouseArea.drag cannot move an anchored target — dragBody is sized by
// width/height only so the whole card (open drawer included) can travel on
// Y. The panel tracks the nearest insertion slot with a hairline, and the
// drop becomes one Nodes-page topology edit through
// EditorNodeController.moveMaskGroupToIndex. Ctrl+Up and Ctrl+Down on the
// focused header move the group one step for keyboard users.
//
// Selection follows docs/VI/README.md: a selected group keeps its surface and
// SVG colors and changes only the outer card outline. A group that owns the
// selected Mask receives quiet header emphasis; the Mask row owns the stronger
// outline.
//
// Fold motion matches EditorNodeMaskDrawer (DESIGN.md): logical expanded flips
// immediately; foldProgress drives height and opacity; the body clips;
// reduceMotion sets duration to zero. Tests can driveFoldProgress without
// wall-clock waits.
Item {
    id: root
    objectName: "editorMaskGroupDelegate"

    property string nodeId: ""
    property string displayName: ""
    property bool groupEnabled: true
    property bool deletionProtected: false
    property var masks: []
    property bool selected: false
    property bool ownerActive: false
    property bool expanded: true
    // Structure gate shared by every action on this row (add/remove group,
    // lock, delete, move, Mask commands): editable session + no command in
    // flight + no incomplete draft.
    property bool actionsEnabled: true
    property string actionsDisabledReason: ""
    // Drag reorder gate resolved by the panel: editable structure plus more
    // than one group on the committed backbone.
    property bool reorderEnabled: false
    property string selectedMaskId: ""
    property var maskThumbnails: null
    property string groupThumbUrl: ""
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property color hoverColor: appTheme.hoverColor
    property color cardSurfaceColor: appTheme.cardSurfaceColor
    property color cardBorderColor: appTheme.cardBorderColor
    property color selectionOutlineColor: appTheme.graphSelectionOutlineColor
    property real selectionOutlineWidth: appTheme.graphSelectionOutlineWidth

    property real foldProgress: expanded ? 1 : 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs

    readonly property int maskCount: {
        if (root.masks === undefined || root.masks === null) {
            return 0
        }
        return root.masks.length !== undefined ? root.masks.length : 0
    }
    readonly property real headerHeight: Math.max(
                                             appTheme.iconButtonHitSizeCompact,
                                             appTheme.maskGroupPreviewSize
                                             + appTheme.spaceXs * 2,
                                             groupName.implicitHeight
                                             + appTheme.spaceXs * 2)
    readonly property real bodyContentHeight: maskList.implicitHeight + appTheme.spaceXs
    readonly property real bodyHeight: Math.max(0, bodyContentHeight) * foldProgress
    readonly property color headerInkColor: root.textColor
    readonly property string deleteDisabledReason: {
        if (!root.actionsEnabled) {
            return root.actionsDisabledReason
        }
        if (root.deletionProtected) {
            return qsTr("Unlock %1 before deleting").arg(root.displayName)
        }
        return ""
    }
    readonly property bool dragging: cardMouse.drag.active
    readonly property bool headerHovered: cardMouse.containsMouse
                                          && cardMouse.mouseY < root.headerHeight
    // Card position in ListView content coordinates while dragging; the panel
    // resolves it to the nearest insertion slot between group rows.
    readonly property real dragContentY: root.y + dragBody.y + root.headerHeight / 2
    property bool _dragMoved: false

    implicitWidth: 260
    implicitHeight: headerHeight + bodyHeight + appTheme.spaceXs
    height: implicitHeight
    // Unclipped while dragging so the card can travel across sibling rows.
    clip: !root.dragging
    z: root.dragging ? 2 : 0

    signal headerClicked()
    signal expansionToggled(bool expanded)
    signal lockClicked()
    signal deleteClicked()
    // contentY args are in ListView content coordinates (see dragContentY).
    signal reorderDragStarted()
    signal reorderDragMoved(real contentY)
    signal reorderDropped(real contentY)
    // Press/release around a reorder so the ListView can drop its flick grab
    // before the card actually moves. Canceled means snap back with no drop.
    signal reorderPressChanged(bool pressed)
    signal reorderCanceled()
    signal reorderStepRequested(int delta)
    signal maskClicked(int maskIndex)
    signal maskDeleteClicked(int maskIndex)
    signal maskNavigateUp(int maskIndex)
    signal maskNavigateDown(int maskIndex)
    signal headerNavigate(int delta)

    function toggle() {
        root.expansionToggled(!root.expanded)
    }

    function activateHeader() {
        root.headerClicked()
        root.toggle()
    }

    // Shared drop handling for the card MouseArea: report the card position
    // in content coordinates, then snap the card back — the Behavior animates
    // the return when nothing moved.
    function finishCardDrag() {
        if (!root._dragMoved) {
            return
        }
        const contentY = root.dragContentY
        dragBody.y = 0
        root.reorderDropped(contentY)
    }

    function maskRowAt(maskIndex) {
        if (maskIndex < 0 || maskIndex >= maskRepeater.count) {
            return null
        }
        return maskRepeater.itemAt(maskIndex)
    }

    function refreshGroupThumb() {
        groupThumbUrl = (root.maskThumbnails && root.nodeId.length > 0)
                ? String(root.maskThumbnails.thumbnailUrl(root.nodeId, "") || "")
                : ""
    }

    onNodeIdChanged: refreshGroupThumb()
    onMaskThumbnailsChanged: refreshGroupThumb()

    Connections {
        target: root.maskThumbnails
        function onThumbnailUrlChanged(changedNodeId, changedMaskId) {
            if (changedNodeId === root.nodeId && changedMaskId === "") {
                root.refreshGroupThumb()
            }
        }
    }

    function focusHeader() {
        header.forceActiveFocus(Qt.TabFocusReason)
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

    // Drag target: the whole card (surface, header, and open Mask drawer)
    // travels on Y. The delegate slot itself does not move, so sibling
    // midpoints stay stable for the panel's drop-slot resolution. Do not
    // anchors.fill this item — MouseArea.drag cannot move an anchored target.
    Item {
        id: dragBody
        objectName: "editorMaskGroupDragBody"
        width: parent.width
        height: parent.height
        opacity: root.dragging ? 0.94 : 1.0

        onYChanged: {
            if (root.dragging) {
                if (!root._dragMoved) {
                    root._dragMoved = true
                    root.reorderDragStarted()
                }
                root.reorderDragMoved(root.dragContentY)
            }
        }

        // Snap back when a drop lands on the same slot or the command is
        // rejected; the pointer owns y while the drag is active.
        Behavior on y {
            enabled: !cardMouse.pressed
            NumberAnimation {
                duration: appTheme.reduceMotion ? 0 : appTheme.motionFadeMs
                easing.type: appTheme.motionEasing
            }
        }

        // Ancestor owner for unclaimed pointer input, matching Qan.NodeItem.
        // Nested Mask rows and action buttons consume their own presses.
        MouseArea {
            id: cardMouse
            objectName: "editorMaskGroupCardMouse"
            width: parent.width
            height: parent.height
            hoverEnabled: true
            focus: false
            preventStealing: root.reorderEnabled
            cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.PointingHandCursor
            drag.target: root.reorderEnabled ? dragBody : null
            drag.axis: Drag.YAxis
            drag.smoothed: false
            drag.filterChildren: false
            onPressed: {
                root._dragMoved = false
                if (root.reorderEnabled) {
                    root.reorderPressChanged(true)
                }
            }
            onReleased: {
                root.finishCardDrag()
                if (root.reorderEnabled) {
                    root.reorderPressChanged(false)
                }
            }
            onCanceled: {
                dragBody.y = 0
                root._dragMoved = false
                if (root.reorderEnabled) {
                    root.reorderPressChanged(false)
                }
                root.reorderCanceled()
            }
            onClicked: function (mouse) {
                mouse.accepted = true
                if (root._dragMoved) {
                    return
                }
                if (mouse.y >= root.headerHeight) {
                    return
                }
                header.forceActiveFocus(Qt.MouseFocusReason)
                root.activateHeader()
            }

            Rectangle {
                id: card
                objectName: "editorMaskGroupCard"
                anchors.fill: parent
                anchors.bottomMargin: appTheme.spaceXs
                radius: appTheme.controlRadiusSmall
                color: root.cardSurfaceColor
                border.width: root.selected ? root.selectionOutlineWidth : 1
                border.color: root.selected ? root.selectionOutlineColor
                                          : root.cardBorderColor
            }

            Column {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                spacing: 0

            Item {
                id: header
                objectName: "editorMaskGroupHeader"
                width: parent.width
                height: root.headerHeight
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: {
                    var name = root.displayName.length > 0 ? root.displayName
                                                           : qsTr("Mask Group")
                    var parts = [name, root.expanded ? qsTr("expanded") : qsTr("collapsed"),
                                 root.maskCount + " "
                                 + (root.maskCount === 1 ? qsTr("Mask") : qsTr("Masks"))]
                    if (root.deletionProtected) {
                        parts.push(qsTr("deletion locked"))
                    }
                    if (!root.groupEnabled) {
                        parts.push(qsTr("disabled"))
                    }
                    if (root.selected) {
                        parts.push(qsTr("selected"))
                    }
                    return parts.join(", ")
                }
                Accessible.description: {
                    if (!root.actionsEnabled) {
                        return root.actionsDisabledReason
                    }
                    if (root.deleteDisabledReason.length > 0) {
                        return root.deleteDisabledReason
                    }
                    const foldHint = root.expanded ? qsTr("Press Left to collapse")
                                                   : qsTr("Press Right to expand")
                    if (root.reorderEnabled) {
                        return foldHint + "; "
                               + qsTr("Drag to reorder or press Ctrl+Up or Ctrl+Down")
                    }
                    return foldHint
                }
                Accessible.onPressAction: root.activateHeader()
                Keys.priority: Keys.BeforeItem

                Keys.onPressed: function (event) {
                    if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                            || event.key === Qt.Key_Enter) {
                        root.activateHeader()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Right && !root.expanded) {
                        root.toggle()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Left && root.expanded) {
                        root.toggle()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Up
                               && (event.modifiers & Qt.ControlModifier) !== 0) {
                        if (root.reorderEnabled) {
                            root.reorderStepRequested(-1)
                        }
                        event.accepted = true
                    } else if (event.key === Qt.Key_Down
                               && (event.modifiers & Qt.ControlModifier) !== 0) {
                        if (root.reorderEnabled) {
                            root.reorderStepRequested(1)
                        }
                        event.accepted = true
                    } else if (event.key === Qt.Key_Up) {
                        root.headerNavigate(-1)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Down) {
                        root.headerNavigate(1)
                        event.accepted = true
                    } else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)
                               && root.deleteDisabledReason.length === 0) {
                        root.deleteClicked()
                        event.accepted = true
                    }
                }

                Rectangle {
                    id: headerWash
                    objectName: "editorMaskGroupHeaderWash"
                    anchors.fill: parent
                    anchors.margins: appTheme.graphSelectionOutlineWidth
                    radius: appTheme.controlRadiusSmall - appTheme.graphSelectionOutlineWidth
                    color: root.ownerActive || root.headerHovered || header.activeFocus
                           ? root.hoverColor : "transparent"
                    border.width: header.activeFocus && !root.selected ? 1 : 0
                    border.color: root.headerInkColor
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceSm
                    anchors.rightMargin: appTheme.spaceXs
                    spacing: appTheme.spaceSm

                    Item {
                        id: chevronZone
                        objectName: "editorMaskGroupChevronZone"
                        Layout.preferredWidth: appTheme.spaceMd + appTheme.spaceXs
                        Layout.preferredHeight: root.headerHeight
                        Accessible.ignored: true

                        Canvas {
                            id: chevron
                            objectName: "editorMaskGroupChevron"
                            anchors.centerIn: parent
                            width: appTheme.spaceMd
                            height: appTheme.spaceMd
                            antialiasing: true
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                ctx.strokeStyle = root.headerInkColor
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
                                function onHeaderInkColorChanged() {
                                    chevron.requestPaint()
                                }
                            }
                        }
                    }

                    // Composite preview slot. The column stays reserved for empty
                    // groups so every group name starts at the same x; the dark
                    // well only appears once the group actually owns Masks — an
                    // empty box would pretend coverage exists (NM9.4 fills it).
                    Item {
                        objectName: "editorMaskGroupPreviewSlot"
                        Layout.preferredWidth: appTheme.maskGroupPreviewSize
                        Layout.preferredHeight: appTheme.maskGroupPreviewSize
                        Layout.alignment: Qt.AlignVCenter
                        Accessible.ignored: true

                        Rectangle {
                            objectName: "editorMaskGroupPreview"
                            anchors.fill: parent
                            visible: root.maskCount > 0
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.bgBaseColor
                            border.width: 1
                            border.color: "transparent"

                            ColorImage {
                                anchors.centerIn: parent
                                width: appTheme.iconOpticalSize
                                height: appTheme.iconOpticalSize
                                source: "qrc:/panel_icons/masks.svg"
                                sourceSize.width: appTheme.iconSourceSize
                                sourceSize.height: appTheme.iconSourceSize
                                fillMode: Image.PreserveAspectFit
                                smooth: true
                                color: root.mutedColor
                                visible: groupThumb.source.toString().length === 0
                            }

                            Image {
                                id: groupThumb
                                objectName: "editorMaskGroupPreviewImage"
                                anchors.fill: parent
                                source: root.groupThumbUrl
                                cache: false
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                                smooth: true
                                visible: source.toString().length > 0
                            }
                        }
                    }

                    Text {
                        id: groupName
                        objectName: "editorMaskGroupName"
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        text: root.displayName
                        color: root.headerInkColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        font.weight: root.selected || root.ownerActive
                                     ? appTheme.fontWeightStrong
                                     : appTheme.fontWeightRegular
                        wrapMode: Text.Wrap
                        Accessible.ignored: true
                        ToolTip.visible: root.headerHovered && truncated
                        ToolTip.text: root.displayName
                    }

                    Item {
                        Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
                        Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                        Layout.alignment: Qt.AlignVCenter

                        IconActionButton {
                            id: lockButton
                            objectName: "editorMaskGroupLockButton"
                            anchors.fill: parent
                            compact: true
                            stretchInLayout: true
                            enabled: root.actionsEnabled
                            selected: root.deletionProtected
                            iconSrc: root.deletionProtected ? "qrc:/panel_icons/lock.svg"
                                                            : "qrc:/panel_icons/lock-open.svg"
                            iconColorDefault: root.deletionProtected ? root.textColor
                                                                     : root.mutedColor
                            iconColorMuted: root.mutedColor
                            fillIdle: "transparent"
                            fillHover: root.hoverColor
                            fillPressed: appTheme.buttonPressedFillColor
                            focusRingColor: root.headerInkColor
                            actionName: root.deletionProtected
                                        ? qsTr("Unlock %1").arg(root.displayName)
                                        : qsTr("Lock %1 against deletion").arg(root.displayName)
                            toolTipText: enabled ? actionName : root.actionsDisabledReason
                            focusOnPointerPress: false
                            onClicked: root.lockClicked()
                        }
                    }

                    Item {
                        Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
                        Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                        Layout.alignment: Qt.AlignVCenter

                        IconActionButton {
                            id: deleteButton
                            objectName: "editorMaskGroupDeleteButton"
                            anchors.fill: parent
                            compact: true
                            stretchInLayout: true
                            enabled: root.deleteDisabledReason.length === 0
                            iconSrc: "qrc:/panel_icons/trash.svg"
                            iconColorDefault: root.mutedColor
                            iconColorMuted: root.mutedColor
                            fillIdle: "transparent"
                            fillHover: root.hoverColor
                            fillPressed: appTheme.buttonPressedFillColor
                            focusRingColor: root.headerInkColor
                            actionName: qsTr("Delete %1").arg(root.displayName)
                            toolTipText: root.deleteDisabledReason.length > 0
                                         ? root.deleteDisabledReason
                                         : actionName
                            focusOnPointerPress: false
                            onClicked: root.deleteClicked()
                        }
                    }
                }
            }

            Item {
                id: body
                objectName: "editorMaskGroupBody"
                width: parent.width
                height: root.bodyHeight
                visible: root.foldProgress > 0.001
                opacity: root.foldProgress
                clip: true
                Accessible.ignored: root.foldProgress < 0.001

                Column {
                    id: maskList
                    objectName: "editorMaskGroupMaskList"
                    width: parent.width
                    spacing: 0

                    Repeater {
                        id: maskRepeater
                        objectName: "editorMaskGroupMaskRepeater"
                        model: root.masks

                        EditorMaskGroupMaskRow {
                            id: maskRow
                            width: maskList.width
                            nodeId: root.nodeId
                            maskId: modelData.maskId !== undefined ? String(modelData.maskId) : ""
                            sourceKind: modelData.sourceKind !== undefined
                                        ? String(modelData.sourceKind) : ""
                            displayName: modelData.displayName !== undefined
                                         ? String(modelData.displayName) : ""
                            maskEnabled: modelData.enabled !== undefined ? modelData.enabled === true
                                                                         : true
                            opacityValue: modelData.opacity !== undefined
                                          ? Number(modelData.opacity) : 1.0
                            selected: root.selectedMaskId.length > 0
                                      && maskRow.maskId === root.selectedMaskId
                            actionsEnabled: root.actionsEnabled
                            actionsDisabledReason: root.actionsDisabledReason
                            textColor: root.textColor
                            mutedColor: root.mutedColor
                            hoverColor: root.hoverColor
                            selectionOutlineColor: root.selectionOutlineColor
                            selectionOutlineWidth: root.selectionOutlineWidth
                            maskThumbnails: root.maskThumbnails
                            onClicked: root.maskClicked(index)
                            onDeleteClicked: root.maskDeleteClicked(index)
                            onNavigateUp: root.maskNavigateUp(index)
                            onNavigateDown: root.maskNavigateDown(index)
                        }
                    }

                    Label {
                        objectName: "editorMaskGroupEmpty"
                        width: maskList.width
                        height: root.maskCount === 0 ? appTheme.graphMaskRowHeight : 0
                        visible: root.maskCount === 0
                        leftPadding: appTheme.spaceSm + appTheme.spaceMd + appTheme.spaceXs
                                     + appTheme.maskGroupPreviewSize + appTheme.spaceSm
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("No masks")
                        color: root.mutedColor
                        elide: Text.ElideRight
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        Accessible.ignored: root.foldProgress < 0.001
                    }
                }
            }
            }
        }
    }

    // Reused delegates must not carry a drag offset into their next row.
    ListView.onReused: {
        dragBody.y = 0
        root._dragMoved = false
        root.refreshGroupThumb()
    }
}
