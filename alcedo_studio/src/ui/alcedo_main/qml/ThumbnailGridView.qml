import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import QtQuick.Effects
import Alcedo.Main 1.0

// Flat (ungrouped) Library photo area. Two layouts show the loaded photos of
// LibraryModule.thumbnailModel; LibraryViewPreferences.gridLayout selects one:
//
// - "classic": a GridView of 3:2 cards with a fixed column count per zoom level.
// - "justified": rows of one photo height that fill the view width (JustifiedLayoutModel and
//   JustifiedThumbnailTile). The tile width follows the aspect ratio of the photo.
//
// Both layouts use one input overlay (selection, rubber band, stars, context menu, wheel), one
// scroll bar, and the same geometry functions (layoutRect, cardIndexAt, delegateForIndex, ...).
//
// Zoom: a new zoom level is applied in one synchronous step (applyZoomTransition). Before the
// layout changes, each shown card records its drawn rectangle; after the new layout and the new
// scroll position are set, each card animates from that rectangle to its new one, and the card
// under the pointer (Ctrl+wheel) or at the view centre keeps its height on the screen. A zoom
// step during an animation starts from the drawn rectangles of that moment, so the cards do not
// jump to the end of the running animation.
Item {
    id: root
    objectName: "thumbnailGridView"
    clip: true
    readonly property color cardBg: appTheme.cardSurfaceColor
    readonly property color cardBgHover: appTheme.hoverColor
    readonly property color cardMuted: appTheme.textMutedColor
    readonly property color cardText: appTheme.textColor
    // Surface behind the justified tiles (the Library browser panel); the ring of the color
    // label dots in their caption.
    readonly property color justifiedCanvasColor: appTheme.glassPanelColor
    readonly property color cardDanger: appTheme.dangerColor
    readonly property color cardDangerTint: appTheme.dangerTintColor
    readonly property int dataFontWeight: 500
    readonly property real dataLetterSpacing: -0.2

    // The library selection (LibrarySelection). A delegate asks it for its own photo.
    readonly property var selection: appModules.library.selection
    property var exportQueueById: ({})

    // ── Layout ──
    // "classic" or "justified" (LibraryViewPreferences.gridLayout).
    property string layoutMode: appModules.libraryView ? appModules.libraryView.gridLayout
                                                       : "classic"
    // The layout that the view shows; it follows layoutMode in onLayoutModeChanged, which
    // keeps the first shown photo at the top.
    property bool _layoutJustified: false
    readonly property bool justified: _layoutJustified
    // The scrolled view of the shown layout: the GridView, or the justified-rows item with the
    // same contentX, contentY, originY, and contentHeight properties.
    readonly property Item view: _layoutJustified ? justifiedView : grid
    readonly property Item inputOverlay: overlay

    // ── Zoom control ──
    // zoomLevel maps to column counts: 2, 3, 4, 5, 6, 8, 11, 14
    readonly property var zoomColumns: [2, 3, 4, 5, 6, 8, 11, 14]
    // resolution tiers per zoom level (max edge px): 2048, 1024, 1024, 512, 512, 256, 256, 256
    readonly property var zoomResolutionEdges: [2048, 1024, 1024, 512, 512, 256, 256, 256]
    // Target photo height of a justified row per zoom level.
    readonly property var justifiedRowHeights: [480, 360, 280, 220, 180, 140, 104, 80]
    readonly property int zoomLevelCount: zoomColumns.length
    property int zoomLevel: 4  // default: 6 columns
    property bool zoomAdjusting: false
    property bool _zoomReady: false
    property int committedZoomLevel: Math.max(0, Math.min(zoomLevelCount - 1, zoomLevel))
    property bool thumbnailBindingSuspended: false
    property var _deferredThumbnailReleases: ({})
    property int selectionAnchorIndex: -1
    // Zoom level of the shown layout. applyZoomTransition sets it after the shown cards are
    // recorded, so every size below changes together with the layout.
    property int _layoutZoomLevel: 4

    readonly property int columns: zoomColumns[Math.min(_layoutZoomLevel, zoomLevelCount - 1)]
    readonly property int desiredMaxEdge: zoomResolutionEdges[Math.min(committedZoomLevel, zoomLevelCount - 1)]
    readonly property bool compactText: columns >= 8
    readonly property bool hideMetadata: columns >= 8
    readonly property bool hideAllText: columns >= 14
    // 2 to 6 columns: the card stars are a control. At 8 and 11 columns a star is too small to
    // hit, so the stars are display only and a click on them selects the card.
    readonly property bool starsInteractive: columns <= 6
    readonly property int cardInset: cardInsetForColumns(columns)
    readonly property int delegateGap: delegateGapForColumns(columns)
    readonly property int textAreaHeight: textAreaHeightForColumns(columns)
    readonly property int titleFontSize: columns >= 11 ? 9 : (columns >= 8 ? 10 : 12)
    readonly property int metadataFontSize: columns >= 8 ? 8 : 10
    // Justified rows: the target photo height, the file name line below the photo, the gap
    // between tiles and rows, and the hover details (all levels with a file name).
    readonly property real justifiedRowHeight: justifiedRowHeights[Math.min(_layoutZoomLevel,
                                                                            zoomLevelCount - 1)]
    readonly property real justifiedCaptionHeight: hideAllText ? 0 : (compactText ? 16 : 22)
    readonly property real justifiedSpacing: cardInset
    readonly property bool justifiedDetailsEnabled: !hideAllText
    // Pointer targets of the input overlay, read by the delegates.
    readonly property int hoveredCardIndex: overlay.hoveredIndex
    readonly property int hoveredCardStar: overlay.hoveredStar
    readonly property int hoveredEditMarkIndex: overlay.hoveredEditMarkIndex

    property bool _zoomLayoutAnimating: false
    // Focus point of the next zoom step in viewport coordinates; -1 is the view centre.
    property real _zoomFocusX: -1
    property real _zoomFocusY: -1
    // Ctrl+wheel angle not yet used for a zoom step (a touchpad sends small steps).
    property real _zoomWheelAngle: 0
    property int _layoutRequestId: 0
    property int _pendingLayoutRequestId: 0
    readonly property int zoomSettleDelay: 90
    readonly property int thumbnailResumeDelay: 150
    readonly property int delegateReflowDuration: appTheme.reduceMotion ? 0 : 280
    // In-place change of the shown photos (an import or a delete): the cards before the change,
    // read on libraryChangeAboutToApply. While a change animates, the cards slide from their old
    // viewport positions.
    property var _changeCapture: null
    property bool _changeLayoutAnimating: false
    // True while the view width follows the collections sidebar fold. The thumbnail cache size
    // waits for the final size.
    property bool geometryAnimating: false
    // Calls of LibraryModule::SetThumbnailCacheHint; read by tests.
    property int cacheHintUpdateCount: 0
    // When more cards than this appear or disappear in the view, the change is shown without
    // animation; the scroll anchor stays.
    readonly property int changeAnimationLimit: 48

    signal imageSelectionChanged(int elementId, int imageId, string fileName, bool isHdr,
                                 bool selected)
    signal replaceSelection(var items)
    // A star click on the card of @p item (2 to 6 columns): rate that photo only.
    signal ratingRequested(var item, int rating)
    signal imageFocused(var item, int index)
    signal contextMenuRequested(var item, real sceneX, real sceneY)
    signal zoomChanged(int zoomLevel)
    // Ctrl+wheel asks the owner for a zoom level, so an owner binding of zoomLevel stays. The
    // view sets zoomLevel itself only when the owner did not.
    signal zoomLevelRequested(int zoomLevel)

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
        _layoutZoomLevel = committedZoomLevel
        _layoutJustified = layoutMode === "justified"
        _zoomReady = true
        root.applyJustifiedGeometry()
        root.relayoutAndClamp()
        root.updateCacheHint()
    }

    onZoomLevelChanged: {
        const clamped = root.clampedZoomLevel(zoomLevel)
        if (!_zoomReady) {
            committedZoomLevel = clamped
            _layoutZoomLevel = clamped
            return
        }
        root.zoomChanged(clamped)
        const focusX = _zoomFocusX >= 0 ? _zoomFocusX : root.view.width * 0.5
        const focusY = _zoomFocusY >= 0 ? _zoomFocusY : root.view.height * 0.5
        _zoomFocusX = -1
        _zoomFocusY = -1
        if (clamped === _layoutZoomLevel) {
            return
        }
        root.beginThumbnailBindingSuspension()
        root.applyZoomTransition(clamped, focusX, focusY)
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
    onLayoutModeChanged: root.applyLayoutMode()
    onWidthChanged: {
        if (_layoutJustified) {
            root.applyJustifiedGeometry()
        } else {
            root.relayoutAndClamp()
        }
        root.updateCacheHint()
    }
    onHeightChanged: {
        root.clampContentY()
        root.updateCacheHint()
    }
    // The fold ends: one relayout and one cache size for the final size, at once. A relayout
    // queued by the last frame is cancelled, so the cache size is set exactly once and before
    // the next event.
    onGeometryAnimatingChanged: {
        if (geometryAnimating) {
            return
        }
        relayoutAndClampTimer.stop()
        if (!_layoutJustified) {
            grid.forceLayout()
        }
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

    // Caption height of a classic card: the file name, camera, and date lines (2 to 6 columns),
    // one line (8 and 11 columns), or none (14 columns).
    function textAreaHeightForColumns(columnCount) {
        if (columnCount >= 14) {
            return 0
        }
        return columnCount >= 8 ? 18 : 50
    }

    // Classic cell geometry of a zoom level for the current view width. The GridView cells and
    // the old rectangles of a zoom animation both read it.
    function metricsForZoomLevel(level) {
        const visualCols = root.visualColumnsForZoomLevel(level)
        const inset = root.cardInsetForColumns(visualCols)
        const gap = root.delegateGapForColumns(visualCols)
        const textHeight = root.textAreaHeightForColumns(visualCols)
        const cellW = Math.max(72, Math.floor(root.width / visualCols))
        const cellH = Math.max(64, Math.round((cellW - inset * 2) * 2 / 3
                                              + textHeight
                                              + inset * 2
                                              + gap))
        return {
            visualCols: visualCols,
            cols: Math.max(1, Math.floor(root.width / cellW)),
            cellW: cellW,
            cellH: cellH,
            delegateGap: gap
        }
    }
    readonly property var classicMetrics: metricsForZoomLevel(_layoutZoomLevel)

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
        const cards = root.photoDelegates()
        for (let i = 0; i < cards.length; ++i) {
            cards[i].syncThumbnailBinding()
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

    // ── Geometry of the shown layout ──
    // Layout coordinates: x from the left of the content, y from the top of the first row
    // (GridView puts that row at originY; the justified view at 0).

    // Rectangle of the card of photo @p index in layout coordinates, or null when the layout has
    // no card for it (a justified photo that is not loaded).
    function layoutRect(index) {
        if (index < 0) {
            return null
        }
        if (_layoutJustified) {
            const r = justifiedModel.tileRect(index)
            return r.width > 0 ? { x: r.x, y: r.y, w: r.width, h: r.height } : null
        }
        return root.classicRectForMetrics(index, {
            cols: root.effectiveColumnCount(), cellW: grid.cellWidth, cellH: grid.cellHeight,
            delegateGap: root.delegateGap
        })
    }

    function classicRectForMetrics(index, metrics) {
        const cols = Math.max(1, metrics.cols)
        return {
            x: (index % cols) * metrics.cellW,
            y: Math.floor(index / cols) * metrics.cellH,
            w: Math.max(1, metrics.cellW - metrics.delegateGap),
            h: Math.max(1, metrics.cellH - metrics.delegateGap)
        }
    }

    function layoutReady() {
        if (root.view.width <= 0 || root.view.height <= 0) {
            return false
        }
        return _layoutJustified ? justifiedModel.layoutRowCount > 0 : grid.cellHeight > 0
    }

    // Photo whose card holds the viewport point; -1 in a gap.
    function cardIndexAt(viewX, viewY) {
        if (_layoutJustified) {
            return justifiedModel.indexAt(viewX + justifiedView.contentX,
                                          viewY + justifiedView.contentY)
        }
        return grid.indexAt(viewX + grid.contentX, viewY + grid.contentY)
    }

    // Photo whose card is nearest to the viewport point; -1 without photos.
    function nearestCardIndexAt(viewX, viewY) {
        if (_layoutJustified) {
            return justifiedModel.nearestIndex(viewX + justifiedView.contentX,
                                               viewY + justifiedView.contentY)
        }
        const total = root.modelTotalCount()
        if (total <= 0 || grid.cellWidth <= 0 || grid.cellHeight <= 0) {
            return -1
        }
        const cols = root.effectiveColumnCount()
        const col = Math.max(0, Math.min(cols - 1,
                                         Math.floor((viewX + grid.contentX) / grid.cellWidth)))
        const row = Math.max(0, Math.floor((viewY + grid.contentY - grid.originY) / grid.cellHeight))
        return Math.min(total - 1, row * cols + col)
    }

    function delegateForIndex(index) {
        if (index < 0) {
            return null
        }
        if (_layoutJustified) {
            const row = index - justifiedModel.firstIndex
            return row >= 0 && row < justifiedRepeater.count ? justifiedRepeater.itemAt(row) : null
        }
        return grid.itemAtIndex(index)
    }

    // The created photo delegates of the shown layout.
    function photoDelegates() {
        const cards = []
        if (_layoutJustified) {
            for (let i = 0; i < justifiedRepeater.count; ++i) {
                const tile = justifiedRepeater.itemAt(i)
                if (tile) {
                    cards.push(tile)
                }
            }
            return cards
        }
        if (!grid.contentItem) {
            return cards
        }
        const children = grid.contentItem.children
        for (let i = 0; i < children.length; ++i) {
            const child = children[i]
            if (child && child.cardIndex !== undefined && child.startZoomLayoutAnim
                    && child.visible) {
                cards.push(child)
            }
        }
        return cards
    }

    // Drawn rectangle of a card in viewport coordinates, with its running animation.
    function drawnViewportRect(card) {
        return {
            x: card.x + card.zoomTranslateX - root.view.contentX,
            y: card.y + card.zoomTranslateY - root.view.contentY,
            w: card.width * card.zoomScaleX,
            h: card.height * card.zoomScaleY
        }
    }

    function rowPitch() {
        return _layoutJustified ? justifiedRowHeight + justifiedCaptionHeight + justifiedSpacing
                                : grid.cellHeight
    }

    function rowTopOfIndex(index) {
        if (_layoutJustified) {
            return justifiedModel.rowTopOfIndex(index)
        }
        return Math.floor(index / root.effectiveColumnCount()) * grid.cellHeight
    }

    // Photos of the rows that the viewport shows (the thumbnail cache size).
    function visiblePhotoCount() {
        if (_layoutJustified) {
            const range = justifiedModel.indexRangeForY(justifiedView.contentY,
                                                        justifiedView.contentY + justifiedView.height)
            return Math.max(1, range.last - range.first + 1)
        }
        const rows = Math.max(1, Math.ceil(grid.height / grid.cellHeight))
        return root.effectiveColumnCount() * rows
    }

    // Exposed for library view-state restore across workspace Loader teardown.
    readonly property real contentX: view.contentX
    readonly property real contentY: view.contentY
    function restoreContentY(y) {
        if (y === undefined || y === null) {
            return
        }
        root.view.contentY = clampYForHeight(layoutContentHeight(), Number(y))
    }

    // Put the requested image at the first visible Library row. The content height is
    // estimated from the complete model count, so this stays correct while thumbnail pages are
    // still being appended. A justified photo that is not loaded yet loads its page first;
    // the caller asks again when the page arrives.
    function scrollToIndexAtTop(index) {
        const total = root.modelTotalCount()
        if (total <= 0 || root.view.width <= 0 || root.view.height <= 0) {
            return false
        }
        const clampedIndex = Math.max(0, Math.min(total - 1, Number(index)))
        if (_layoutJustified) {
            if (clampedIndex >= root.modelCount()) {
                appModules.library.LoadThumbnailsThroughIndex(clampedIndex)
                return false
            }
        } else {
            grid.forceLayout()
            if (grid.cellHeight <= 0) {
                return false
            }
        }
        const targetY = root.view.originY + root.rowTopOfIndex(clampedIndex)
        root.view.contentY = root.clampYForHeight(root.layoutContentHeight(), targetY)
        root.maybeLoadMoreThumbnails()
        return true
    }

    function scrollToElementAtTop(elementId) {
        if (!appModules.library || !appModules.library.thumbnailModel) {
            return false
        }
        const index = appModules.library.thumbnailModel.rowByElementId(Number(elementId))
        return index >= 0 ? root.scrollToIndexAtTop(index) : false
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
        if (_layoutJustified) {
            return justifiedModel.contentHeight
        }
        if (grid.cellHeight <= 0) {
            return 0
        }
        return Math.ceil(modelTotalCount() / effectiveColumnCount()) * grid.cellHeight
    }

    function loadedContentHeight() {
        if (_layoutJustified) {
            return justifiedModel.loadedHeight
        }
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
        return root.view.contentHeight
    }

    function maxContentY() {
        return maxContentYForHeight(layoutContentHeight(), root.view.originY)
    }

    function maxContentYForHeight(contentHeight, originY) {
        return originY + Math.max(0, contentHeight - root.view.height)
    }

    function clampYForHeight(contentHeight, contentY, originY) {
        const minY = originY === undefined ? root.view.originY : originY
        return Math.max(minY, Math.min(maxContentYForHeight(contentHeight, minY), contentY))
    }

    function clampContentY() {
        root.view.contentY = clampYForHeight(layoutContentHeight(), root.view.contentY)
    }

    function scrollBy(deltaY) {
        root.view.contentY = clampYForHeight(layoutContentHeight(), root.view.contentY + deltaY)
    }

    function relayoutAndClamp() {
        _pendingLayoutRequestId = ++_layoutRequestId
        relayoutAndClampTimer.restart()
    }

    function finishRelayoutAndClamp() {
        if (_pendingLayoutRequestId !== _layoutRequestId) {
            return
        }
        if (!_layoutJustified) {
            grid.forceLayout()
        }
        root.clampContentY()
        root.updateCacheHint()
    }

    // Justified rows for the current width and zoom level. The photo at the top of the view
    // keeps its place.
    function applyJustifiedGeometry() {
        if (!_layoutJustified) {
            return
        }
        const anchor = justifiedModel.layoutRowCount > 0 ? root.nearestCardIndexAt(0, 0) : -1
        let fraction = 0
        if (anchor >= 0) {
            const r = justifiedModel.tileRect(anchor)
            fraction = r.height > 0 ? (justifiedView.contentY - r.y) / r.height : 0
        }
        justifiedView.contentY = justifiedModel.setGeometryAnchored(
            root.width, root.justifiedRowHeight, root.justifiedSpacing,
            root.justifiedCaptionHeight, anchor, fraction, 0)
    }

    // Show the layout that layoutMode names. The first shown photo stays at the top.
    function applyLayoutMode() {
        const wantJustified = layoutMode === "justified"
        if (!_zoomReady) {
            _layoutJustified = wantJustified
            return
        }
        if (wantJustified === _layoutJustified) {
            return
        }
        const topIndex = root.layoutReady() ? root.nearestCardIndexAt(0, 0) : -1
        root.finishActiveZoomLayoutAnimations()
        root.finishChangeAnimations()
        overlay.hoveredIndex = -1
        _layoutJustified = wantJustified
        root.applyJustifiedGeometry()
        if (topIndex >= 0) {
            if (!_layoutJustified) {
                grid.forceLayout()
            }
            root.scrollToIndexAtTop(topIndex)
        } else {
            root.clampContentY()
        }
        root.updateCacheHint()
    }

    function setZoomLevelAt(nextZoomLevel, focusViewportY, focusViewportX) {
        const clamped = root.clampedZoomLevel(nextZoomLevel)
        if (clamped === zoomLevel) {
            return
        }
        _zoomFocusY = focusViewportY === undefined ? -1 : Math.max(0, focusViewportY)
        _zoomFocusX = focusViewportX === undefined ? -1 : Math.max(0, focusViewportX)
        root.zoomLevelRequested(clamped)
        if (zoomLevel !== clamped) {
            zoomLevel = clamped
        }
    }

    // The drawn viewport rectangle and the thumbnail of each created card, by element id.
    function captureShownCards() {
        const shown = {}
        const cards = root.photoDelegates()
        for (let i = 0; i < cards.length; ++i) {
            const card = cards[i]
            if (card.elementId <= 0) {
                continue
            }
            const r = root.drawnViewportRect(card)
            r.thumb = card.liveThumbUrl
            r.onScreen = r.y + r.h > 0 && r.y < root.view.height
                    && r.x + r.w > 0 && r.x < root.view.width
            shown[card.elementId] = r
        }
        return shown
    }

    // Apply zoom level @p target. The card nearest the viewport point (@p focusX, @p focusY)
    // keeps the point at the same fraction of its height under @p focusY.
    function applyZoomTransition(target, focusX, focusY) {
        if (target === _layoutZoomLevel) {
            return
        }
        if (!root.layoutReady()) {
            _layoutZoomLevel = target
            root.applyJustifiedGeometry()
            root.relayoutAndClamp()
            return
        }
        const before = root.captureShownCards()
        const oldMetrics = _layoutJustified ? null : root.classicMetrics
        const oldOriginY = root.view.originY
        const oldContentX = root.view.contentX
        const oldContentY = root.view.contentY
        const fy = Math.max(0, Math.min(root.view.height, focusY))
        const fx = Math.max(0, Math.min(root.view.width, focusX))
        const focusIndex = root.nearestCardIndexAt(fx, fy)
        let fraction = 0.5
        if (focusIndex >= 0) {
            const focusCard = root.delegateForIndex(focusIndex)
            const lr = root.layoutRect(focusIndex)
            const r = focusCard ? root.drawnViewportRect(focusCard)
                                : (lr ? { y: lr.y + oldOriginY - oldContentY, h: lr.h } : null)
            if (r && r.h > 0) {
                fraction = Math.max(0, Math.min(1, (fy - r.y) / r.h))
            }
        }

        root.finishActiveZoomLayoutAnimations()
        _zoomLayoutAnimating = true
        _layoutZoomLevel = target
        ++_layoutRequestId
        if (_layoutJustified) {
            justifiedView.contentY = justifiedModel.setGeometryAnchored(
                root.width, root.justifiedRowHeight, root.justifiedSpacing,
                root.justifiedCaptionHeight, focusIndex, fraction, fy)
        } else {
            grid.forceLayout()
            const nr = root.layoutRect(focusIndex)
            if (nr) {
                const targetY = grid.originY + nr.y + fraction * nr.h - fy
                grid.contentY = root.clampYForHeight(root.layoutContentHeight(), targetY)
            } else {
                root.clampContentY()
            }
            grid.forceLayout()
        }

        const seen = {}
        const cards = root.photoDelegates()
        for (let i = 0; i < cards.length; ++i) {
            const card = cards[i]
            const to = {
                x: card.x - root.view.contentX,
                y: card.y - root.view.contentY,
                w: Math.max(1, card.width),
                h: Math.max(1, card.height)
            }
            seen[card.elementId] = true
            let from = before[card.elementId]
            if (!from && oldMetrics) {
                const r = root.classicRectForMetrics(card.cardIndex, oldMetrics)
                from = { x: r.x - oldContentX, y: oldOriginY + r.y - oldContentY, w: r.w, h: r.h }
            }
            const toOnScreen = to.y + to.h > 0 && to.y < root.view.height
            if (!toOnScreen && (!from || !(from.y + from.h > 0 && from.y < root.view.height))) {
                continue
            }
            if (from) {
                card.startZoomLayoutAnim(from.x - to.x, from.y - to.y, from.w / to.w,
                                         from.h / to.h, 1)
            } else {
                card.startZoomLayoutAnim(0, 0, 1, 1, 0)
            }
        }

        // Cards that leave the view slide to their new rectangle as ghosts.
        const model = appModules.library.thumbnailModel
        for (const key in before) {
            const from = before[key]
            if (seen[key] || !from.onScreen) {
                continue
            }
            const r = root.layoutRect(model.rowByElementId(Number(key)))
            const to = r ? { x: r.x - root.view.contentX, y: r.y + root.view.originY - root.view.contentY,
                             w: r.w, h: r.h }
                         : from
            changeGhostComponent.createObject(changeGhostLayer, {
                x: from.x, y: from.y, width: from.w, height: from.h,
                toX: to.x, toY: to.y, toWidth: to.w, toHeight: to.h,
                source: from.thumb
            })
        }
        zoomLayoutAnimDoneTimer.restart()
        root.updateCacheHint()
    }

    function finishActiveZoomLayoutAnimations() {
        zoomLayoutAnimDoneTimer.stop()
        const cards = root.photoDelegates()
        for (let i = 0; i < cards.length; i++) {
            cards[i].finishZoomLayoutAnim()
        }
        for (let g = changeGhostLayer.children.length - 1; g >= 0; --g) {
            changeGhostLayer.children[g].destroy()
        }
        _zoomLayoutAnimating = false
    }

    function updateCacheHint() {
        if (thumbnailBindingSuspended || geometryAnimating || !root.layoutReady()) {
            return
        }
        root.cacheHintUpdateCount += 1
        appModules.library.SetThumbnailCacheHint(root.visiblePhotoCount(), root.desiredMaxEdge)
    }

    // ── Scroll bar ──
    // The view takes no flick input and its content height covers only the loaded rows, so the
    // scroll bar is not attached to it: its size and position come from the height of all photos
    // of the result (layoutContentHeight). A drag moves contentY there and loads the pages up to
    // the rows that it shows.
    function scrollFraction() {
        const height = root.layoutContentHeight()
        return height > 0 ? (root.view.contentY - root.view.originY) / height : 0
    }

    function scrollToFraction(fraction) {
        const height = root.layoutContentHeight()
        if (height <= 0 || !root.layoutReady()) {
            return
        }
        root.view.contentY = root.clampYForHeight(height, root.view.originY + fraction * height)
        let lastIndex = 0
        if (_layoutJustified) {
            lastIndex = justifiedModel.indexAtY(justifiedView.contentY + justifiedView.height
                                                + root.rowPitch())
        } else {
            const columnsForZoom = root.effectiveColumnCount()
            const topRow = Math.floor((grid.contentY - grid.originY) / grid.cellHeight)
            const shownRows = Math.ceil(grid.height / grid.cellHeight) + 1
            lastIndex = (topRow + shownRows) * columnsForZoom
        }
        lastIndex = Math.min(root.modelTotalCount() - 1, lastIndex)
        if (lastIndex >= root.modelCount()) {
            appModules.library.LoadThumbnailsThroughIndex(lastIndex)
        }
    }

    function maybeLoadMoreThumbnails() {
        if (thumbnailBindingSuspended
                || !appModules.library.thumbnailModel.hasMore
                || appModules.library.thumbnailModel.loading
                || root.rowPitch() <= 0) {
            return
        }
        const threshold = Math.max(root.rowPitch() * 3, root.view.height * 0.5)
        const loadedMaxY = maxContentYForHeight(loadedContentHeight(), root.view.originY)
        if (root.view.contentY >= loadedMaxY - threshold) {
            appModules.library.LoadMoreThumbnails()
        }
    }

    // A position read (RequestFocusPosition) loaded the page of the file; in the flat view the
    // occurrence is the grid index.
    Connections {
        target: appModules.library
        function onFocusPositionReady(fileId, occurrence, sectionRow) {
            if (occurrence >= 0) {
                root.scrollToIndexAtTop(occurrence)
            }
        }
        function onLibraryChangeAboutToApply() { root.captureChange() }
        function onLibraryChangeApplied() { root.applyChange() }
    }

    // ── In-place change (import or delete) ──
    // The first visible card keeps its position on the screen: before the change each created
    // card is recorded with its viewport position; after the change the row of the first
    // recorded card that is still shown is put at the same height. Then each card that moved
    // slides from its old place, a new card fades in, and a removed card fades out.
    function changeCards() {
        const cards = []
        const shown = root.photoDelegates()
        for (let i = 0; i < shown.length; ++i) {
            const card = shown[i]
            if (card.elementId <= 0) {
                continue
            }
            cards.push({ target: card, elementId: card.elementId, index: card.cardIndex,
                         x: card.x - root.view.contentX, y: card.y - root.view.contentY,
                         w: card.width, h: card.height, thumb: card.liveThumbUrl })
        }
        cards.sort(function(a, b) { return a.index - b.index })
        return cards
    }

    function finishChangeAnimations() {
        changeLayoutAnimDoneTimer.stop()
        const cards = root.photoDelegates()
        for (let i = 0; i < cards.length; i++) {
            cards[i].finishZoomLayoutAnim()
        }
        for (let g = changeGhostLayer.children.length - 1; g >= 0; --g) {
            changeGhostLayer.children[g].destroy()
        }
        _changeLayoutAnimating = false
    }

    function captureChange() {
        _changeCapture = null
        if (!root.layoutReady()) {
            return
        }
        finishChangeAnimations()
        root.finishActiveZoomLayoutAnimations()
        _changeLayoutAnimating = true
        const cards = changeCards()
        const byId = {}
        for (let i = 0; i < cards.length; ++i) {
            cards[i].onScreen = cards[i].y + cards[i].h > 0 && cards[i].y < root.view.height
            byId[cards[i].elementId] = cards[i]
        }
        const anchorRow = appModules.library.thumbnailModel.getItemAt(selectionAnchorIndex)
        _changeCapture = { cards: cards, byId: byId,
                           selectionAnchorId: anchorRow && anchorRow.elementId
                                              ? Number(anchorRow.elementId) : 0 }
    }

    function keepChangeAnchor(cards) {
        const model = appModules.library.thumbnailModel
        for (let i = 0; i < cards.length; ++i) {
            const card = cards[i]
            if (card.y + card.h <= 0) {
                continue
            }
            const index = model.rowByElementId(card.elementId)
            if (index < 0) {
                continue
            }
            const item = root.delegateForIndex(index)
            const y = item ? item.y : root.view.originY + root.rowTopOfIndex(index)
            root.view.contentY = root.clampYForHeight(root.layoutContentHeight(), y - card.y)
            if (!_layoutJustified) {
                grid.forceLayout()
            }
            return
        }
    }

    function applyChange() {
        const capture = _changeCapture
        _changeCapture = null
        if (!capture) {
            _changeLayoutAnimating = false
            return
        }
        if (!_layoutJustified) {
            grid.forceLayout()
        }
        keepChangeAnchor(capture.cards)
        if (capture.selectionAnchorId > 0) {
            selectionAnchorIndex = root.loadedIndexForElement(capture.selectionAnchorId)
        }

        const after = changeCards()
        const shown = {}
        const moves = []
        const fades = []
        for (let i = 0; i < after.length; ++i) {
            const card = after[i]
            shown[card.elementId] = true
            const before = capture.byId[card.elementId]
            const onScreen = card.y + card.h > 0 && card.y < root.view.height
            if (before) {
                const dx = before.x - card.x
                const dy = before.y - card.y
                if ((onScreen || before.onScreen) && (Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5)) {
                    moves.push({ target: card.target, dx: dx, dy: dy })
                }
            } else if (onScreen) {
                fades.push(card.target)
            }
        }
        const ghosts = []
        for (let j = 0; j < capture.cards.length; ++j) {
            const card = capture.cards[j]
            if (card.onScreen && !shown[card.elementId]) {
                ghosts.push(card)
            }
        }
        if (moves.length === 0 && fades.length === 0 && ghosts.length === 0
                || fades.length + ghosts.length > changeAnimationLimit) {
            _changeLayoutAnimating = false
            return
        }
        for (let m = 0; m < moves.length; ++m) {
            moves[m].target.startZoomLayoutAnim(moves[m].dx, moves[m].dy, 1, 1, 1)
        }
        for (let f = 0; f < fades.length; ++f) {
            fades[f].startZoomLayoutAnim(0, 0, 1, 1, 0)
        }
        for (let g = 0; g < ghosts.length; ++g) {
            changeGhostComponent.createObject(changeGhostLayer, {
                x: ghosts[g].x, y: ghosts[g].y, width: ghosts[g].w, height: ghosts[g].h,
                toX: ghosts[g].x, toY: ghosts[g].y, toWidth: ghosts[g].w, toHeight: ghosts[g].h,
                source: ghosts[g].thumb
            })
        }
        changeLayoutAnimDoneTimer.restart()
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
        interval: root.delegateReflowDuration + 50
        repeat: false
        onTriggered: root.finishActiveZoomLayoutAnimations()
    }

    Timer {
        id: changeLayoutAnimDoneTimer
        interval: root.delegateReflowDuration + 50
        repeat: false
        onTriggered: root.finishChangeAnimations()
    }

    // Removed cards of an in-place change fade out here, over the view, at their old viewport
    // positions. Cards that leave the view in a zoom move to their new rectangle and fade out.
    Item {
        id: changeGhostLayer
        objectName: "thumbnailGridView_changeGhosts"
        anchors.fill: parent
        clip: true
        z: 1
    }

    Component {
        id: changeGhostComponent
        Rectangle {
            id: ghost
            property string source: ""
            property real toX: 0
            property real toY: 0
            property real toWidth: 0
            property real toHeight: 0
            radius: root.columns >= 11 ? 6 : appTheme.panelRadius
            color: root._layoutJustified ? appTheme.bgBaseColor : root.cardBg
            Image {
                anchors.fill: parent
                anchors.margins: root._layoutJustified ? 0 : root.cardInset
                visible: ghost.source.length > 0
                source: ghost.source
                fillMode: root._layoutJustified ? Image.PreserveAspectCrop
                                                : Image.PreserveAspectFit
            }
            ParallelAnimation {
                running: true
                NumberAnimation {
                    target: ghost; property: "x"; to: ghost.toX
                    duration: root.delegateReflowDuration; easing.type: Easing.OutCubic
                }
                NumberAnimation {
                    target: ghost; property: "y"; to: ghost.toY
                    duration: root.delegateReflowDuration; easing.type: Easing.OutCubic
                }
                NumberAnimation {
                    target: ghost; property: "width"; to: ghost.toWidth
                    duration: root.delegateReflowDuration; easing.type: Easing.OutCubic
                }
                NumberAnimation {
                    target: ghost; property: "height"; to: ghost.toHeight
                    duration: root.delegateReflowDuration; easing.type: Easing.OutCubic
                }
                NumberAnimation {
                    target: ghost; property: "opacity"; from: 1; to: 0
                    duration: root.delegateReflowDuration; easing.type: Easing.OutCubic
                }
                onFinished: ghost.destroy()
            }
        }
    }

    function keyForElement(elementId) {
        return String(Number(elementId))
    }

    function isImageSelected(elementId) {
        // Reading revision makes a binding that calls this read again after each change.
        return selection.revision >= 0 && selection.IsSelected(Number(elementId))
    }

    function isImageQueued(elementId) {
        return Object.prototype.hasOwnProperty.call(
            exportQueueById, keyForElement(elementId))
    }

    function hasMultiSelectModifier(modifiers) {
        return ShortcutRegistry.modifierMatches("library.extendSelection", modifiers)
               || ShortcutRegistry.modifierMatches("library.toggleSelection", modifiers)
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
            root.selection.AddItems(rangeItems)
        } else {
            root.replaceSelection(rangeItems)
        }
        updateSelectionAnchor(index)
    }

    // ── Justified rows ──
    JustifiedLayoutModel {
        id: justifiedModel
        objectName: "thumbnailGridView_justifiedModel"
        source: root._layoutJustified ? appModules.library.thumbnailModel : null
        viewportY: justifiedView.contentY
        viewportHeight: justifiedView.height
    }

    // A plain item with the scroll properties of a Flickable, not a Flickable: the view takes no
    // flick input, and a Flickable moves contentY back into its bounds by itself when the content
    // height changes, after the zoom transition has set the anchored position.
    Item {
        id: justifiedView
        objectName: "thumbnailGridView_justified"
        z: 0
        anchors.fill: parent
        visible: root._layoutJustified
        clip: true
        property real contentX: 0
        property real contentY: 0
        readonly property real originY: 0
        readonly property real contentHeight: justifiedModel.contentHeight
        onContentYChanged: root.maybeLoadMoreThumbnails()

        Item {
            id: justifiedContent
            x: -justifiedView.contentX
            y: -justifiedView.contentY
            width: justifiedView.width
            height: justifiedView.contentHeight

            Repeater {
                id: justifiedRepeater
                model: justifiedModel
                delegate: JustifiedThumbnailTile {
                    view: root
                }
                onCountChanged: root.maybeLoadMoreThumbnails()
            }
        }
    }

    // ── Classic grid ──
    GridView {
        id: grid
        objectName: "thumbnailGridView_grid"
        z: 0
        anchors.fill: parent
        visible: !root._layoutJustified
        model: root._layoutJustified ? null : appModules.library.thumbnailModel
        clip: true
        cacheBuffer: 0
        boundsBehavior: Flickable.StopAtBounds
        cellWidth: root.classicMetrics.cellW
        cellHeight: root.classicMetrics.cellH
        interactive: false
        onContentYChanged: root.maybeLoadMoreThumbnails()
        onContentHeightChanged: {
            if (!root._zoomLayoutAnimating) {
                root.clampContentY()
            }
        }
        onCellWidthChanged: {
            if (!root._zoomLayoutAnimating) {
                root.relayoutAndClamp()
            }
        }
        onCountChanged: {
            root.relayoutAndClamp()
            root.maybeLoadMoreThumbnails()
        }

        delegate: Rectangle {
            id: cardDelegate
            // Stable automation ids: first card is the canonical open-editor target.
            objectName: index === 0 ? "thumbnailGridView_firstCard"
                                    : ("thumbnailGridView_card_" + index)
            required property int index
            required property int elementId
            required property int imageId
            required property string fileName
            required property string cameraModel
            required property int iso
            required property string aperture
            required property string captureDate
            required property int rating
            required property int colorLabels
            required property bool deleteMark
            required property bool isHdr
            required property bool isEdited
            required property var editedAt
            required property string editVersionName
            required property string tags
            required property string accent
            required property string thumbUrl
            required property bool thumbLoading
            required property bool thumbMissingSource
            required property string thumbErrorText
            // Photo index of the card; the view reads it from every delegate kind.
            readonly property int cardIndex: index
            readonly property alias liveThumbUrl: thumbPin.liveThumbUrl
            readonly property string displayTags: root.semanticTagsDisplayText(tags)
            readonly property bool thumbnailReady: thumbPin.liveThumbUrl.length > 0
            readonly property bool thumbnailLoadingState: thumbPin.liveThumbLoading
            readonly property bool thumbnailMissingState: !thumbnailReady && !thumbnailLoadingState
                                                          && thumbPin.liveThumbMissingSource
            readonly property bool thumbnailErrorState: !thumbnailReady && !thumbnailLoadingState
                                                        && thumbPin.liveThumbErrorText.length > 0
            readonly property bool thumbnailProblemState: thumbnailMissingState || thumbnailErrorState
            readonly property string thumbnailProblemText: thumbPin.liveThumbErrorText.length > 0
                                                           ? thumbPin.liveThumbErrorText
                                                           : qsTr("Source file was moved or deleted")

            ThumbnailPinBinding {
                id: thumbPin
                view: root
                elementId: cardDelegate.elementId
                imageId: cardDelegate.imageId
                thumbUrl: cardDelegate.thumbUrl
                thumbLoading: cardDelegate.thumbLoading
                thumbMissingSource: cardDelegate.thumbMissingSource
                thumbErrorText: cardDelegate.thumbErrorText
            }

            function syncThumbnailBinding() {
                thumbPin.sync()
            }

            readonly property bool isSelected: root.isImageSelected(elementId)
            readonly property bool isHovered: root.hoveredCardIndex === index

            // Star (1 to 5) under the overlay point @p x, @p y, or 0 (see ThumbnailStarRating).
            function starAtOverlay(x, y) {
                if (!cardStars.visible || root.hideMetadata) {
                    return 0
                }
                const p = overlay.mapToItem(cardStars, x, y)
                return cardStars.starAt(p.x, p.y)
            }

            function ratingForStar(star) {
                return cardStars.ratingForStar(star)
            }

            function editMarkAtOverlay(x, y) {
                const mark = editMark
                if (!mark.visible) {
                    return false
                }
                const p = overlay.mapToItem(mark, x, y)
                return p.x >= 0 && p.y >= 0 && p.x < mark.width && p.y < mark.height
            }

            Accessible.role: Accessible.ListItem
            Accessible.name: {
                const parts = [fileName,
                               rating > 0 ? qsTr("%n star(s)", "", rating) : qsTr("Unrated")]
                if (isEdited) {
                    parts.push(qsTr("Edited"))
                }
                if (colorLabels !== 0) {
                    parts.push(cardLabelDots.Accessible.name)
                }
                if (deleteMark) {
                    parts.push(cardDeleteMark.Accessible.name)
                }
                return parts.join(", ")
            }
            Accessible.selected: isSelected

            width: grid.cellWidth - root.delegateGap
            height: grid.cellHeight - root.delegateGap
            radius: root.columns >= 11 ? 6 : appTheme.panelRadius
            // Monochrome selection: the normal card fill and a 2 px text-color outline, the
            // same as AlbumSectionView; never an accent wash or frame.
            color: isHovered ? root.cardBgHover : root.cardBg
            border.width: isSelected ? 2 : 0
            border.color: root.cardText
            Behavior on color { ColorAnimation { duration: 120 } }
            Behavior on border.width { NumberAnimation { duration: 150 } }
            opacity: zoomOpacity

            // The card is drawn at (x + zoomTranslateX, y + zoomTranslateY) with its size
            // scaled by zoomScaleX and zoomScaleY from its top-left corner.
            transform: [
                Scale {
                    xScale: cardDelegate.zoomScaleX
                    yScale: cardDelegate.zoomScaleY
                },
                Translate {
                    x: cardDelegate.zoomTranslateX
                    y: cardDelegate.zoomTranslateY
                }
            ]
            property real zoomTranslateX: 0
            property real zoomTranslateY: 0
            property real zoomScaleX: 1
            property real zoomScaleY: 1
            property real zoomOpacity: 1

            ThumbnailLayoutAnimation {
                id: layoutAnimation
                card: cardDelegate
                duration: root.delegateReflowDuration
            }

            function startZoomLayoutAnim(fromX, fromY, fromScaleX, fromScaleY, fromOpacity) {
                layoutAnimation.start(fromX, fromY, fromScaleX, fromScaleY, fromOpacity)
            }

            function finishZoomLayoutAnim() {
                layoutAnimation.finish()
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
                ThumbnailLoadingSpinner {
                    anchors.centerIn: parent
                    width: 28
                    height: 28
                    visible: cardDelegate.thumbnailLoadingState
                }
                Image {
                    id: thumbImage
                    anchors.centerIn: parent
                    width: parent.width - 4
                    height: parent.height - 4
                    source: thumbPin.liveThumbUrl
                    visible: false
                    asynchronous: true
                    // A new tier keeps the shown thumbnail until the new image is ready.
                    retainWhileLoading: true
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
                    saturation: cardDeleteMark.photoSaturation
                    visible: cardDelegate.thumbnailReady
                }
                // Delete mark: the scrim over the photo box, below the edit mark.
                ThumbnailDeleteMark {
                    id: cardDeleteMark
                    objectName: "thumbnailGridDeleteMark"
                    anchors.fill: parent
                    radius: root.columns >= 11 ? 5 : 10
                    marked: cardDelegate.deleteMark
                    compact: root.columns >= 8
                }
                // Edit mark: a chip on the top-right corner of the photo.
                ThumbnailEditMark {
                    id: editMark
                    image: thumbImage
                    compact: root.columns >= 8
                    // Left of the problem dot (8 px margin, 18 px) when the source is missing.
                    rightInset: cardDelegate.thumbnailProblemState ? 30 : editMark.inset
                    edited: isEdited
                    editedAt: cardDelegate.editedAt
                    versionName: editVersionName
                    pointerOver: root.hoveredEditMarkIndex === index
                }
                Rectangle {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 8
                    width: 18
                    height: 18
                    radius: 9
                    visible: cardDelegate.thumbnailProblemState
                    color: root.cardDangerTint
                    border.width: 1
                    border.color: root.cardDanger
                }
                Column {
                    anchors.centerIn: parent
                    width: parent.width - 12
                    visible: cardDelegate.thumbnailProblemState
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
                        text: cardDelegate.thumbnailProblemText
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
                ToolTip.visible: cardDelegate.thumbnailProblemState && thumbHover.hovered
                ToolTip.text: cardDelegate.thumbnailProblemText
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

                // 8 and 11 columns: one line with the file name, the color label dots, and the
                // filled stars. The file name elides first.
                Item {
                    visible: root.compactText && !root.hideAllText
                    width: parent.width
                    height: root.textAreaHeight

                    Label {
                        id: compactName
                        objectName: "thumbnailGridCompactName"
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, Math.max(0, parent.width
                                        - compactTrailing.width
                                        - (compactTrailing.width > 0 ? 4 : 0)
                                        - (compactLabelDots.visible
                                           ? compactLabelDots.width + compactLabelDots.leadingGap
                                           : 0)))
                        text: fileName
                        color: cardDelegate.deleteMark ? root.cardMuted : root.cardText
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: root.titleFontSize
                        font.weight: root.dataFontWeight
                        font.letterSpacing: root.dataLetterSpacing
                        elide: Text.ElideRight
                    }

                    ThumbnailColorLabelDots {
                        id: compactLabelDots
                        objectName: "thumbnailGridCompactLabelDots"
                        anchors.left: compactName.right
                        anchors.leftMargin: leadingGap
                        anchors.verticalCenter: parent.verticalCenter
                        labels: cardDelegate.colorLabels
                        compact: true
                        ringColor: cardDelegate.color
                    }

                    Row {
                        id: compactTrailing
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        ThumbnailStarRating {
                            objectName: "thumbnailGridCompactStars"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: rating > 0
                            photoId: elementId
                            rating: cardDelegate.rating
                            interactive: false
                            pixelSize: root.titleFontSize
                        }
                    }
                }

                // 2 to 6 columns: file name with the color label dots; camera line; date line
                // with the stars.
                Item {
                    visible: !root.compactText
                    width: parent.width
                    height: titleLabel.implicitHeight

                    Label {
                        id: titleLabel
                        objectName: "thumbnailGridName"
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, Math.max(0, parent.width
                                        - (cardLabelDots.visible
                                           ? cardLabelDots.width + cardLabelDots.leadingGap
                                           : 0)))
                        text: fileName
                        color: cardDelegate.deleteMark ? root.cardMuted : root.cardText
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: root.titleFontSize
                        font.weight: root.dataFontWeight
                        font.letterSpacing: root.dataLetterSpacing
                        elide: Text.ElideRight
                    }
                    ThumbnailColorLabelDots {
                        id: cardLabelDots
                        objectName: "thumbnailGridLabelDots"
                        anchors.left: titleLabel.right
                        anchors.leftMargin: leadingGap
                        anchors.verticalCenter: parent.verticalCenter
                        labels: cardDelegate.colorLabels
                        ringColor: cardDelegate.color
                    }
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
                Item {
                    visible: !root.hideMetadata
                    width: parent.width
                    height: Math.max(dateLabel.implicitHeight, cardStars.implicitHeight,
                                     hdrGridTag.visible ? hdrGridTag.height : 0)

                    Label {
                        id: dateLabel
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, (hdrGridTag.visible ? hdrGridTag.x : cardStars.x) - 4)
                        text: cardDelegate.displayTags.length > 0
                              ? qsTr("%1 | %2").arg(captureDate).arg(cardDelegate.displayTags)
                              : captureDate
                        color: root.cardMuted
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: root.metadataFontSize
                        font.weight: root.dataFontWeight
                        font.letterSpacing: root.dataLetterSpacing
                        elide: Text.ElideRight
                    }

                    // Neutral HDR mark: the Library has no accent color.
                    Rectangle {
                        id: hdrGridTag
                        visible: isHdr
                        anchors.right: cardStars.left
                        anchors.rightMargin: 5
                        anchors.verticalCenter: parent.verticalCenter
                        width: appTheme.iconSourceSizeCompact + appTheme.spaceSm
                        height: appTheme.spaceLg
                        radius: 3
                        color: appTheme.bgBaseColor
                        border.width: 1
                        border.color: appTheme.textMutedColor
                        ColorImage {
                            anchors.centerIn: parent
                            width: appTheme.iconSourceSize
                            height: appTheme.iconSourceSize
                            sourceSize.width: appTheme.iconSourceSize
                            sourceSize.height: appTheme.iconSourceSize
                            source: "qrc:/panel_icons/hdr.svg"
                            color: appTheme.textColor
                        }
                    }

                    ThumbnailStarRating {
                        id: cardStars
                        objectName: "thumbnailGridStars"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        photoId: elementId
                        rating: cardDelegate.rating
                        interactive: root.starsInteractive
                        cardHovered: cardDelegate.isHovered
                        hoveredStar: root.hoveredCardIndex === index ? root.hoveredCardStar : 0
                        pixelSize: root.titleFontSize
                    }
                }
            }
        }
    }

    // ── Interaction overlay ──
    MouseArea {
        id: overlay
        objectName: "thumbnailGridView_inputOverlay"
        z: 20
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        preventStealing: true

        property int hoveredIndex: -1
        // Star (1 to 5) under the pointer on the hovered card, or 0 (2 to 6 columns only).
        property int hoveredStar: 0
        // Card index whose edit mark is under the pointer, or -1.
        property int hoveredEditMarkIndex: -1
        // Star and card under the left press; a release on the same star rates the photo.
        property int pressedStar: 0
        property int pressedStarIndex: -1
        property point dragStart: Qt.point(0, 0)
        property point dragCurrent: Qt.point(0, 0)
        // Scroll position at the press, measured from the first row (view.originY), so that a
        // band keeps its rows when the content origin moves.
        property real dragStartContentY: 0
        property bool isDragging: false
        property bool dragAdditive: false

        cursorShape: hoveredIndex >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor

        function gridIndexAt(viewX, viewY) {
            return root.cardIndexAt(viewX, viewY)
        }

        function starAt(idx, viewX, viewY) {
            if (idx < 0 || !root.starsInteractive) {
                return 0
            }
            const card = root.delegateForIndex(idx)
            return card ? card.starAtOverlay(viewX, viewY) : 0
        }

        function updatePointerTargets(viewX, viewY) {
            hoveredIndex = gridIndexAt(viewX, viewY)
            hoveredStar = starAt(hoveredIndex, viewX, viewY)
            const card = hoveredIndex >= 0 ? root.delegateForIndex(hoveredIndex) : null
            hoveredEditMarkIndex = card && card.editMarkAtOverlay(viewX, viewY) ? hoveredIndex : -1
        }

        function hasSelectionModifier(modifiers) {
            return ShortcutRegistry.modifierMatches("library.extendSelection", modifiers)
                    || ShortcutRegistry.modifierMatches("library.toggleSelection", modifiers)
        }

        // Band edges in layout coordinates: 0 is the top of the first row. GridView moves
        // originY away from 0 after a relayout or a change above the view, so contentY alone
        // is not a row position.
        function layoutContentY() {
            return root.view.contentY - root.view.originY
        }

        function dragStartContentTop() {
            return dragStart.y + dragStartContentY
        }

        function dragCurrentContentTop() {
            return dragCurrent.y + layoutContentY()
        }

        function rubberBandViewportY() {
            return Math.min(dragStartContentTop(), dragCurrentContentTop()) - layoutContentY()
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
                // The selection from before the drag plus the photos in the band.
                root.selection.SetRangeItems(bandItems)
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
            root.selection.EndRange()
            applyRubberBandSelection()
        }

        // Justified rows: the loaded tiles that the band covers.
        function collectJustifiedBandItems() {
            const bLeft = Math.max(0, Math.min(dragStart.x, dragCurrent.x))
            const bRight = Math.min(justifiedView.width, Math.max(dragStart.x, dragCurrent.x))
            const bTop = Math.max(0, Math.min(dragStartContentTop(), dragCurrentContentTop()))
            const bBottom = Math.max(dragStartContentTop(), dragCurrentContentTop())
            const indices = justifiedModel.indicesInRect(bLeft + justifiedView.contentX, bTop,
                                                         Math.max(0, bRight - bLeft),
                                                         Math.max(0, bBottom - bTop))
            const items = []
            for (let i = 0; i < indices.length; ++i) {
                const item = root.selectionItemForIndex(indices[i])
                if (item) {
                    items.push(item)
                }
            }
            return items
        }

        function collectRubberBandItems() {
            if (root._layoutJustified) {
                return collectJustifiedBandItems()
            }
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
            updatePointerTargets(mouse.x, mouse.y)
        }

        onPressed: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                const idx = gridIndexAt(mouse.x, mouse.y)
                if (idx >= 0) {
                    const item = root.selectionItemForIndex(idx)
                    if (item) {
                        root.imageFocused(item, idx)
                        const scenePoint = overlay.mapToItem(null, mouse.x, mouse.y)
                        root.contextMenuRequested(item, scenePoint.x, scenePoint.y)
                    }
                }
                return
            }
            dragStart = Qt.point(mouse.x, mouse.y)
            dragCurrent = Qt.point(mouse.x, mouse.y)
            dragStartContentY = layoutContentY()
            isDragging = false
            dragAdditive = false
            pressedStarIndex = gridIndexAt(mouse.x, mouse.y)
            pressedStar = hasSelectionModifier(mouse.modifiers)
                    ? 0 : starAt(pressedStarIndex, mouse.x, mouse.y)
            mouse.accepted = true
        }

        onReleased: function(mouse) {
            if (mouse.button !== Qt.LeftButton) {
                isDragging = false
                return
            }
            if (!isDragging && pressedStar > 0) {
                // A click on a card star rates that photo only; the selection stays.
                const idx = gridIndexAt(mouse.x, mouse.y)
                const star = starAt(idx, mouse.x, mouse.y)
                const card = idx >= 0 ? root.delegateForIndex(idx) : null
                const item = idx >= 0 ? root.selectionItemForIndex(idx) : null
                if (idx === pressedStarIndex && star === pressedStar && card && item) {
                    root.ratingRequested(item, card.ratingForStar(star))
                }
                pressedStar = 0
                pressedStarIndex = -1
                isDragging = false
                return
            }
            pressedStar = 0
            pressedStarIndex = -1
            if (!isDragging) {
                const idx = gridIndexAt(mouse.x, mouse.y)
                if (idx >= 0) {
                    const item = root.selectionItemForIndex(idx)
                    if (item) {
                        root.imageFocused(item, idx)
                        if (ShortcutRegistry.modifierMatches("library.extendSelection",
                                                             mouse.modifiers)) {
                            root.selectRangeToIndex(
                                idx, ShortcutRegistry.modifierMatches("library.toggleSelection",
                                                                      mouse.modifiers))
                        } else if (ShortcutRegistry.modifierMatches("library.toggleSelection",
                                                                    mouse.modifiers)) {
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
            // Two quick star clicks rate twice; they do not open the editor.
            if (starAt(idx, mouse.x, mouse.y) > 0) {
                return
            }
            if (idx >= 0) {
                const item = root.selectionItemForIndex(idx)
                if (item) {
                    root.imageFocused(item, idx)
                    appModules.workspaceRouter.openEditor(item.elementId, item.imageId)
                }
            }
        }

        onExited: {
            hoveredIndex = -1
            hoveredStar = 0
            hoveredEditMarkIndex = -1
        }

        onWheel: function(wheel) {
            // Ctrl+Scroll → zoom, otherwise → scroll. A zoom step needs one wheel notch
            // (120); a touchpad sends smaller steps, which add up.
            if (wheel.modifiers & Qt.ControlModifier) {
                const angle = wheel.angleDelta.y
                if (angle !== 0 && (root._zoomWheelAngle > 0) !== (angle > 0)) {
                    root._zoomWheelAngle = 0
                }
                root._zoomWheelAngle += angle
                while (Math.abs(root._zoomWheelAngle) >= 120) {
                    const step = root._zoomWheelAngle > 0 ? 1 : -1
                    root._zoomWheelAngle -= step * 120
                    root.setZoomLevelAt(root.zoomLevel + step, wheel.y, wheel.x)
                }
                updatePointerTargets(wheel.x, wheel.y)
                wheel.accepted = true
                return
            }

            const wheelDelta = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y : wheel.angleDelta.y
            root.scrollBy(-wheelDelta)
            root.maybeLoadMoreThumbnails()
            if (isDragging) {
                applyRubberBandSelection()
            }
            updatePointerTargets(wheel.x, wheel.y)
            wheel.accepted = true
        }

        // A child of the overlay: the bar takes its own presses and drags, and a wheel over it
        // reaches the overlay, which scrolls the view.
        ScrollBar {
            id: gridScrollBar
            objectName: "thumbnailGridView_scrollBar"
            z: 1
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            orientation: Qt.Vertical
            policy: ScrollBar.AsNeeded
            size: {
                const height = root.layoutContentHeight()
                return height > root.view.height ? root.view.height / height : 1
            }
            // Shown while it is hovered or dragged, and for a short time after a scroll.
            active: hovered || pressed || scrollActivityTimer.running
            onPositionChanged: {
                if (pressed) {
                    root.scrollToFraction(position)
                }
            }
            Binding on position {
                when: !gridScrollBar.pressed
                value: root.scrollFraction()
                restoreMode: Binding.RestoreNone
            }
        }

        Timer {
            id: scrollActivityTimer
            interval: 800
            repeat: false
        }
        Connections {
            target: root.view
            function onContentYChanged() { scrollActivityTimer.restart() }
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
