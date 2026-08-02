import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Bottom filmstrip dock for EditorWorkspace.
// Phase 7C: the editor filmstrip reuses the library thumbnail model while
// keeping its own horizontal selection and focus surface.
// Motion / surface tokens: DESIGN.md.
Item {
    id: root
    objectName: "editorFilmstrip"

    property var theme: null
    property var host: null
    property var editorSession: null
    // InteractionPolicyController is the authoritative save-checkpoint gate.
    property var interactionPolicy: null
    property var selectedImagesById: ({})
    property bool collapsed: editorSession ? editorSession.filmstripCollapsed : false
    // Minimum scale = current default dock proportion; live max = 50% of window.
    readonly property real minExpandedHeight: 128
    readonly property real hostWindowHeight: {
        const win = Window.window
        return win && win.height > 0 ? Number(win.height) : 0
    }
    readonly property real maxExpandedHeight: hostWindowHeight > 0
            ? Math.max(minExpandedHeight, hostWindowHeight * 0.5)
            : 4096
    // Drag uses a local override so QSettings is not written on every pixel.
    property real _dragExpandedHeight: -1
    readonly property real expandedHeight: {
        if (_dragExpandedHeight > 0)
            return _dragExpandedHeight
        const persisted = editorSession ? Number(editorSession.filmstripExpandedHeight) : minExpandedHeight
        return root.clampExpandedHeight(persisted)
    }
    readonly property var libraryModule: (typeof appModules !== "undefined" && appModules)
                                         ? appModules.library : null
    readonly property var thumbnailModel: libraryModule ? libraryModule.thumbnailModel : null
    readonly property var workspaceRouter: (typeof appModules !== "undefined" && appModules)
                                           ? appModules.workspaceRouter : null
    readonly property int selectedElementId: editorSession ? Number(editorSession.elementId) : 0
    readonly property int selectedIndex: thumbnailModel && selectedElementId > 0
                                        ? thumbnailModel.rowByElementId(selectedElementId) : -1
    readonly property int currentIndex: selectedIndex >= 0 ? selectedIndex + 1 : 0
    readonly property int totalCount: thumbnailModel ? Number(thumbnailModel.count) : 0
    property string currentFileName: ""
    readonly property bool saving: editorSession
                                    && (String(editorSession.sessionState) === "Saving"
                                        || String(editorSession.sessionState) === "Switching")
    readonly property bool renderBusy: editorSession ? Boolean(editorSession.renderBusy) : false
    // Fixed decode edge — resize only scales card geometry; never re-requests thumbs.
    readonly property int filmstripThumbnailMaxEdge: 512
    property int focusIndex: selectedIndex >= 0 ? selectedIndex : 0
    property int selectionAnchorIndex: -1
    property bool _listHadFocus: false
    property bool _restoringScroll: false
    property int _pendingRevealIndex: -1
    property bool _heightResizing: false
    readonly property bool selectionEnabled: editorSession
                                             ? Boolean(editorSession.actions.canSelectImage)
                                             : (interactionPolicy
                                                ? Boolean(interactionPolicy.canSelectEditorImage)
                                                : true)
    readonly property string selectionDisabledReason: editorSession
                                                     ? ""
                                                     : (interactionPolicy
                                                        ? String(interactionPolicy.selectEditorImageReason || "")
                                                        : "")
    property bool hasImage: editorSession ? editorSession.hasImage : false

    readonly property real handleHeight: 28
    // dockExpandProgress drives the downward fold (0 collapsed -> 1 expanded).
    // collapsed flips immediately (persisted session state); only the visual
    // height animates so the handle stays stationary and state assertions hold.
    // foldManualDrive + driveFoldProgress() pin intermediate geometry for tests.
    property real dockExpandProgress: 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs
    readonly property real dockHeight: handleHeight
                                       + (expandedHeight - handleHeight) * dockExpandProgress
    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colHover: appTheme.buttonHoveredFillColor
    readonly property color colCardSurface: appTheme.cardSurfaceColor
    readonly property color colCardBorder: appTheme.cardBorderColor
    readonly property int panelRadius: appTheme.panelRadius
    readonly property real contentX: filmstripListView ? filmstripListView.contentX : 0

    function clampExpandedHeight(value) {
        const lo = minExpandedHeight
        const hi = Math.max(lo, maxExpandedHeight)
        return Math.max(lo, Math.min(hi, Number(value)))
    }

    function setExpandedHeightLive(value) {
        _dragExpandedHeight = root.clampExpandedHeight(value)
    }

    function commitExpandedHeight() {
        const value = root.clampExpandedHeight(
            _dragExpandedHeight > 0 ? _dragExpandedHeight : expandedHeight)
        _dragExpandedHeight = -1
        if (editorSession && editorSession.filmstripExpandedHeight !== undefined) {
            editorSession.filmstripExpandedHeight = value
        }
    }

    function storeFilmstripScroll() {
        if (_restoringScroll || !filmstripListView) {
            return
        }
        const value = Math.max(0, filmstripListView.contentX)
        if (host && host.contentX !== undefined) {
            host.contentX = value
        }
        if (editorSession && editorSession.filmstripScrollPosition !== undefined) {
            editorSession.filmstripScrollPosition = value
        }
    }

    function savedFilmstripScroll() {
        if (host && host.contentX !== undefined) {
            return Number(host.contentX || 0)
        }
        return editorSession && editorSession.filmstripScrollPosition !== undefined
                ? Number(editorSession.filmstripScrollPosition || 0)
                : 0
    }

    function refreshCurrentFileName() {
        let name = ""
        if (thumbnailModel && selectedIndex >= 0 && thumbnailModel.getItemAt) {
            const row = thumbnailModel.getItemAt(selectedIndex)
            if (row && row.fileName) {
                name = String(row.fileName)
            }
        }
        currentFileName = name
    }

    function restoreFilmstripScroll() {
        if (!filmstripListView) {
            return
        }

        if (_pendingRevealIndex >= 0) {
            if (_pendingRevealIndex >= filmstripListView.count) {
                return
            }
            _restoringScroll = true
            filmstripListView.positionViewAtIndex(_pendingRevealIndex, ListView.Beginning)
            _restoringScroll = false
            _pendingRevealIndex = -1
            root.storeFilmstripScroll()
            return
        }

        const maxX = Math.max(0, filmstripListView.contentWidth - filmstripListView.width)
        const savedX = Math.max(0, root.savedFilmstripScroll())
        _restoringScroll = true
        filmstripListView.contentX = Math.min(savedX, maxX)
        _restoringScroll = false
    }

    function scheduleScrollRestore() {
        scrollRestoreTimer.restart()
    }

    function revealIndexAtBeginning(index) {
        const target = Number(index)
        if (target < 0 || target >= totalCount) {
            return
        }
        selectionAnchorIndex = target
        _pendingRevealIndex = target
        scheduleScrollRestore()
    }

    function applyFilmstripScrollTarget() {
        if (!host || !thumbnailModel || host.filmstripScrollTargetElementId === undefined) {
            return false
        }
        const elementId = Number(host.filmstripScrollTargetElementId || 0)
        if (elementId <= 0) {
            return false
        }

        let index = Number(host.filmstripScrollTargetIndex)
        if (index < 0 && thumbnailModel.rowByElementId) {
            index = thumbnailModel.rowByElementId(elementId)
        }
        if (index < 0 || index >= totalCount) {
            return false
        }

        root.revealIndexAtBeginning(index)
        if (host.clearFilmstripScrollTarget) {
            host.clearFilmstripScrollTarget(elementId)
        }
        return true
    }

    function notifyImageInteraction(item, index) {
        if (!item || !host) {
            return
        }
        if (host.setFocusedImage) {
            host.setFocusedImage(item)
        }
        if (host.requestLibraryScrollToElement) {
            host.requestLibraryScrollToElement(Number(item.elementId), index)
        }
    }

    function keyForElement(elementId) {
        return String(Number(elementId))
    }

    function isImageSelected(elementId) {
        return Object.prototype.hasOwnProperty.call(
            selectedImagesById || ({}), keyForElement(elementId))
    }

    function selectionItemForIndex(index) {
        if (!thumbnailModel || index < 0 || index >= totalCount) {
            return null
        }
        const row = thumbnailModel.getItemAt(index)
        if (!row || Number(row.elementId) <= 0 || Number(row.imageId) <= 0) {
            return null
        }
        return {
            elementId: Number(row.elementId),
            fileId: Number(row.fileId || row.elementId),
            imageId: Number(row.imageId),
            folderId: Number(row.folderId || 0),
            scopeType: row.scopeType ? String(row.scopeType) : "",
            fileName: row.fileName ? row.fileName : qsTr("(unnamed)"),
            rating: Number(row.rating || 0),
            isHdr: row.isHdr === true
        }
    }

    function selectionItemsForRange(firstIndex, lastIndex) {
        const first = Math.min(firstIndex, lastIndex)
        const last = Math.max(firstIndex, lastIndex)
        let rows = []
        if (thumbnailModel && thumbnailModel.getItemsInRange) {
            rows = thumbnailModel.getItemsInRange(first, last)
        } else {
            for (let index = first; index <= last; ++index) {
                const item = root.selectionItemForIndex(index)
                if (item) {
                    rows.push(item)
                }
            }
        }
        const items = []
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (!row || Number(row.elementId) <= 0 || Number(row.imageId) <= 0) {
                continue
            }
            items.push({
                elementId: Number(row.elementId),
                fileId: Number(row.fileId || row.elementId),
                imageId: Number(row.imageId),
                folderId: Number(row.folderId || 0),
                scopeType: row.scopeType ? String(row.scopeType) : "",
                fileName: row.fileName ? row.fileName : qsTr("(unnamed)"),
                rating: Number(row.rating || 0),
                isHdr: row.isHdr === true
            })
        }
        return items
    }

    function updateSelectionAnchor(index) {
        if (index >= 0) {
            selectionAnchorIndex = index
        }
    }

    function selectRangeToIndex(index, additive) {
        if (!selectionEnabled || index < 0) {
            return
        }
        const anchor = selectionAnchorIndex >= 0 ? selectionAnchorIndex : index
        if (libraryModule && libraryModule.LoadThumbnailsThroughIndex) {
            libraryModule.LoadThumbnailsThroughIndex(Math.max(anchor, index))
        }
        const rangeItems = root.selectionItemsForRange(anchor, index)
        if (additive) {
            root.replaceSelection(Object.values(selectedImagesById || ({})).concat(rangeItems))
        } else {
            root.replaceSelection(rangeItems)
        }
        updateSelectionAnchor(index)
    }

    function handleIndexSelection(index, modifiers, activate) {
        if (!selectionEnabled || index < 0 || index >= totalCount) {
            return
        }
        const item = root.selectionItemForIndex(index)
        if (!item) {
            return
        }

        focusCurrentIndex(index)
        filmstripListView.forceActiveFocus()
        const shift = (modifiers & Qt.ShiftModifier) !== 0
        const control = (modifiers & Qt.ControlModifier) !== 0
        if (shift) {
            root.selectRangeToIndex(index, control)
        } else if (control) {
            root.imageSelectionChanged(item.elementId, item.imageId, item.fileName,
                                       item.isHdr === true, !root.isImageSelected(item.elementId))
            root.updateSelectionAnchor(index)
        } else {
            root.replaceSelection([item])
            root.updateSelectionAnchor(index)
        }

        if (activate) {
            root.activateImage(index)
        } else {
            // Selection from this filmstrip must not move contentX. Only
            // cross-view requests (Library → applyFilmstripScrollTarget) may
            // scroll the strip so the peer can find the new image.
            root.notifyImageInteraction(item, index)
        }
    }

    function selectAllImages() {
        if (!selectionEnabled) {
            return
        }
        if (host && host.selectAllCurrentAlbum) {
            host.selectAllCurrentAlbum()
            return
        }
        const total = thumbnailModel
                ? Number(thumbnailModel.totalCount || totalCount) : 0
        if (total <= 0) {
            root.replaceSelection([])
            selectionAnchorIndex = -1
            return
        }
        if (libraryModule && libraryModule.LoadThumbnailsThroughIndex) {
            libraryModule.LoadThumbnailsThroughIndex(total - 1)
        }
        root.replaceSelection(root.selectionItemsForRange(0, total - 1))
        selectionAnchorIndex = 0
    }

    function restoreFocusAfterFold() {
        if (collapsed) {
            collapseHandle.forceActiveFocus()
        } else if (_listHadFocus) {
            filmstripListView.forceActiveFocus()
        }
    }

    function focusCurrentIndex(index) {
        if (totalCount <= 0) {
            focusIndex = -1
            return
        }
        focusIndex = Math.max(0, Math.min(totalCount - 1, index))
    }

    function moveFocus(delta, modifiers) {
        if (totalCount <= 0) {
            return
        }
        const nextIndex = Math.max(0, Math.min(totalCount - 1, focusIndex + delta))
        root.handleIndexSelection(nextIndex, modifiers || 0, false)
    }

    function activateFocused() {
        activateImage(focusIndex)
    }

    function driveFoldProgress(value) {
        foldManualDrive = true
        dockExpandProgress = Math.max(0, Math.min(1, value))
    }

    function endFoldDrive() {
        foldManualDrive = false
        dockExpandProgress = collapsed ? 0 : 1
    }

    onCollapsedChanged: {
        if (collapsed) {
            _listHadFocus = filmstripListView ? filmstripListView.activeFocus : false
            // Drop any in-flight resize so collapse geometry stays at handle height.
            if (_heightResizing || _dragExpandedHeight > 0) {
                commitExpandedHeight()
                _heightResizing = false
            }
        }
        _foldDuration = collapsed ? appTheme.motionFoldCloseMs : appTheme.motionFoldOpenMs
        if (!foldManualDrive) {
            dockExpandProgress = collapsed ? 0 : 1
        }
        focusRestoreTimer.restart()
    }
    onMaxExpandedHeightChanged: {
        if (_heightResizing)
            return
        if (editorSession && editorSession.filmstripExpandedHeight !== undefined) {
            const clamped = root.clampExpandedHeight(editorSession.filmstripExpandedHeight)
            if (Math.abs(clamped - Number(editorSession.filmstripExpandedHeight)) > 0.5) {
                editorSession.filmstripExpandedHeight = clamped
            }
        }
    }
    onSelectedIndexChanged: {
        // Update focus/label only. Do not scroll: open/selection from this
        // strip (or keyboard) must leave contentX alone. Library-originated
        // reveals go through applyFilmstripScrollTarget instead.
        refreshCurrentFileName()
        if (selectedIndex >= 0) {
            focusIndex = selectedIndex
        }
    }
    onTotalCountChanged: {
        refreshCurrentFileName()
        if (totalCount <= 0) {
            focusIndex = -1
        } else if (focusIndex < 0 || focusIndex >= totalCount) {
            focusIndex = Math.max(0, Math.min(totalCount - 1, selectedIndex))
        }
        applyFilmstripScrollTarget()
        scheduleScrollRestore()
    }
    Component.onCompleted: {
        // Snap to the persisted collapse state on load (no open animation).
        dockExpandProgress = collapsed ? 0 : 1
        _motionArmed = true
        refreshCurrentFileName()
        // Prefer an explicit Library reveal target; otherwise the list restores
        // the saved contentX (no jump to selected as leftmost).
        applyFilmstripScrollTarget()
    }
    Behavior on dockExpandProgress {
        enabled: root._motionArmed && !root.foldManualDrive
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : root._foldDuration
            easing.type: Easing.OutCubic
        }
    }

    Timer {
        id: scrollRestoreTimer
        interval: 0
        repeat: false
        onTriggered: root.restoreFilmstripScroll()
    }

    Timer {
        id: focusRestoreTimer
        interval: 0
        repeat: false
        onTriggered: root.restoreFocusAfterFold()
    }

    Connections {
        target: root.host
        ignoreUnknownSignals: true
        function onFilmstripScrollRequestIdChanged() {
            root.applyFilmstripScrollTarget()
        }
    }

    Connections {
        target: root.thumbnailModel
        ignoreUnknownSignals: true
        function onCountChanged() {
            root.refreshCurrentFileName()
            root.applyFilmstripScrollTarget()
            root.scheduleScrollRestore()
        }
        function onDataChanged() { root.refreshCurrentFileName() }
        function onModelReset() { root.refreshCurrentFileName() }
    }

    signal expandRequested()
    signal collapseRequested()
    signal toggleRequested()
    signal imageActivated(int index)
    signal imageSelectionChanged(int elementId, int imageId, string fileName, bool isHdr,
                                 bool selected)
    signal replaceSelection(var items)
    signal contextMenuRequested(var item, real sceneX, real sceneY)

    function activateImage(index) {
        if (!selectionEnabled || !thumbnailModel || index < 0 || index >= totalCount) {
            return
        }
        const row = thumbnailModel.getItemAt(index)
        if (!row || Number(row.elementId) <= 0 || Number(row.imageId) <= 0) {
            return
        }
        focusCurrentIndex(index)
        filmstripListView.forceActiveFocus()
        root.notifyImageInteraction(root.selectionItemForIndex(index), index)
        if (workspaceRouter && workspaceRouter.openEditor) {
            workspaceRouter.openEditor(Number(row.elementId), Number(row.imageId))
        }
        root.imageActivated(index)
    }

    // The filmstrip shares the Main-level image context menu with the Library
    // grid: this component only resolves the clicked row into a menu item and
    // forwards the request; all actions (rating, copy/paste, delete, albums,
    // Discard on the current image) are assembled and wired by Main.
    function contextMenuItemForIndex(index) {
        if (!thumbnailModel || index < 0 || index >= totalCount) {
            return null
        }
        const row = thumbnailModel.getItemAt(index)
        if (!row || !row.elementId || Number(row.elementId) <= 0 || Number(row.imageId) <= 0) {
            return null
        }
        return {
            elementId: Number(row.elementId),
            fileId: Number(row.fileId || row.elementId),
            imageId: Number(row.imageId),
            folderId: Number(row.folderId || 0),
            scopeType: row.scopeType ? String(row.scopeType) : "",
            fileName: row.fileName ? row.fileName : qsTr("(unnamed)"),
            rating: Number(row.rating),
            isHdr: row.isHdr === true
        }
    }

    function requestContextMenuForIndex(index, sceneX, sceneY) {
        const item = contextMenuItemForIndex(index)
        if (!item) {
            return
        }
        root.contextMenuRequested(item, sceneX, sceneY)
    }

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
        id: filmstripShell
        objectName: "editorFilmstripShell"
        anchors.fill: parent
        // Stable panel silhouette — always all four corners at panelRadius.
        // Do not couple radius to window active/focus; that looked like a bug
        // (corners only visible while the app is unfocused).
        radius: root.panelRadius
        antialiasing: true
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder
        clip: true

        // Persistent focusable handle — remains keyboard- and pointer-accessible
        // when the dock is collapsed so the released height returns to the viewport.
        // When expanded, vertical drag on this row resizes the dock; click still
        // toggles collapse (drag threshold separates the two gestures).
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
            Accessible.description: root.currentFileName.length > 0
                                    ? qsTr("Editing %1").arg(root.currentFileName)
                                    : qsTr("No image selected")
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
                objectName: "editorFilmstripHandleFocusFill"
                anchors.fill: parent
                radius: root.panelRadius
                antialiasing: true
                color: handleMouse.containsMouse || collapseHandle.activeFocus
                       ? root.colHover
                       : "transparent"
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceMd
                anchors.rightMargin: appTheme.spaceMd
                spacing: appTheme.spaceMd

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
                        return root.currentFileName.length > 0
                                ? root.currentFileName
                                : qsTr("Image %1").arg(Math.max(1, root.currentIndex))
                    }
                    color: root.colText
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
                    elide: Text.ElideRight
                }

                Label {
                    visible: !root.selectionEnabled
                             && root.selectionDisabledReason.length > 0
                    Layout.alignment: Qt.AlignVCenter
                    Layout.maximumWidth: 180
                    text: root.selectionDisabledReason
                    color: root.colMuted
                    font.pixelSize: appTheme.fontSizeCaption
                    elide: Text.ElideRight
                }
            }

            MouseArea {
                id: handleMouse
                objectName: "editorFilmstripResizeHandle"
                anchors.fill: parent
                hoverEnabled: true
                preventStealing: true
                cursorShape: root.collapsed
                             ? Qt.PointingHandCursor
                             : (root._heightResizing ? Qt.SizeVerCursor : Qt.SizeVerCursor)
                property real pressGlobalY: 0
                property real pressExpandedHeight: 0
                readonly property real dragThreshold: 4

                onPressed: function(mouse) {
                    pressGlobalY = mapToGlobal(mouse.x, mouse.y).y
                    pressExpandedHeight = root.expandedHeight
                    root._heightResizing = false
                }
                onPositionChanged: function(mouse) {
                    if (!pressed || root.collapsed)
                        return
                    const globalY = mapToGlobal(mouse.x, mouse.y).y
                    const delta = pressGlobalY - globalY  // drag up ⇒ taller dock
                    if (!root._heightResizing && Math.abs(delta) >= dragThreshold) {
                        root._heightResizing = true
                    }
                    if (root._heightResizing) {
                        root.setExpandedHeightLive(pressExpandedHeight + delta)
                    }
                }
                onReleased: function(mouse) {
                    if (root._heightResizing) {
                        root.commitExpandedHeight()
                        root._heightResizing = false
                        return
                    }
                    root._heightResizing = false
                    root.toggleCollapsed()
                }
                onCanceled: function() {
                    if (root._heightResizing) {
                        root.commitExpandedHeight()
                    }
                    root._heightResizing = false
                }
            }
        }

        // The body is clipped during the fold. The ListView keeps its expanded
        // delegate geometry even at zero visible body height, so thumbnail pins
        // are released only when a delegate is genuinely destroyed.
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

            ListView {
                id: filmstripListView
                objectName: "editorFilmstripListView"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                anchors.topMargin: appTheme.spaceXs
                height: Math.max(1, root.expandedHeight - root.handleHeight
                                    - appTheme.spaceSm - appTheme.spaceXs)
                orientation: ListView.Horizontal
                model: root.thumbnailModel
                spacing: appTheme.spaceSm
                clip: true
                cacheBuffer: 0
                boundsBehavior: Flickable.StopAtBounds
                interactive: true
                focus: true
                activeFocusOnTab: true
                keyNavigationEnabled: false

                onContentXChanged: root.storeFilmstripScroll()
                onContentWidthChanged: root.scheduleScrollRestore()
                onWidthChanged: root.scheduleScrollRestore()
                onCountChanged: root.scheduleScrollRestore()

                WheelHandler {
                    id: filmstripWheelHandler
                    target: null
                    onWheel: function(wheel) {
                        const delta = wheel.pixelDelta.x !== 0 ? wheel.pixelDelta.x
                                : (wheel.angleDelta.x !== 0 ? wheel.angleDelta.x
                                   : (wheel.pixelDelta.y !== 0
                                      ? wheel.pixelDelta.y : wheel.angleDelta.y))
                        const maxX = Math.max(0, filmstripListView.contentWidth
                                                 - filmstripListView.width)
                        filmstripListView.contentX = Math.max(
                            0, Math.min(maxX, filmstripListView.contentX - delta))
                        root.storeFilmstripScroll()
                        wheel.accepted = true
                    }
                }

                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_A
                            && (event.modifiers & Qt.ControlModifier)) {
                        root.selectAllImages()
                        event.accepted = true
                    } else if (event.key === Qt.Key_Left) {
                        root.moveFocus(-1, event.modifiers)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Right) {
                        root.moveFocus(1, event.modifiers)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Home) {
                        root.handleIndexSelection(0, event.modifiers, false)
                        event.accepted = true
                    } else if (event.key === Qt.Key_End) {
                        root.handleIndexSelection(root.totalCount - 1, event.modifiers, false)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                               || event.key === Qt.Key_Space) {
                        root.activateFocused()
                        event.accepted = true
                    }
                }

                Component.onCompleted: root.scheduleScrollRestore()

                delegate: Item {
                    id: thumbnailDelegate
                    objectName: "editorFilmstripTile"
                    required property int index
                    required property int elementId
                    required property int imageId
                    required property string fileName
                    required property string thumbUrl
                    required property bool thumbLoading
                    required property bool thumbMissingSource
                    required property string thumbErrorText

                    property string liveThumbUrl: thumbUrl
                    property bool liveThumbLoading: thumbLoading
                    property bool liveThumbMissingSource: thumbMissingSource
                    property string liveThumbErrorText: thumbErrorText
                    property int pinnedElementId: 0
                    property int pinnedImageId: 0
                    property int pinnedMaxEdge: 0

                    readonly property bool isCurrentImage: Number(elementId) === root.selectedElementId
                    readonly property bool isLibrarySelected: root.isImageSelected(elementId)
                    readonly property bool isSelected: isCurrentImage || isLibrarySelected
                    property int displayRating: 0
                    readonly property bool hasFocusFrame: root.focusIndex === index
                                                               && filmstripListView.activeFocus
                    readonly property bool thumbnailReady: liveThumbUrl.length > 0
                    readonly property bool thumbnailProblemState: !thumbnailReady
                                                                    && !liveThumbLoading
                                                                    && (liveThumbMissingSource
                                                                        || liveThumbErrorText.length > 0)
                    readonly property string thumbnailProblemText: liveThumbErrorText.length > 0
                                                                   ? liveThumbErrorText
                                                                   : qsTr("Source file was moved or deleted")

                    height: ListView.view ? ListView.view.height : 1
                    readonly property real fileNameLabelHeight: appTheme.lineHeightCaption
                    readonly property real thumbnailAreaHeight: Math.max(
                        1, height - fileNameLabelHeight - appTheme.spaceXs * 2)
                    width: Math.max(appTheme.spaceXl * 7, thumbnailAreaHeight * 1.55)
                    Accessible.role: Accessible.ListItem
                    Accessible.name: fileName.length > 0 ? fileName
                                                         : qsTr("Image %1").arg(index + 1)
                    Accessible.description: (isCurrentImage
                                             ? qsTr("Current image")
                                             : (isLibrarySelected ? qsTr("Selected image")
                                                                   : qsTr("Open image")))
                                            + qsTr(" | Rating %1/5").arg(displayRating)

                    function releasePinnedThumbnail() {
                        if (pinnedElementId !== 0 && pinnedImageId !== 0 && root.libraryModule) {
                            root.libraryModule.SetThumbnailVisible(pinnedElementId, pinnedImageId,
                                                                   false, pinnedMaxEdge)
                        }
                        pinnedElementId = 0
                        pinnedImageId = 0
                        pinnedMaxEdge = 0
                    }

                    function refreshRating() {
                        const row = root.thumbnailModel && root.thumbnailModel.getItemAt
                                ? root.thumbnailModel.getItemAt(index) : null
                        const value = row && row.rating !== undefined ? Number(row.rating) : 0
                        displayRating = value > 0 ? Math.min(5, Math.round(value)) : 0
                    }

                    function bindThumbnailLifetime() {
                        liveThumbUrl = thumbUrl
                        liveThumbLoading = thumbLoading
                        liveThumbMissingSource = thumbMissingSource
                        liveThumbErrorText = thumbErrorText
                        if (pinnedElementId === elementId && pinnedImageId === imageId
                                && pinnedMaxEdge === root.filmstripThumbnailMaxEdge) {
                            return
                        }
                        releasePinnedThumbnail()
                        pinnedElementId = Number(elementId)
                        pinnedImageId = Number(imageId)
                        pinnedMaxEdge = root.filmstripThumbnailMaxEdge
                        if (pinnedElementId !== 0 && pinnedImageId !== 0 && root.libraryModule) {
                            root.libraryModule.SetThumbnailVisible(pinnedElementId, pinnedImageId,
                                                                   true, pinnedMaxEdge)
                        }
                    }

                    function releaseThumbnailBinding() {
                        releasePinnedThumbnail()
                    }

                    onThumbUrlChanged: liveThumbUrl = thumbUrl
                    onThumbLoadingChanged: liveThumbLoading = thumbLoading
                    onThumbMissingSourceChanged: liveThumbMissingSource = thumbMissingSource
                    onThumbErrorTextChanged: liveThumbErrorText = thumbErrorText
                    Component.onCompleted: {
                        bindThumbnailLifetime()
                        refreshRating()
                    }
                    onElementIdChanged: {
                        bindThumbnailLifetime()
                        refreshRating()
                    }
                    onImageIdChanged: {
                        bindThumbnailLifetime()
                        refreshRating()
                    }
                    Component.onDestruction: releaseThumbnailBinding()

                    EditorFilmstripThumbnailCard {
                        id: thumbnailCard
                        anchors.fill: parent
                        fileName: thumbnailDelegate.fileName
                        imageIndex: thumbnailDelegate.index
                        liveThumbUrl: thumbnailDelegate.liveThumbUrl
                        liveThumbLoading: thumbnailDelegate.liveThumbLoading
                        thumbnailReady: thumbnailDelegate.thumbnailReady
                        thumbnailProblemState: thumbnailDelegate.thumbnailProblemState
                        thumbnailProblemText: thumbnailDelegate.thumbnailProblemText
                        selected: thumbnailDelegate.isSelected
                        hovered: thumbnailMouse.containsMouse
                        hasFocusFrame: thumbnailDelegate.hasFocusFrame
                        saving: root.saving
                        renderBusy: root.renderBusy
                        displayRating: thumbnailDelegate.displayRating
                        thumbnailMaxEdge: root.filmstripThumbnailMaxEdge
                    }

                    MouseArea {
                        id: thumbnailMouse
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: function(mouse) {
                            if (mouse.button === Qt.LeftButton) {
                                const hasSelectionModifier = (mouse.modifiers
                                                               & (Qt.ShiftModifier
                                                                  | Qt.ControlModifier)) !== 0
                                root.handleIndexSelection(thumbnailDelegate.index,
                                                          mouse.modifiers,
                                                          !hasSelectionModifier)
                            } else if (mouse.button === Qt.RightButton) {
                                const p = thumbnailMouse.mapToItem(null, mouse.x, mouse.y)
                                root.requestContextMenuForIndex(thumbnailDelegate.index, p.x, p.y)
                            }
                        }
                    }

                    Connections {
                        target: root.libraryModule
                        ignoreUnknownSignals: true
                        function onThumbnailUpdated(updatedElementId, updatedUrl, loading,
                                                    missingSource, errorText) {
                            if (Number(updatedElementId) === Number(thumbnailDelegate.elementId)) {
                                thumbnailDelegate.liveThumbUrl = updatedUrl || ""
                                thumbnailDelegate.liveThumbLoading = Boolean(loading)
                                thumbnailDelegate.liveThumbMissingSource = Boolean(missingSource)
                                thumbnailDelegate.liveThumbErrorText = errorText || ""
                            }
                        }
                    }

                    Connections {
                        target: root.thumbnailModel
                        ignoreUnknownSignals: true
                        function onDataChanged(topLeft, bottomRight) {
                            if (!topLeft || !bottomRight
                                    || (topLeft.row <= thumbnailDelegate.index
                                        && thumbnailDelegate.index <= bottomRight.row)) {
                                thumbnailDelegate.refreshRating()
                            }
                        }
                        function onModelReset() { thumbnailDelegate.refreshRating() }
                    }
                }
            }
        }

    }
}
