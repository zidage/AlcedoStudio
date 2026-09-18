import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Mask Groups page (layout option C — thumbnail-first). A second, flat
// projection of the same PipelineDocument DAG: one group row per backbone
// Color Grade plus its ordered Masks. This panel owns no layer or adjustment
// state — group rows read EditorNodeController.maskGroups, expansion persists
// on EditorNodeLayoutStore, and every command routes through the shared node
// controller / Mask creation adapter so Nodes and Mask Groups stay two views
// of one document.
//
// Scroll and expansion survive Loader teardown: contentY is exported through
// listContentY / restoreListContentY for the rail, and group expansion lives
// on the layout store keyed by NodeId, not on delegates.
Item {
    id: root
    objectName: "editorMaskGroupsPageBody"

    property var theme: null
    property var editorSession: null
    property var nodeController: null
    property var nodeLayoutStore: null

    readonly property var maskCreation: root.editorSession ? root.editorSession.maskCreation : null
    readonly property var maskThumbnails: root.nodeController ? root.nodeController.maskThumbnails
                                                              : null

    property var groupsModel: []
    property int expansionRevision: 0

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor

    readonly property string sessionStateName: root.editorSession
            ? String(root.editorSession.sessionState || "")
            : ""
    readonly property bool graphReady: root.nodeController && root.nodeController.hasSnapshot
    readonly property bool graphLoading: !root.graphReady
            && (root.sessionStateName === "Loading"
                || root.sessionStateName === "Acquiring"
                || root.sessionStateName === "Switching")
    readonly property bool noImage: !root.graphReady && !root.graphLoading
    readonly property bool incompleteDraft: root.nodeController
            && root.nodeController.incompleteDraft
    readonly property bool structureEditable: root.graphReady
            && root.nodeController && root.nodeController.canEditMaskGroupStructure

    // One accurate reason per blocking state; surfaces on disabled tooltips
    // and the draft notice.
    readonly property string structureDisabledReason: {
        if (!root.nodeController || !root.graphReady) {
            return qsTr("Open an image to edit Mask Groups")
        }
        if (root.nodeController.incompleteDraft) {
            return root.nodeController.incompleteDraftInstruction.length > 0
                   ? root.nodeController.incompleteDraftInstruction
                   : qsTr("Finish the node graph first")
        }
        if (root.nodeController.commandActive) {
            return qsTr("Updating node graph")
        }
        if (root.editorSession && !root.editorSession.canEdit) {
            return qsTr("The session is not editable")
        }
        return ""
    }

    readonly property string selectedNodeId: root.nodeController
            ? String(root.nodeController.selectedNodeId || "")
            : ""
    readonly property string selectedMaskId: root.maskCreation
            ? String(root.maskCreation.selectedMaskId || "")
            : ""

    // Rail contract: status text and scroll offset live outside the Loader so
    // page switches restore both.
    readonly property string statusMessage: root.nodeController
            ? String(root.nodeController.lastError || "")
            : ""
    readonly property real listContentY: groupsList ? groupsList.contentY : 0

    function restoreListContentY(y) {
        if (!groupsList) {
            return
        }
        groupsList.restoringContentY = true
        groupsList.preservedContentY = Math.max(0, Number(y || 0))
        Qt.callLater(function () {
            if (!groupsList) {
                return
            }
            var maxY = Math.max(0, groupsList.contentHeight - groupsList.height)
            groupsList.contentY = Math.max(0, Math.min(groupsList.preservedContentY, maxY))
            groupsList.restoringContentY = false
        })
    }

    function refreshGroupsModel() {
        if (groupsList) {
            groupsList.captureScrollBeforeReset()
        }
        root.groupsModel = root.nodeController ? root.nodeController.maskGroups : []
        if (groupsList) {
            groupsList.restoreScrollAfterReset()
        }
    }

    function groupExpanded(nodeId) {
        return !root.nodeLayoutStore || root.nodeLayoutStore.drawerOpen(nodeId)
    }

    function groupOwnsMask(groupRow, maskId) {
        if (!groupRow || maskId.length === 0 || !groupRow.masks) {
            return false
        }
        for (var i = 0; i < groupRow.masks.length; ++i) {
            const mask = groupRow.masks[i]
            if (mask && String(mask.maskId || "") === maskId) {
                return true
            }
        }
        return false
    }

    function ownerGroupIndexForMask(maskId) {
        for (var i = 0; i < root.groupsModel.length; ++i) {
            if (root.groupOwnsMask(root.groupsModel[i], maskId)) {
                return i
            }
        }
        return -1
    }

    function maskIdAt(groupIndex, maskIndex) {
        if (groupIndex < 0 || groupIndex >= root.groupsModel.length) {
            return ""
        }
        var masks = root.groupsModel[groupIndex].masks
        if (!masks || maskIndex < 0 || maskIndex >= masks.length) {
            return ""
        }
        return String(masks[maskIndex].maskId || "")
    }

    function maskDeletionProtectedAt(groupIndex, maskIndex) {
        if (groupIndex < 0 || groupIndex >= root.groupsModel.length) {
            return false
        }
        var masks = root.groupsModel[groupIndex].masks
        if (!masks || maskIndex < 0 || maskIndex >= masks.length) {
            return false
        }
        return masks[maskIndex].deletionProtected === true
    }

    function focusRow(groupIndex, maskIndex) {
        if (!groupsList) {
            return false
        }
        var delegate = groupsList.itemAtIndex(groupIndex)
        if (!delegate) {
            // Keyboard navigation may target a row outside the cache window;
            // bring it into view first, then focus the fresh delegate.
            groupsList.positionViewAtIndex(groupIndex, ListView.Contain)
            delegate = groupsList.itemAtIndex(groupIndex)
        }
        if (!delegate) {
            return false
        }
        if (maskIndex < 0) {
            delegate.focusHeader()
            return true
        }
        var row = delegate.maskRowAt(maskIndex)
        if (!row) {
            return false
        }
        row.forceActiveFocus(Qt.TabFocusReason)
        return true
    }

    // Logical row order: group header (-1) then its visible Mask rows, then the
    // next group header. Collapsed groups contribute only their header.
    function navigateRow(groupIndex, maskIndex, delta) {
        var count = root.groupsModel.length
        if (count === 0 || groupIndex < 0 || groupIndex >= count) {
            return
        }
        var g = groupIndex
        var m = maskIndex
        while (true) {
            if (delta > 0) {
                var group = root.groupsModel[g]
                var maskCount = group.masks ? group.masks.length : 0
                if (root.groupExpanded(String(group.nodeId || "")) && m + 1 < maskCount) {
                    m += 1
                } else {
                    g += 1
                    m = -1
                }
            } else {
                if (m > 0) {
                    m -= 1
                } else if (m === 0) {
                    m = -1
                } else {
                    g -= 1
                    if (g < 0) {
                        return
                    }
                    var prev = root.groupsModel[g]
                    var prevCount = prev.masks ? prev.masks.length : 0
                    m = root.groupExpanded(String(prev.nodeId || "")) && prevCount > 0
                        ? prevCount - 1
                        : -1
                }
            }
            if (g >= count) {
                return
            }
            if (root.focusRow(g, m)) {
                return
            }
        }
    }

    // Minimal positioning: only scroll when the target is outside the visible
    // window; a visible middle row must not jump on selection.
    function ensureItemVisible(item) {
        if (!item || !groupsList) {
            return
        }
        var top = item.mapToItem(groupsList.contentItem, 0, 0).y
        var bottom = top + item.height
        if (top < groupsList.contentY) {
            groupsList.contentY = Math.max(0, top - appTheme.spaceXs)
        } else if (bottom > groupsList.contentY + groupsList.height) {
            groupsList.contentY = Math.min(
                        Math.max(0, groupsList.contentHeight - groupsList.height),
                        bottom - groupsList.height + appTheme.spaceXs)
        }
    }

    function revealSelectedMask() {
        var maskId = root.selectedMaskId
        if (maskId.length === 0) {
            return
        }
        var ownerIndex = root.ownerGroupIndexForMask(maskId)
        if (ownerIndex < 0) {
            return
        }
        var owner = root.groupsModel[ownerIndex]
        var ownerId = String(owner.nodeId || "")
        if (root.nodeLayoutStore && !root.nodeLayoutStore.drawerOpen(ownerId)) {
            root.nodeLayoutStore.setDrawerOpen(ownerId, true)
        }
        var maskIndex = -1
        if (owner.masks) {
            for (var i = 0; i < owner.masks.length; ++i) {
                if (String(owner.masks[i].maskId || "") === maskId) {
                    maskIndex = i
                    break
                }
            }
        }
        Qt.callLater(function () {
            if (!groupsList) {
                return
            }
            var delegate = groupsList.itemAtIndex(ownerIndex)
            if (!delegate) {
                return
            }
            var row = maskIndex >= 0 ? delegate.maskRowAt(maskIndex) : null
            root.ensureItemVisible(row !== null ? row : delegate)
        })
    }

    function selectGroup(nodeId) {
        if (!root.nodeController || String(nodeId).length === 0) {
            return
        }
        root.nodeController.selectNode(nodeId)
    }

    function selectMaskFromGroup(nodeId, maskId) {
        if (!root.maskCreation || String(maskId).length === 0) {
            return
        }
        // Select the owning Grade first: it finishes any open Mask edit on
        // another group before the new selection lands on this Mask.
        root.selectGroup(nodeId)
        root.maskCreation.selectMask(nodeId, maskId)
    }

    function removeMaskFromGroup(nodeId, maskId) {
        if (!root.maskCreation || String(maskId).length === 0) {
            return
        }
        root.selectGroup(nodeId)
        root.maskCreation.removeMask(nodeId, maskId)
    }

    function toggleMaskLock(nodeId, maskId, locked) {
        if (!root.maskCreation || String(maskId).length === 0) {
            return
        }
        root.maskCreation.setMaskDeletionProtected(nodeId, maskId, locked)
    }

    function toggleGroupLock(nodeId, locked) {
        if (!root.nodeController) {
            return
        }
        root.nodeController.setColorGradeDeletionProtected(nodeId, locked)
    }

    function removeGroup(nodeId) {
        if (!root.nodeController) {
            return
        }
        root.nodeController.removeMaskGroup(nodeId)
    }

    // Drag-and-drop reorder entry point: targetIndex is the final position in
    // the downstream-first list (0 = nearest DRT/Post). One drop becomes one
    // Nodes-page topology delta and follows the same owner path as connector
    // edits.
    function moveGroupToIndex(nodeId, targetIndex) {
        if (!root.nodeController || !root.structureEditable) {
            return
        }
        root.nodeController.moveMaskGroupToIndex(nodeId, targetIndex)
    }

    function addMaskGroup() {
        if (!root.structureEditable) {
            return
        }
        root.nodeController.insertMaskGroupAtTop()
    }

    Connections {
        target: root.nodeController
        function onMaskGroupsChanged() {
            root.refreshGroupsModel()
        }
        function onSnapshotChanged() {
            root.refreshGroupsModel()
        }
    }

    Connections {
        target: root.nodeLayoutStore
        function onNodeHeightChanged() {
            root.expansionRevision += 1
        }
        function onLayoutChanged() {
            root.expansionRevision += 1
        }
    }

    // Reveal runs once per newly selected Mask — not on every
    // maskCreationChanged (parameter drags must not pull the viewport back).
    property string _lastRevealedMaskId: ""

    Connections {
        target: root.maskCreation
        function onMaskCreationChanged() {
            const maskId = root.selectedMaskId
            if (maskId === root._lastRevealedMaskId) {
                return
            }
            root._lastRevealedMaskId = maskId
            root.revealSelectedMask()
        }
    }

    Component.onCompleted: refreshGroupsModel()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appTheme.spaceLg
        spacing: appTheme.spaceMd

        RowLayout {
            objectName: "editorMaskGroupsPanelHeader"
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Label {
                objectName: "editorMaskGroupsPanelTitle"
                Layout.fillWidth: true
                text: qsTr("Mask Groups")
                color: root.colText
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightHeading
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Mask Groups")
            }

            IconActionButton {
                id: addButton
                objectName: "editorMaskGroupsAddButton"
                compact: true
                enabled: root.structureEditable
                iconSrc: "qrc:/panel_icons/plus.svg"
                iconColorDefault: root.colMuted
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Add Mask Group")
                toolTipText: enabled ? actionName : root.structureDisabledReason
                onClicked: root.addMaskGroup()
            }
        }

        ColumnLayout {
            objectName: "editorMaskGroupsDraftNotice"
            Layout.fillWidth: true
            spacing: appTheme.spaceXs
            visible: root.incompleteDraft

            Label {
                objectName: "editorMaskGroupsDraftInstruction"
                Layout.fillWidth: true
                text: root.nodeController ? root.nodeController.incompleteDraftInstruction : ""
                color: root.colMuted
                wrapMode: Text.WordWrap
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            Repeater {
                objectName: "editorMaskGroupsDraftLocateRepeater"
                model: root.nodeController ? root.nodeController.detachedDraftNodeIds : []

                IconActionButton {
                    objectName: "editorMaskGroupsDraftLocateButton"
                    compact: true
                    iconSrc: "qrc:/panel_icons/nodes.svg"
                    iconColorDefault: root.colMuted
                    iconColorMuted: root.colMuted
                    fillIdle: root.colCardSurface
                    fillHover: appTheme.buttonHoveredFillColor
                    fillPressed: appTheme.buttonPressedFillColor
                    focusRingColor: root.colText
                    actionName: qsTr("Locate unfinished node %1 in Nodes")
                              .arg(index + 1)
                    onClicked: {
                        if (root.nodeController) {
                            root.nodeController.locateNodeInGraph(String(modelData))
                        }
                    }
                }
            }
        }

        Label {
            objectName: "editorMaskGroupsCommandError"
            Layout.fillWidth: true
            visible: root.nodeController && root.nodeController.lastError.length > 0
            text: root.nodeController ? root.nodeController.lastError : ""
            color: appTheme.dangerColor
            wrapMode: Text.WordWrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Item {
            objectName: "editorMaskGroupsListWell"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Rectangle {
                anchors.fill: parent
                radius: appTheme.controlRadiusSmall
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: root.colCardBorder
            }

            ListView {
                id: groupsList
                objectName: "editorMaskGroupsList"
                anchors.fill: parent
                anchors.margins: appTheme.spaceSm
                clip: true
                model: root.groupsModel
                spacing: appTheme.spaceSm
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true

                // Same scroll-preservation contract as the history list: a
                // projection rebuild reassigns the model, which must not pin
                // the viewport back to the top while the selection is visible.
                property real preservedContentY: 0
                property bool restoringContentY: false

                // Drag reorder state. groupPointerIndex is the row currently
                // holding the pointer (disables flick immediately on press).
                // groupDragSourceIndex / groupDragSlot are only set once the
                // card actually moves; -1 means no drag is in progress.
                property int groupPointerIndex: -1
                property int groupDragSourceIndex: -1
                property int groupDragSlot: -1
                interactive: groupPointerIndex < 0

                // Nearest insertion boundary for a card position in content
                // coordinates: the first row whose midpoint sits below the
                // dragged card. Off-screen (non-instantiated) rows are skipped,
                // so the slot can never jump beyond the visible range.
                function groupDropSlot(contentY) {
                    var count = root.groupsModel.length
                    var slot = count
                    for (var i = 0; i < count; ++i) {
                        var item = itemAtIndex(i)
                        if (!item) {
                            continue
                        }
                        if (contentY < item.y + item.height / 2) {
                            slot = i
                            break
                        }
                        slot = i + 1
                    }
                    return Math.min(slot, count)
                }

                function beginGroupDrag(index) {
                    groupDragSourceIndex = index
                    groupDragSlot = index
                }

                function updateGroupDrag(contentY) {
                    groupDragSlot = groupDropSlot(contentY)
                }

                function noteGroupPointer(index, pressed) {
                    groupPointerIndex = pressed ? index : -1
                }

                function cancelGroupDrag() {
                    groupPointerIndex = -1
                    groupDragSourceIndex = -1
                    groupDragSlot = -1
                }

                // Removing the source row shifts every boundary after it down
                // by one, so a slot past the source maps to slot - 1.
                function finishGroupDrag(nodeId, index, contentY) {
                    var source = groupDragSourceIndex >= 0 ? groupDragSourceIndex
                                                         : index
                    var slot = groupDropSlot(contentY)
                    groupPointerIndex = -1
                    groupDragSourceIndex = -1
                    groupDragSlot = -1
                    var count = root.groupsModel.length
                    var target = slot > source ? slot - 1 : slot
                    target = Math.max(0, Math.min(count - 1, target))
                    if (target !== source && String(nodeId).length > 0) {
                        root.moveGroupToIndex(nodeId, target)
                    }
                }

                // Hairline position for the current insertion boundary: the gap
                // between rows slot-1 and slot, centered on the list spacing.
                // Boundary slots clamp into the content rect — the ideal gap
                // position sits half a spacing outside the first/last card,
                // which the ListView clip would hide entirely.
                function groupDropIndicatorY() {
                    var maxY = Math.max(0, contentHeight - 2)
                    var slot = groupDragSlot
                    if (slot <= 0) {
                        var first = itemAtIndex(0)
                        return Math.max(0, Math.min(maxY,
                            first ? first.y - spacing / 2 - 1 : 0))
                    }
                    var previous = itemAtIndex(slot - 1)
                    if (previous) {
                        return Math.max(0, Math.min(maxY,
                            previous.y + previous.height + spacing / 2 - 1))
                    }
                    return maxY
                }

                onContentYChanged: {
                    if (!restoringContentY) {
                        preservedContentY = contentY
                    }
                }

                function captureScrollBeforeReset() {
                    if (!restoringContentY) {
                        preservedContentY = contentY
                    }
                    restoringContentY = true
                }

                function restoreScrollAfterReset() {
                    restoringContentY = true
                    Qt.callLater(function () {
                        if (!groupsList) {
                            return
                        }
                        var maxY = Math.max(0, groupsList.contentHeight - groupsList.height)
                        groupsList.contentY = Math.max(
                                    0, Math.min(groupsList.preservedContentY, maxY))
                        groupsList.restoringContentY = false
                    })
                }

                delegate: EditorMaskGroupDelegate {
                    nodeId: modelData.nodeId !== undefined ? String(modelData.nodeId) : ""
                    displayName: modelData.displayName !== undefined
                                 ? String(modelData.displayName) : ""
                    groupEnabled: modelData.enabled !== undefined ? modelData.enabled === true
                                                                  : true
                    deletionProtected: modelData.deletionProtected === true
                    masks: modelData.masks !== undefined && modelData.masks !== null
                           ? modelData.masks : []
                    selectedMaskId: root.selectedMaskId
                    ownerActive: root.groupOwnsMask(modelData, root.selectedMaskId)
                    selected: nodeId.length > 0 && nodeId === root.selectedNodeId
                              && !ownerActive
                    expanded: {
                        root.expansionRevision
                        return root.nodeLayoutStore
                               ? root.nodeLayoutStore.drawerOpen(nodeId)
                               : true
                    }
                    actionsEnabled: root.structureEditable
                    actionsDisabledReason: root.structureDisabledReason
                    reorderEnabled: root.structureEditable && root.groupsModel.length > 1
                    textColor: root.colText
                    mutedColor: root.colMuted
                    hoverColor: appTheme.hoverColor
                    cardSurfaceColor: root.colCardSurface
                    cardBorderColor: root.colCardBorder
                    selectionOutlineColor: appTheme.graphSelectionOutlineColor
                    selectionOutlineWidth: appTheme.graphSelectionOutlineWidth
                    maskThumbnails: root.maskThumbnails
                    width: groupsList.width

                    onHeaderClicked: root.selectGroup(nodeId)
                    onExpansionToggled: function (nextExpanded) {
                        if (root.nodeLayoutStore) {
                            root.nodeLayoutStore.setDrawerOpen(nodeId, nextExpanded)
                        }
                    }
                    onLockClicked: root.toggleGroupLock(nodeId, !deletionProtected)
                    onDeleteClicked: root.removeGroup(nodeId)
                    onReorderDragStarted: groupsList.beginGroupDrag(index)
                    onReorderDragMoved: function (contentY) {
                        groupsList.updateGroupDrag(contentY)
                    }
                    onReorderDropped: function (contentY) {
                        groupsList.finishGroupDrag(nodeId, index, contentY)
                    }
                    onReorderPressChanged: function (pressed) {
                        groupsList.noteGroupPointer(index, pressed)
                    }
                    onReorderCanceled: groupsList.cancelGroupDrag()
                    onReorderStepRequested: function (delta) {
                        root.moveGroupToIndex(nodeId, index + delta)
                    }
                    onMaskClicked: function (maskIndex) {
                        root.selectMaskFromGroup(nodeId, root.maskIdAt(index, maskIndex))
                    }
                    onMaskLockClicked: function (maskIndex) {
                        root.toggleMaskLock(nodeId, root.maskIdAt(index, maskIndex),
                                            !root.maskDeletionProtectedAt(index, maskIndex))
                    }
                    onMaskDeleteClicked: function (maskIndex) {
                        root.removeMaskFromGroup(nodeId, root.maskIdAt(index, maskIndex))
                    }
                    onMaskNavigateUp: function (maskIndex) {
                        root.navigateRow(index, maskIndex, -1)
                    }
                    onMaskNavigateDown: function (maskIndex) {
                        root.navigateRow(index, maskIndex, 1)
                    }
                    onHeaderNavigate: function (delta) {
                        root.navigateRow(index, -1, delta)
                    }
                }

                // Drop-slot hairline: tracks groupDragSlot while a card is
                // dragged. Lives in the content item so it scrolls with rows.
                Rectangle {
                    id: dropIndicator
                    objectName: "editorMaskGroupDropIndicator"
                    z: 3
                    width: groupsList.width
                    height: 2
                    radius: 1
                    color: appTheme.accentColor
                    visible: groupsList.groupDragSlot >= 0
                    y: groupsList.groupDropIndicatorY()
                }
            }

            Label {
                objectName: "editorMaskGroupsEmptyState"
                anchors.fill: parent
                anchors.margins: appTheme.spaceLg
                visible: root.noImage
                text: qsTr("Select an image to edit Mask Groups")
                color: root.colMuted
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Select an image to edit Mask Groups")
            }

            Label {
                objectName: "editorMaskGroupsLoadingState"
                anchors.fill: parent
                anchors.margins: appTheme.spaceLg
                visible: root.graphLoading
                text: qsTr("Loading Mask Groups")
                color: root.colMuted
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Loading Mask Groups")
            }

            Label {
                objectName: "editorMaskGroupsNoGroups"
                anchors.fill: parent
                anchors.margins: appTheme.spaceLg
                visible: root.graphReady && root.groupsModel.length === 0
                text: qsTr("No Mask Groups")
                color: root.colMuted
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("No Mask Groups")
            }
        }
    }
}
