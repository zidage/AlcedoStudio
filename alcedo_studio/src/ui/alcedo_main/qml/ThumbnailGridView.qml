import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

Item {
    id: root
    clip: true
    readonly property color cardBg: appTheme.cardSurfaceColor
    readonly property color cardBgSelected: appTheme.selectedTintColor
    readonly property color cardBgHover: appTheme.hoverColor
    readonly property color cardMuted: appTheme.textMutedColor
    readonly property color cardText: appTheme.textColor
    readonly property color cardAccent: appTheme.accentColor
    readonly property color cardDanger: appTheme.dangerColor
    readonly property color cardDangerTint: appTheme.dangerTintColor
    readonly property int dataFontWeight: 500
    readonly property real dataLetterSpacing: -0.2

    property var selectedImagesById: ({})
    property var exportQueueById: ({})

    // ── Zoom control ──────────────────────────────────────────────────
    // zoomLevel maps to column counts: 2, 3, 4, 5, 6, 8, 11, 14
    readonly property var zoomColumns: [2, 3, 4, 5, 6, 8, 11, 14]
    // resolution tiers per zoom level (max edge px): 2048, 1024, 1024, 512, 512, 256, 256, 256
    readonly property var zoomResolutionEdges: [2048, 1024, 1024, 512, 512, 256, 256, 256]
    readonly property int zoomLevelCount: zoomColumns.length
    property int zoomLevel: 4  // default: 6 columns
    property bool zoomAdjusting: false
    property bool _zoomReady: false
    property int committedZoomLevel: Math.max(0, Math.min(zoomLevelCount - 1, zoomLevel))
    property bool thumbnailBindingSuspended: false
    property var _deferredThumbnailReleases: ({})
    property int selectionAnchorIndex: -1

    readonly property int columns: zoomColumns[Math.min(zoomLevel, zoomLevelCount - 1)]
    readonly property int desiredMaxEdge: zoomResolutionEdges[Math.min(committedZoomLevel, zoomLevelCount - 1)]
    readonly property bool compactText: columns >= 8
    readonly property bool hideMetadata: columns >= 8
    readonly property bool hideAllText: columns >= 14
    readonly property int cardInset: columns >= 11 ? 4 : (columns >= 8 ? 6 : 8)
    readonly property int delegateGap: columns >= 11 ? 4 : (columns >= 8 ? 6 : 12)
    readonly property int textAreaHeight: hideAllText ? 0 : (hideMetadata ? 18 : 42)
    readonly property int titleFontSize: columns >= 11 ? 9 : (columns >= 8 ? 10 : 12)
    readonly property int metadataFontSize: columns >= 8 ? 8 : 10
    property bool _inZoomToCursor: false
    property bool _zoomLayoutAnimating: false
    property var _zoomLayoutSnapshot: ({})
    property int _layoutZoomLevel: Math.max(0, Math.min(zoomLevelCount - 1, zoomLevel))
    property int _layoutRequestId: 0
    property int _pendingLayoutRequestId: 0
    property int _zoomRequestId: 0
    property int _pendingZoomRequestId: 0
    property real _pendingZoomFocusRatio: 0
    property real _pendingZoomFocusY: 0
    property int _pendingZoomTargetLevel: Math.max(0, Math.min(zoomLevelCount - 1, zoomLevel))
    readonly property int zoomSettleDelay: 90
    readonly property int thumbnailResumeDelay: 150
    readonly property int delegateReflowDuration: 280
    readonly property real delegateReflowTravelDamping: 0.32
    readonly property real delegateReflowMaxTravel: 44

    signal imageSelectionChanged(int elementId, int imageId, string fileName, bool isHdr,
                                 bool selected)
    signal replaceSelection(var items)
    signal imageFocused(var item)
    signal contextMenuRequested(var item, real sceneX, real sceneY)
    signal zoomChanged(int zoomLevel)

    function semanticTagTitle(rawTag) {
        const normalized = rawTag === undefined || rawTag === null
                ? ""
                : String(rawTag).trim().replace(/\s+/g, " ").toLowerCase()
        if (normalized.length === 0) {
            return ""
        }

        const displayNames = {
            "black and white": qsTr("Black & White"),
            "food and drink": qsTr("Food & Drink")
        }
        if (displayNames[normalized]) {
            return displayNames[normalized]
        }

        const smallWords = {
            "and": true,
            "of": true,
            "or": true
        }
        const words = normalized.split(" ")
        for (let i = 0; i < words.length; ++i) {
            const word = words[i]
            if (word.length === 0) {
                continue
            }
            if (i > 0 && smallWords[word]) {
                continue
            }
            words[i] = word.charAt(0).toUpperCase() + word.slice(1)
        }
        return words.join(" ")
    }

    function semanticTagsDisplayText(rawTags) {
        const text = rawTags === undefined || rawTags === null ? "" : String(rawTags).trim()
        if (text.length === 0) {
            return ""
        }

        const parts = []
        const seen = ({})
        const rawParts = text.split(",")
        for (let i = 0; i < rawParts.length; ++i) {
            const title = semanticTagTitle(rawParts[i])
            const key = title.toLowerCase()
            if (title.length === 0 || seen[key]) {
                continue
            }
            seen[key] = true
            parts.push(title)
        }
        if (parts.length === 0) {
            return ""
        }

        return parts.slice(0, 3).join(" / ")
    }

    Component.onCompleted: {
        committedZoomLevel = root.clampedZoomLevel(zoomLevel)
        _zoomReady = true
        root.relayoutAndClamp()
        root.updateCacheHint()
    }

    onZoomLevelChanged: {
        const clamped = root.clampedZoomLevel(zoomLevel)
        if (!_zoomReady) {
            committedZoomLevel = clamped
            return
        }
        root.zoomChanged(clamped)
        root.beginThumbnailBindingSuspension()
        if (!_inZoomToCursor) {
            root.prepareZoomLayoutTransition(clamped, grid.height * 0.5)
        }
        if (zoomAdjusting) {
            zoomCommitTimer.stop()
            return
        }
        zoomCommitTimer.restart()
    }
    onZoomAdjustingChanged: {
        if (!_zoomReady || zoomAdjusting || !thumbnailBindingSuspended) {
            return
        }
        root.finishZoomCommit()
    }
    onWidthChanged: {
        root.relayoutAndClamp()
        root.updateCacheHint()
    }
    onHeightChanged: {
        root.clampContentY()
        root.updateCacheHint()
    }

    function modelCount() {
        return appModules.library.thumbnailModel.count
    }

    function modelTotalCount() {
        return Math.max(modelCount(), appModules.library.thumbnailModel.totalCount)
    }

    function clampedZoomLevel(nextZoomLevel) {
        return Math.max(0, Math.min(zoomLevelCount - 1, nextZoomLevel))
    }

    function visualColumnsForZoomLevel(level) {
        return zoomColumns[root.clampedZoomLevel(level)]
    }

    function cardInsetForColumns(columnCount) {
        return columnCount >= 11 ? 4 : (columnCount >= 8 ? 6 : 8)
    }

    function delegateGapForColumns(columnCount) {
        return columnCount >= 11 ? 4 : (columnCount >= 8 ? 6 : 12)
    }

    function textAreaHeightForColumns(columnCount) {
        if (columnCount >= 14) {
            return 0
        }
        return columnCount >= 8 ? 18 : 42
    }

    function metricsForZoomLevel(level) {
        const visualCols = root.visualColumnsForZoomLevel(level)
        const inset = root.cardInsetForColumns(visualCols)
        const gap = root.delegateGapForColumns(visualCols)
        const textHeight = root.textAreaHeightForColumns(visualCols)
        const cellW = Math.max(72, Math.floor(grid.width / visualCols))
        const cellH = Math.max(64, Math.round((cellW - inset * 2) * 2 / 3
                                              + textHeight
                                              + inset * 2
                                              + gap))
        return {
            visualCols: visualCols,
            cols: Math.max(1, Math.floor(grid.width / cellW)),
            cellW: cellW,
            cellH: cellH,
            delegateGap: gap
        }
    }

    function contentHeightForMetrics(metrics) {
        if (!metrics || metrics.cellH <= 0) {
            return 0
        }
        return Math.ceil(modelTotalCount() / Math.max(1, metrics.cols)) * metrics.cellH
    }

    function clampedUnitScale(rawScale) {
        return Math.max(0.94, Math.min(1.06, rawScale))
    }

    function softenedReflowDelta(delta) {
        const sign = delta < 0 ? -1 : 1
        const softened = Math.abs(delta) * delegateReflowTravelDamping
        return sign * Math.min(delegateReflowMaxTravel, softened)
    }

    function thumbnailBindingKey(elementId, imageId, maxEdge) {
        return String(Number(elementId)) + ":" + String(Number(imageId)) + ":" + String(Number(maxEdge))
    }

    function beginThumbnailBindingSuspension() {
        resumeThumbnailBindingTimer.stop()
        if (thumbnailBindingSuspended) {
            return
        }
        thumbnailBindingSuspended = true
        _deferredThumbnailReleases = ({})
    }

    function deferThumbnailRelease(elementId, imageId, maxEdge) {
        if (elementId === 0 || imageId === 0) {
            return
        }
        const pending = Object.assign({}, _deferredThumbnailReleases)
        pending[thumbnailBindingKey(elementId, imageId, maxEdge)] = {
            elementId: Number(elementId),
            imageId: Number(imageId),
            maxEdge: Number(maxEdge)
        }
        _deferredThumbnailReleases = pending
    }

    function syncVisibleThumbnailBindings() {
        if (!grid.contentItem) {
            return
        }
        const children = grid.contentItem.children
        for (let i = 0; i < children.length; ++i) {
            const child = children[i]
            if (child && child.visible !== false && child.syncThumbnailBinding) {
                child.syncThumbnailBinding()
            }
        }
    }

    function flushDeferredThumbnailReleases() {
        const pending = _deferredThumbnailReleases
        _deferredThumbnailReleases = ({})
        for (const key in pending) {
            const release = pending[key]
            appModules.library.SetThumbnailVisible(release.elementId, release.imageId, false,
                                             release.maxEdge)
        }
    }

    // Exposed for library view-state restore across workspace Loader teardown.
    readonly property real contentY: grid.contentY
    function restoreContentY(y) {
        if (y === undefined || y === null) {
            return
        }
        grid.contentY = clampYForHeight(layoutContentHeight(), Number(y))
    }

    // Destroying the library mid-zoom must not leave deferred releases unprocessed;
    // otherwise the backend keeps treating those thumbnails as visible.
    Component.onDestruction: {
        resumeThumbnailBindingTimer.stop()
        thumbnailBindingSuspended = false
        flushDeferredThumbnailReleases()
    }

    function finishZoomCommit() {
        if (!_zoomReady) {
            return
        }
        committedZoomLevel = root.clampedZoomLevel(zoomLevel)
        grid.forceLayout()
        root.clampContentY()
        resumeThumbnailBindingTimer.restart()
    }

    function resumeThumbnailBindings() {
        thumbnailBindingSuspended = false
        root.syncVisibleThumbnailBindings()
        root.flushDeferredThumbnailReleases()
        root.clampContentY()
        root.updateCacheHint()
        loadMoreThumbnailTimer.restart()
    }

    function effectiveColumnCount() {
        if (grid.width <= 0 || grid.cellWidth <= 0) {
            return 1
        }
        return Math.max(1, Math.floor(grid.width / grid.cellWidth))
    }

    function estimatedContentHeight() {
        if (grid.cellHeight <= 0) {
            return 0
        }
        return Math.ceil(modelTotalCount() / effectiveColumnCount()) * grid.cellHeight
    }

    function loadedContentHeight() {
        if (grid.cellHeight <= 0) {
            return 0
        }
        return Math.ceil(modelCount() / effectiveColumnCount()) * grid.cellHeight
    }

    function layoutContentHeight() {
        const estimated = estimatedContentHeight()
        if (estimated > 0 || modelTotalCount() === 0) {
            return estimated
        }
        return grid.contentHeight
    }

    function maxContentY() {
        return maxContentYForHeight(layoutContentHeight(), grid.originY)
    }

    function maxContentYForHeight(contentHeight, originY) {
        return originY + Math.max(0, contentHeight - grid.height)
    }

    function clampYForHeight(contentHeight, contentY, originY) {
        const minY = originY === undefined ? grid.originY : originY
        return Math.max(minY, Math.min(maxContentYForHeight(contentHeight, minY), contentY))
    }

    function clampContentY() {
        if (_inZoomToCursor) {
            return
        }
        grid.contentY = clampYForHeight(layoutContentHeight(), grid.contentY)
    }

    function scrollBy(deltaY) {
        grid.contentY = clampYForHeight(layoutContentHeight(), grid.contentY + deltaY)
    }

    function relayoutAndClamp() {
        if (_inZoomToCursor) {
            return
        }
        _pendingLayoutRequestId = ++_layoutRequestId
        relayoutAndClampTimer.restart()
    }

    function finishRelayoutAndClamp() {
        if (_pendingLayoutRequestId !== _layoutRequestId || _inZoomToCursor) {
            return
        }
        grid.forceLayout()
        _layoutZoomLevel = root.clampedZoomLevel(zoomLevel)
        root.clampContentY()
        root.updateCacheHint()
    }

    function setZoomLevelAt(nextZoomLevel, focusViewportY) {
        const clamped = Math.max(0, Math.min(zoomLevelCount - 1, nextZoomLevel))
        if (clamped === zoomLevel) {
            return
        }

        root.prepareZoomLayoutTransition(clamped, focusViewportY)
        zoomLevel = clamped
    }

    function prepareZoomLayoutTransition(targetZoomLevel, focusViewportY) {
        const target = root.clampedZoomLevel(targetZoomLevel)
        const oldLevel = root.clampedZoomLevel(_layoutZoomLevel)
        const oldMetrics = root.metricsForZoomLevel(oldLevel)
        if (grid.width <= 0 || grid.height <= 0 || oldMetrics.cellW <= 0 || oldMetrics.cellH <= 0) {
            root.relayoutAndClamp()
            return
        }

        root.finishActiveZoomLayoutAnimations()
        const oldOriginY = grid.originY
        const oldHeight = Math.max(1, root.contentHeightForMetrics(oldMetrics))
        const oldContentY = clampYForHeight(oldHeight, grid.contentY, oldOriginY)
        const focusY = Math.max(0, Math.min(grid.height, focusViewportY))
        const focusRatio = (oldContentY - oldOriginY + focusY) / oldHeight

        _zoomLayoutSnapshot = {
            cols: oldMetrics.cols,
            cellW: oldMetrics.cellW,
            cellH: oldMetrics.cellH,
            delegateGap: oldMetrics.delegateGap,
            originY: oldOriginY,
            contentX: grid.contentX,
            contentY: oldContentY,
            viewportH: grid.height
        }
        _inZoomToCursor = true
        _zoomLayoutAnimating = true
        ++_layoutRequestId
        _pendingZoomRequestId = ++_zoomRequestId
        _pendingZoomFocusRatio = focusRatio
        _pendingZoomFocusY = focusY
        _pendingZoomTargetLevel = target
        zoomToCursorTimer.restart()
    }

    function finishZoomToCursor() {
        if (_pendingZoomRequestId !== _zoomRequestId) {
            return
        }
        grid.forceLayout()
        const newOriginY = grid.originY
        const newHeight = Math.max(1, layoutContentHeight())
        const targetY = newOriginY + _pendingZoomFocusRatio * newHeight - _pendingZoomFocusY
        grid.contentY = clampYForHeight(newHeight, targetY, newOriginY)
        _inZoomToCursor = false
        _layoutZoomLevel = _pendingZoomTargetLevel
        zoomLayoutApplyTimer.restart()
        root.updateCacheHint()
    }

    function applyZoomLayoutAnimation() {
        const snap = _zoomLayoutSnapshot
        if (!snap || !snap.cols || snap.cols <= 0 || snap.cellW <= 0 || snap.cellH <= 0) {
            _zoomLayoutAnimating = false
            return
        }
        const newCols = root.effectiveColumnCount()
        const newCellW = grid.cellWidth
        const newCellH = grid.cellHeight
        const newOriginY = grid.originY
        const newContentX = grid.contentX
        const newContentY = grid.contentY
        if (newCellW <= 0 || newCellH <= 0) {
            _zoomLayoutAnimating = false
            return
        }
        const children = grid.contentItem.children
        let animCount = 0
        for (let i = 0; i < children.length; i++) {
            const child = children[i]
            if (!child || child.index === undefined || !child.startZoomLayoutAnim) {
                continue
            }
            const idx = child.index
            const oldCol = idx % snap.cols
            const oldRow = Math.floor(idx / snap.cols)
            const oldX = oldCol * snap.cellW
            const oldY = oldRow * snap.cellH + snap.originY
            const newCol = idx % newCols
            const newRow = Math.floor(idx / newCols)
            const newX = newCol * newCellW
            const newY = newRow * newCellH + newOriginY
            const oldViewportX = oldX - snap.contentX
            const oldViewportY = oldY - snap.contentY
            const newViewportX = newX - newContentX
            const newViewportY = newY - newContentY
            const dx = oldViewportX - newViewportX
            const dy = oldViewportY - newViewportY
            const oldBottom = oldViewportY + snap.cellH
            const newBottom = newViewportY + newCellH
            const wasNearViewport = oldBottom >= -snap.cellH
                    && oldViewportY <= snap.viewportH + snap.cellH
            const isNearViewport = newBottom >= -newCellH
                    && newViewportY <= grid.height + newCellH
            if (!wasNearViewport && !isNearViewport) {
                continue
            }

            const oldDelegateW = Math.max(1, snap.cellW - snap.delegateGap)
            const oldDelegateH = Math.max(1, snap.cellH - snap.delegateGap)
            const newDelegateW = Math.max(1, newCellW - root.delegateGap)
            const newDelegateH = Math.max(1, newCellH - root.delegateGap)
            const travel = Math.sqrt(dx * dx + dy * dy)
            const largeTravel = travel > Math.max(96, newCellH * 0.82)
            const easedDx = largeTravel ? 0 : root.softenedReflowDelta(dx)
            const easedDy = largeTravel ? root.softenedReflowDelta(dy) * 0.25
                                        : root.softenedReflowDelta(dy)
            const rawScale = wasNearViewport
                    ? Math.sqrt((oldDelegateW / newDelegateW) * (oldDelegateH / newDelegateH))
                    : 0.985
            const scale = root.clampedUnitScale(1.0 + (rawScale - 1.0) * 0.34)
            const startOpacity = wasNearViewport ? (largeTravel ? 0.56 : 1.0) : 0.0
            if (Math.abs(easedDx) > 0.25 || Math.abs(easedDy) > 0.25
                    || Math.abs(scale - 1.0) > 0.01
                    || startOpacity < 1.0) {
                child.startZoomLayoutAnim(easedDx, easedDy, scale, scale, startOpacity)
                animCount++
            }
        }
        _zoomLayoutSnapshot = ({})
        if (animCount > 0) {
            zoomLayoutAnimDoneTimer.restart()
        } else {
            _zoomLayoutAnimating = false
        }
    }

    function finishActiveZoomLayoutAnimations() {
        zoomLayoutAnimDoneTimer.stop()
        if (!grid.contentItem) {
            _zoomLayoutAnimating = false
            return
        }
        const children = grid.contentItem.children
        for (let i = 0; i < children.length; i++) {
            if (children[i] && children[i].finishZoomLayoutAnim) {
                children[i].finishZoomLayoutAnim()
            }
        }
        _zoomLayoutAnimating = false
    }

    function updateCacheHint() {
        if (thumbnailBindingSuspended) {
            return
        }
        if (grid.width <= 0 || grid.cellWidth <= 0 || grid.cellHeight <= 0) {
            return
        }
        const cols = effectiveColumnCount()
        const rows = Math.max(1, Math.ceil(grid.height / grid.cellHeight))
        appModules.library.SetThumbnailCacheHint(cols * rows, root.desiredMaxEdge)
    }

    function maybeLoadMoreThumbnails() {
        if (thumbnailBindingSuspended
                || !appModules.library.thumbnailModel.hasMore
                || appModules.library.thumbnailModel.loading
                || grid.cellHeight <= 0) {
            return
        }
        const threshold = Math.max(grid.cellHeight * 3, grid.height * 0.5)
        const loadedMaxY = maxContentYForHeight(loadedContentHeight(), grid.originY)
        if (grid.contentY >= loadedMaxY - threshold) {
            appModules.library.LoadMoreThumbnails()
        }
    }

    Connections {
        target: appModules.library.thumbnailModel
        function onLoadingChanged() {
            if (!appModules.library.thumbnailModel.loading) {
                loadMoreThumbnailTimer.restart()
            }
        }
    }

    Timer {
        id: relayoutAndClampTimer
        interval: 0
        repeat: false
        onTriggered: root.finishRelayoutAndClamp()
    }

    Timer {
        id: zoomToCursorTimer
        interval: 0
        repeat: false
        onTriggered: root.finishZoomToCursor()
    }

    Timer {
        id: zoomLayoutApplyTimer
        interval: 0
        repeat: false
        onTriggered: root.applyZoomLayoutAnimation()
    }

    Timer {
        id: zoomCommitTimer
        interval: root.zoomSettleDelay
        repeat: false
        onTriggered: root.finishZoomCommit()
    }

    Timer {
        id: resumeThumbnailBindingTimer
        interval: root.thumbnailResumeDelay
        repeat: false
        onTriggered: root.resumeThumbnailBindings()
    }

    Timer {
        id: loadMoreThumbnailTimer
        interval: 0
        repeat: false
        onTriggered: root.maybeLoadMoreThumbnails()
    }

    Timer {
        id: zoomLayoutAnimDoneTimer
        interval: 650
        repeat: false
        onTriggered: root.finishActiveZoomLayoutAnimations()
    }

    function keyForElement(elementId) {
        return String(Number(elementId))
    }

    function isImageSelected(elementId) {
        return Object.prototype.hasOwnProperty.call(
            selectedImagesById, keyForElement(elementId))
    }

    function isImageQueued(elementId) {
        return Object.prototype.hasOwnProperty.call(
            exportQueueById, keyForElement(elementId))
    }

    function hasMultiSelectModifier(modifiers) {
        return (modifiers & Qt.ShiftModifier) || (modifiers & Qt.ControlModifier)
    }

    function selectionItemForIndex(index) {
        const row = appModules.library.thumbnailModel.getItemAt(index)
        if (!row || !row.elementId) {
            return null
        }
        const elementId = Number(row.elementId)
        if (elementId <= 0) {
            return null
        }

        return {
            elementId: elementId,
            fileId: Number(row.fileId || row.elementId),
            imageId: Number(row.imageId),
            folderId: Number(row.folderId || 0),
            scopeType: row.scopeType ? String(row.scopeType) : "",
            fileName: row.fileName ? row.fileName : qsTr("(unnamed)"),
            rating: Number(row.rating),
            isHdr: row.isHdr === true
        }
    }

    function selectionItemsForRange(firstIndex, lastIndex) {
        const rows = appModules.library.thumbnailModel.getItemsInRange(firstIndex, lastIndex)
        const items = []
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (!row || !row.elementId) {
                continue
            }
            const elementId = Number(row.elementId)
            if (elementId <= 0) {
                continue
            }
            items.push({
                elementId: elementId,
                fileId: Number(row.fileId || row.elementId),
                imageId: Number(row.imageId),
                folderId: Number(row.folderId || 0),
                scopeType: row.scopeType ? String(row.scopeType) : "",
                fileName: row.fileName ? row.fileName : qsTr("(unnamed)"),
                rating: Number(row.rating),
                isHdr: row.isHdr === true
            })
        }
        return items
    }

    function loadedIndexForElement(elementId) {
        return appModules.library.thumbnailModel.rowByElementId(Number(elementId))
    }

    function updateSelectionAnchor(index) {
        if (index >= 0) {
            selectionAnchorIndex = index
        }
    }

    function selectRangeToIndex(index, additive) {
        if (index < 0) {
            return
        }
        const anchor = selectionAnchorIndex >= 0 ? selectionAnchorIndex : index
        appModules.library.LoadThumbnailsThroughIndex(Math.max(anchor, index))
        const rangeItems = selectionItemsForRange(anchor, index)
        if (additive) {
            root.replaceSelection(Object.values(selectedImagesById).concat(rangeItems))
        } else {
            root.replaceSelection(rangeItems)
        }
        updateSelectionAnchor(index)
    }

    GridView {
        id: grid
        z: 0
        anchors.fill: parent
        model: appModules.library.thumbnailModel
        clip: true
        cacheBuffer: 0
        boundsBehavior: Flickable.StopAtBounds
        cellWidth: Math.max(72, Math.floor(width / root.columns))
        cellHeight: Math.max(64, Math.round((cellWidth - root.cardInset * 2) * 2 / 3
                                            + root.textAreaHeight
                                            + root.cardInset * 2
                                            + root.delegateGap))
        interactive: false
        onContentYChanged: root.maybeLoadMoreThumbnails()
        onContentHeightChanged: root.clampContentY()
        onHeightChanged: root.clampContentY()
        onCellWidthChanged: root.relayoutAndClamp()
        onCellHeightChanged: root.clampContentY()
        onCountChanged: {
            root.relayoutAndClamp()
            root.maybeLoadMoreThumbnails()
        }

        delegate: Rectangle {
            id: cardDelegate
            required property int index
            required property int elementId
            required property int imageId
        required property string fileName
        required property string cameraModel
        required property int iso
        required property string aperture
        required property string captureDate
        required property int rating
        required property bool isHdr
        required property string tags
        required property string accent
        required property string thumbUrl
        required property bool thumbLoading
        required property bool thumbMissingSource
        required property string thumbErrorText
        property string liveThumbUrl: thumbUrl
        onThumbUrlChanged: liveThumbUrl = thumbUrl
        property bool liveThumbLoading: thumbLoading
        onThumbLoadingChanged: liveThumbLoading = thumbLoading
        property bool liveThumbMissingSource: thumbMissingSource
        onThumbMissingSourceChanged: liveThumbMissingSource = thumbMissingSource
        property string liveThumbErrorText: thumbErrorText
        onThumbErrorTextChanged: liveThumbErrorText = thumbErrorText
        readonly property string displayTags: root.semanticTagsDisplayText(tags)
        property int pinnedElementId: 0
        property int pinnedImageId: 0
        property int pinnedMaxEdge: 0
        readonly property bool thumbnailReady: liveThumbUrl.length > 0
        readonly property bool thumbnailLoadingState: liveThumbLoading
        readonly property bool thumbnailMissingState: !thumbnailReady && !thumbnailLoadingState && liveThumbMissingSource
        readonly property bool thumbnailErrorState: !thumbnailReady && !thumbnailLoadingState
                                                    && liveThumbErrorText.length > 0
        readonly property bool thumbnailProblemState: thumbnailMissingState || thumbnailErrorState
        readonly property string thumbnailProblemText: liveThumbErrorText.length > 0
                                                       ? liveThumbErrorText
                                                       : qsTr("Source file was moved or deleted")
        readonly property bool thumbnailIdleState: !thumbnailReady && !thumbnailLoadingState
                                                   && !thumbnailProblemState

        function releasePinnedThumbnail() {
            if (pinnedElementId !== 0 && pinnedImageId !== 0) {
                appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, false,
                                                 pinnedMaxEdge > 0 ? pinnedMaxEdge : root.desiredMaxEdge)
            }
            pinnedElementId = 0
            pinnedImageId = 0
            pinnedMaxEdge = 0
        }

        function bindThumbnailLifetime(force) {
            liveThumbUrl = thumbUrl
            liveThumbLoading = thumbLoading
            liveThumbMissingSource = thumbMissingSource
            liveThumbErrorText = thumbErrorText

            if (root.thumbnailBindingSuspended && !force) {
                return
            }

            if (pinnedElementId === elementId && pinnedImageId === imageId
                    && pinnedMaxEdge === root.desiredMaxEdge) {
                return
            }

            if (pinnedElementId === elementId && pinnedImageId === imageId
                    && pinnedMaxEdge !== root.desiredMaxEdge) {
                const oldMaxEdge = pinnedMaxEdge
                pinnedMaxEdge = root.desiredMaxEdge
                if (pinnedElementId !== 0 && pinnedImageId !== 0) {
                    appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, true,
                                                     pinnedMaxEdge)
                    appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, false,
                                                     oldMaxEdge)
                }
                return
            }

            releasePinnedThumbnail()
            pinnedElementId = elementId
            pinnedImageId = imageId
            pinnedMaxEdge = root.desiredMaxEdge
            liveThumbUrl = thumbUrl
            liveThumbLoading = thumbLoading
            liveThumbMissingSource = thumbMissingSource
            liveThumbErrorText = thumbErrorText
            if (pinnedElementId !== 0 && pinnedImageId !== 0) {
                appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, true, pinnedMaxEdge)
            }
        }

        function syncThumbnailBinding() {
            bindThumbnailLifetime(true)
        }

        function releaseThumbnailBinding() {
            if (root.thumbnailBindingSuspended) {
                root.deferThumbnailRelease(pinnedElementId, pinnedImageId,
                                           pinnedMaxEdge > 0 ? pinnedMaxEdge : root.desiredMaxEdge)
                pinnedElementId = 0
                pinnedImageId = 0
                pinnedMaxEdge = 0
                return
            }
            releasePinnedThumbnail()
        }

        Component.onCompleted: bindThumbnailLifetime(false)
        onElementIdChanged: bindThumbnailLifetime(false)
        onImageIdChanged: bindThumbnailLifetime(false)
        Component.onDestruction: releaseThumbnailBinding()

        Connections {
            target: root
            function onDesiredMaxEdgeChanged() {
                if (!root.thumbnailBindingSuspended) {
                    cardDelegate.syncThumbnailBinding()
                }
            }
        }

            readonly property bool isSelected: root.isImageSelected(elementId)
            readonly property bool isHovered: overlay.hoveredIndex === index

        width: grid.cellWidth - root.delegateGap
        height: grid.cellHeight - root.delegateGap
        radius: root.columns >= 11 ? 6 : appTheme.panelRadius
            color: isSelected ? root.cardBgSelected
                   : (isHovered ? root.cardBgHover : root.cardBg)
            border.width: isSelected ? 2 : 0
            border.color: root.cardAccent
        Behavior on color { ColorAnimation { duration: 120 } }
            Behavior on border.width { NumberAnimation { duration: 150 } }
            Behavior on x {
                enabled: !root._zoomLayoutAnimating
                SpringAnimation { spring: 2.5; damping: 0.42; epsilon: 0.25 }
            }
            Behavior on y {
                enabled: !root._zoomLayoutAnimating
                SpringAnimation { spring: 2.5; damping: 0.42; epsilon: 0.25 }
            }
            opacity: zoomOpacity

            transform: [
                Scale {
                    id: zoomScale
                    origin.x: cardDelegate.width * 0.5
                    origin.y: cardDelegate.height * 0.5
                    xScale: cardDelegate.zoomScaleX
                    yScale: cardDelegate.zoomScaleY
                },
                Translate {
                    id: zoomTranslate
                    x: cardDelegate.zoomTranslateX
                    y: cardDelegate.zoomTranslateY
                }
            ]
            property real zoomTranslateX: 0
            property real zoomTranslateY: 0
            property real zoomScaleX: 1
            property real zoomScaleY: 1
            property real zoomOpacity: 1

            ParallelAnimation {
                id: zoomLayoutAnim
                NumberAnimation {
                    target: cardDelegate
                    property: "zoomTranslateX"
                    to: 0
                    duration: root.delegateReflowDuration
                    easing.type: Easing.OutQuint
                }
                NumberAnimation {
                    target: cardDelegate
                    property: "zoomTranslateY"
                    to: 0
                    duration: root.delegateReflowDuration
                    easing.type: Easing.OutQuint
                }
                NumberAnimation {
                    target: cardDelegate
                    property: "zoomScaleX"
                    to: 1
                    duration: root.delegateReflowDuration
                    easing.type: Easing.OutQuint
                }
                NumberAnimation {
                    target: cardDelegate
                    property: "zoomScaleY"
                    to: 1
                    duration: root.delegateReflowDuration
                    easing.type: Easing.OutQuint
                }
                NumberAnimation {
                    target: cardDelegate
                    property: "zoomOpacity"
                    to: 1
                    duration: Math.round(root.delegateReflowDuration * 0.72)
                    easing.type: Easing.OutCubic
                }
            }

            function startZoomLayoutAnim(fromX, fromY, fromScaleX, fromScaleY, fromOpacity) {
                zoomLayoutAnim.stop()
                zoomTranslateX = fromX
                zoomTranslateY = fromY
                zoomScaleX = fromScaleX
                zoomScaleY = fromScaleY
                zoomOpacity = fromOpacity
                zoomLayoutAnim.restart()
            }

            function finishZoomLayoutAnim() {
                zoomLayoutAnim.stop()
                zoomTranslateX = 0
                zoomTranslateY = 0
                zoomScaleX = 1
                zoomScaleY = 1
                zoomOpacity = 1
            }

        Connections {
            target: appModules.library
            ignoreUnknownSignals: true
            function onThumbnailUpdated(updatedElementId, updatedUrl, loading, missingSource, errorText) {
                if (updatedElementId === elementId) {
                    liveThumbUrl = updatedUrl
                    liveThumbLoading = loading
                    liveThumbMissingSource = missingSource
                    liveThumbErrorText = errorText
                }
            }
        }

            Item {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: root.cardInset
                height: Math.max(48, parent.height - root.textAreaHeight - root.cardInset * 2
                                 - (root.textAreaHeight > 0 ? 4 : 0))
                clip: true

                Rectangle {
                    anchors.fill: parent
                    radius: root.columns >= 11 ? 5 : 10
                    color: appTheme.bgBaseColor
                    border.width: root.columns >= 11 ? 1 : 2
                    border.color: appTheme.dividerColor
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    width: 28
                    height: 28
                    visible: thumbnailLoadingState
                    running: visible
                }
                Image {
                    id: thumbImage
                    anchors.centerIn: parent
                    width: parent.width - 4
                    height: parent.height - 4
                    source: liveThumbUrl
                    visible: false
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                }
                Rectangle {
                    id: thumbMask
                    anchors.fill: thumbImage
                    radius: root.columns >= 11 ? 4 : 8
                    visible: false
                    layer.enabled: true
                }
                MultiEffect {
                    anchors.fill: thumbImage
                    source: thumbImage
                    maskEnabled: true
                    maskSource: thumbMask
                    visible: thumbnailReady
                }
                Rectangle {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 8
                    width: 18
                    height: 18
                    radius: 9
                    visible: thumbnailProblemState
                    color: root.cardDangerTint
                    border.width: 1
                    border.color: root.cardDanger
                }
                Column {
                    anchors.centerIn: parent
                    width: parent.width - 12
                    visible: thumbnailProblemState
                    spacing: 2
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "!"
                        color: root.cardDanger
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: root.columns >= 11 ? 24 : 30
                        font.weight: 700
                    }
                    Label {
                        width: parent.width
                        text: thumbnailProblemText
                        color: root.cardDanger
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: root.columns >= 11 ? 8 : 10
                        font.weight: root.dataFontWeight
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }
                HoverHandler {
                    id: thumbHover
                }
                ToolTip.visible: thumbnailProblemState && thumbHover.hovered
                ToolTip.text: thumbnailProblemText
                ToolTip.delay: 150
        }

        Column {
            id: textColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: root.cardInset
            height: root.textAreaHeight
            visible: root.textAreaHeight > 0
            spacing: root.compactText ? 0 : 2
            Label {
                visible: !root.hideAllText
                text: fileName
                color: root.cardText
                font.family: appTheme.dataFontFamily
                font.pixelSize: root.titleFontSize
                font.weight: root.dataFontWeight
                font.letterSpacing: root.dataLetterSpacing
                elide: Text.ElideRight
                width: parent.width
                height: root.compactText ? root.textAreaHeight : implicitHeight
                verticalAlignment: Text.AlignVCenter
            }
            Label {
                visible: !root.hideMetadata
                text: qsTr("%1 | ISO %2 | f/%3").arg(cameraModel).arg(iso).arg(aperture)
                color: root.cardMuted
                font.family: appTheme.dataFontFamily
                font.pixelSize: root.metadataFontSize
                font.weight: root.dataFontWeight
                font.letterSpacing: root.dataLetterSpacing
                elide: Text.ElideRight
                width: parent.width
            }
            Row {
                visible: !root.hideMetadata
                width: parent.width
                height: Math.max(ratingLabel.implicitHeight, hdrGridTag.height)
                spacing: 5

                Label {
                    id: ratingLabel
                    text: displayTags.length > 0
                          ? qsTr("%1 | Rating %2/5 | %3").arg(captureDate).arg(rating).arg(displayTags)
                          : qsTr("%1 | Rating %2/5").arg(captureDate).arg(rating)
                    color: root.cardMuted
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: root.metadataFontSize
                    font.weight: root.dataFontWeight
                    font.letterSpacing: root.dataLetterSpacing
                    elide: Text.ElideRight
                    width: isHdr ? Math.max(0, parent.width - hdrGridTag.width - parent.spacing)
                                 : parent.width
                    anchors.bottom: parent.bottom
                }

                Rectangle {
                    id: hdrGridTag
                    visible: isHdr
                    width: hdrGridTagText.implicitWidth + 8
                    height: Math.max(13, hdrGridTagText.implicitHeight + 2)
                    radius: 3
                    color: "#3A3020"
                    border.width: 1
                    border.color: "#D8A93B"
                    anchors.bottom: parent.bottom
                    Label {
                        id: hdrGridTagText
                        anchors.centerIn: parent
                        text: qsTr("HDR")
                        color: "#F2C766"
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: Math.max(8, root.metadataFontSize - 1)
                        font.weight: Font.DemiBold
                    }
                }
            }
        }

        }
    }

    // ── Interaction overlay ──
    MouseArea {
        id: overlay
        z: 20
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        preventStealing: true

        property int hoveredIndex: -1
        property point dragStart: Qt.point(0, 0)
        property point dragCurrent: Qt.point(0, 0)
        property real dragStartContentY: 0
        property bool isDragging: false
        property var preDragSelection: ({})
        property bool dragAdditive: false

        cursorShape: hoveredIndex >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor

        function gridIndexAt(viewX, viewY) {
            return grid.indexAt(viewX + grid.contentX, viewY + grid.contentY)
        }

        function dragStartContentTop() {
            return dragStart.y + dragStartContentY
        }

        function dragCurrentContentTop() {
            return dragCurrent.y + grid.contentY
        }

        function rubberBandViewportY() {
            return Math.min(dragStartContentTop(), dragCurrentContentTop()) - grid.contentY
        }

        function rubberBandViewportHeight() {
            return Math.abs(dragCurrentContentTop() - dragStartContentTop())
        }

        function rubberBandIndexBounds() {
            if (grid.cellWidth <= 0 || grid.cellHeight <= 0) {
                return { first: -1, last: -1, minCol: 0, maxCol: -1, minRow: 0, maxRow: -1 }
            }

            const colCount = root.effectiveColumnCount()
            const totalCount = root.modelTotalCount()
            if (totalCount <= 0) {
                return { first: -1, last: -1, minCol: 0, maxCol: -1, minRow: 0, maxRow: -1 }
            }

            const bLeft = Math.max(0, Math.min(dragStart.x, dragCurrent.x))
            const bRight = Math.min(grid.width, Math.max(dragStart.x, dragCurrent.x))
            const bTop = Math.max(0, Math.min(dragStartContentTop(), dragCurrentContentTop()))
            const bBottom = Math.max(dragStartContentTop(), dragCurrentContentTop())
            if (bRight < 0 || bLeft > grid.width || bBottom < 0) {
                return { first: -1, last: -1, minCol: 0, maxCol: -1, minRow: 0, maxRow: -1 }
            }

            const minCol = Math.max(0, Math.min(colCount - 1, Math.floor(bLeft / grid.cellWidth)))
            const maxCol = Math.max(0, Math.min(colCount - 1, Math.floor(bRight / grid.cellWidth)))
            const minRow = Math.max(0, Math.floor(bTop / grid.cellHeight))
            const maxRow = Math.max(0, Math.floor(bBottom / grid.cellHeight))
            const first = minRow * colCount + minCol
            const last = Math.min(totalCount - 1, maxRow * colCount + maxCol)
            if (first > last) {
                return { first: -1, last: -1, minCol: 0, maxCol: -1, minRow: 0, maxRow: -1 }
            }
            return {
                first: first,
                last: last,
                minCol: minCol,
                maxCol: maxCol,
                minRow: minRow,
                maxRow: maxRow
            }
        }

        function applyRubberBandSelection() {
            const bandItems = collectRubberBandItems()
            if (dragAdditive) {
                const merged = Object.values(preDragSelection).concat(bandItems)
                root.replaceSelection(merged)
            } else {
                root.replaceSelection(bandItems)
            }
        }

        function beginRubberBandDrag(modifiers) {
            if (isDragging) {
                return
            }
            isDragging = true
            dragAdditive = root.hasMultiSelectModifier(modifiers)
            if (dragAdditive) {
                preDragSelection = Object.assign({}, root.selectedImagesById)
            }
            applyRubberBandSelection()
        }

        function collectRubberBandItems() {
            const bounds = rubberBandIndexBounds()
            if (bounds.last < 0) {
                return []
            }

            appModules.library.LoadThumbnailsThroughIndex(bounds.last)
            const colCount = root.effectiveColumnCount()
            const loadedCount = root.modelCount()

            const items = []
            for (let row = bounds.minRow; row <= bounds.maxRow; ++row) {
                for (let col = bounds.minCol; col <= bounds.maxCol; ++col) {
                    const idx = row * colCount + col
                    if (idx >= 0 && idx < loadedCount) {
                        const item = root.selectionItemForIndex(idx)
                        if (item) {
                            items.push(item)
                        }
                    }
                }
            }
            return items
        }

        onPositionChanged: function(mouse) {
            if (pressed && (pressedButtons & Qt.LeftButton)) {
                const dx = mouse.x - dragStart.x
                const dy = mouse.y - dragStart.y
                if (!isDragging && (dx * dx + dy * dy) > 64) {
                    beginRubberBandDrag(mouse.modifiers)
                }
                if (isDragging) {
                    dragCurrent = Qt.point(mouse.x, mouse.y)
                    applyRubberBandSelection()
                }
            }
            hoveredIndex = gridIndexAt(mouse.x, mouse.y)
        }

        onPressed: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                const idx = gridIndexAt(mouse.x, mouse.y)
                if (idx >= 0) {
                    const item = root.selectionItemForIndex(idx)
                    if (item) {
                        root.imageFocused(item)
                        const scenePoint = overlay.mapToItem(null, mouse.x, mouse.y)
                        root.contextMenuRequested(item, scenePoint.x, scenePoint.y)
                    }
                }
                return
            }
            dragStart = Qt.point(mouse.x, mouse.y)
            dragCurrent = Qt.point(mouse.x, mouse.y)
            dragStartContentY = grid.contentY
            isDragging = false
            dragAdditive = false
            preDragSelection = ({})
            mouse.accepted = true
        }

        onReleased: function(mouse) {
            if (mouse.button !== Qt.LeftButton) {
                isDragging = false
                return
            }
            if (!isDragging) {
                const idx = gridIndexAt(mouse.x, mouse.y)
                if (idx >= 0) {
                    const item = root.selectionItemForIndex(idx)
                    if (item) {
                        root.imageFocused(item)
                        if (mouse.modifiers & Qt.ShiftModifier) {
                            root.selectRangeToIndex(idx, mouse.modifiers & Qt.ControlModifier)
                        } else if (mouse.modifiers & Qt.ControlModifier) {
                            const next = !root.isImageSelected(item.elementId)
                            root.imageSelectionChanged(item.elementId, item.imageId, item.fileName,
                                                       item.isHdr === true, next)
                            root.updateSelectionAnchor(idx)
                        } else {
                            root.replaceSelection([item])
                            root.updateSelectionAnchor(idx)
                        }
                    }
                } else {
                    root.replaceSelection([])
                    root.selectionAnchorIndex = -1
                }
            }
            isDragging = false
        }

        onDoubleClicked: function(mouse) {
            const idx = gridIndexAt(mouse.x, mouse.y)
            if (idx >= 0) {
                const item = root.selectionItemForIndex(idx)
                if (item) {
                    root.imageFocused(item)
                    appModules.workspaceRouter.openEditor(item.elementId, item.imageId)
                }
            }
        }

        onExited: hoveredIndex = -1

        onWheel: function(wheel) {
            // Ctrl+Scroll → zoom, otherwise → scroll
            if (wheel.modifiers & Qt.ControlModifier) {
                const delta = wheel.angleDelta.y > 0 ? 1 : -1
                root.setZoomLevelAt(root.zoomLevel + delta, wheel.y)
                wheel.accepted = true
                return
            }

            const wheelDelta = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y : wheel.angleDelta.y
            root.scrollBy(-wheelDelta)
            root.maybeLoadMoreThumbnails()
            if (isDragging) {
                applyRubberBandSelection()
            }
            hoveredIndex = gridIndexAt(wheel.x, wheel.y)
            wheel.accepted = true
        }
    }

    // ── Rubber band visual ──
    Rectangle {
        id: rubberBand
        z: 30
        visible: overlay.isDragging
        x: Math.min(overlay.dragStart.x, overlay.dragCurrent.x)
        y: overlay.rubberBandViewportY()
        width: Math.abs(overlay.dragCurrent.x - overlay.dragStart.x)
        height: overlay.rubberBandViewportHeight()
        color: Qt.rgba(appTheme.toneMist.r, appTheme.toneMist.g, appTheme.toneMist.b, 0.08)
        border.width: 1
        border.color: Qt.rgba(appTheme.toneMist.r, appTheme.toneMist.g, appTheme.toneMist.b, 0.50)
        radius: 2
    }
}
