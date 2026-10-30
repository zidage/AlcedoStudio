import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Grouped Library photo area. One vertical ListView over LibraryModule.sectionModel: each
// logical row is a full-width group header or one line of at most `columnCount` photo cells.
// Rows are recycled (reuseItems); selection, collapse state, and loaded photos live in their
// owners, never in a recycled item. Header and photo-row heights are fixed
// (librarySectionHeaderHeight and the zoom-derived photo row height), so scroll positions
// follow AlbumSectionModel.RowOffset exactly. The view emits the same selection, focus,
// context-menu, and zoom signals as ThumbnailGridView, so LibraryWorkspace handles both alike.
Item {
    id: root
    objectName: "albumSectionView"
    clip: true

    // The library selection (LibrarySelection). A delegate asks it for its own photo.
    readonly property var selection: appModules.library.selection
    property var exportQueueById: ({})
    property int zoomLevel: 4

    readonly property var library: appModules.library
    readonly property var sections: library.sectionModel
    readonly property var photos: library.thumbnailModel

    // Same zoom ladder and thumbnail tiers as ThumbnailGridView.
    readonly property var zoomColumns: [2, 3, 4, 5, 6, 8, 11, 14]
    readonly property var zoomResolutionEdges: [2048, 1024, 1024, 512, 512, 256, 256, 256]
    readonly property int zoomIndex: Math.max(0, Math.min(zoomColumns.length - 1, zoomLevel))
    readonly property int visualColumns: zoomColumns[zoomIndex]
    readonly property int desiredMaxEdge: zoomResolutionEdges[zoomIndex]
    readonly property int cardInset: visualColumns >= 11 ? 4 : (visualColumns >= 8 ? 6 : 8)
    readonly property int cellGap: visualColumns >= 11 ? 4 : (visualColumns >= 8 ? 6 : 12)
    // 2 to 6 columns: the file-name line and the date line with the star control. 8 and 11
    // columns: one line with the file name, the edit mark, and the filled stars.
    readonly property bool compactText: visualColumns >= 8
    readonly property bool starsInteractive: visualColumns <= 6
    readonly property int textAreaHeight: visualColumns >= 14 ? 0 : (compactText ? 18 : 36)
    readonly property int cellWidth: Math.max(72, Math.floor(list.width / visualColumns))
    readonly property int columnCount: Math.max(1, Math.floor(list.width / cellWidth))
    readonly property int photoRowHeight: Math.max(64, Math.round(
        (cellWidth - cardInset * 2) * 2 / 3 + textAreaHeight + cardInset * 2 + cellGap))
    readonly property int headerHeight: appTheme.librarySectionHeaderHeight

    // Keyboard focus and range anchor are occurrence positions in the section stream.
    property int focusOccurrence: -1
    property int anchorOccurrence: -1
    property int photoRevision: 0
    property real pendingSelectRequest: 0
    property bool pendingSelectAdditive: false
    // Scroll anchor that waits for the final column count (see restoreScrollAnchor), and the
    // anchor of the current position, kept while the rows match this view's geometry.
    property var pendingScrollAnchor: null
    property var currentScrollAnchor: null
    // In-place change of the shown result (an import or a delete): the headers and cells of the
    // created rows before the change, read on libraryChangeAboutToApply.
    property var changeCapture: null
    property bool changeApplying: false
    // When more photos and headers than this appear or disappear in the view, the change is
    // shown without animation; the scroll anchor stays.
    readonly property int changeAnimationLimit: 48
    readonly property int changeAnimationDuration: 260

    signal imageSelectionChanged(int elementId, int imageId, string fileName, bool isHdr,
                                 bool selected)
    signal replaceSelection(var items)
    signal imageFocused(var item, int index)
    signal contextMenuRequested(var item, real sceneX, real sceneY)
    signal zoomChanged(int zoomLevel)
    // A star click on the cell of @p item (2 to 6 columns): rate that photo only.
    signal ratingRequested(var item, int rating)

    activeFocusOnTab: true
    Accessible.role: Accessible.List
    Accessible.name: qsTr("Grouped photos")

    // A new column count rebuilds the rows (a model reset puts the list at the top), so the
    // row at the top of the view is kept through the change.
    onColumnCountChanged: {
        const anchor = pendingScrollAnchor || currentScrollAnchor
        pendingScrollAnchor = anchor
        sections.SetColumnCount(columnCount)
        Qt.callLater(root.applyPendingScrollAnchor)
    }
    Component.onCompleted: sections.SetColumnCount(columnCount)

    function photoFor(fileId, revision) {
        if (fileId <= 0) {
            return null
        }
        const row = photos.rowByElementId(fileId)
        if (row < 0) {
            return null
        }
        const photo = photos.getItemAt(row)
        const states = photos.getThumbnailStatesInRange(row, row)
        photo.thumbUrl = states.length > 0 && states[0].thumbUrl ? String(states[0].thumbUrl) : ""
        return photo
    }

    function selectionItem(photo) {
        if (!photo || !photo.elementId) {
            return null
        }
        return {
            elementId: Number(photo.elementId),
            fileId: Number(photo.fileId || photo.elementId),
            imageId: Number(photo.imageId),
            folderId: Number(photo.folderId || 0),
            scopeType: photo.scopeType ? String(photo.scopeType) : "",
            fileName: photo.fileName ? photo.fileName : qsTr("(unnamed)"),
            rating: Number(photo.rating),
            isHdr: photo.isHdr === true
        }
    }

    function isSelected(elementId) {
        // Reading revision makes a binding that calls this read again after each change.
        return selection.revision >= 0 && selection.IsSelected(Number(elementId))
    }

    function groupTitleText(title, unknown) {
        const field = library.groupField
        if (unknown) {
            if (field === "label") {
                return qsTr("Unlabelled")
            }
            return field === "edited" ? qsTr("Unedited") : qsTr("Unknown")
        }
        if (field === "rating") {
            return Number(title) === 0 ? qsTr("Unrated") : qsTr("%n star(s)", "", Number(title))
        }
        if (field === "label") {
            const normalized = String(title).trim().toLowerCase()
            return normalized.length > 0
                   ? normalized.charAt(0).toUpperCase() + normalized.slice(1) : String(title)
        }
        return String(title)
    }

    // ── Scroll anchor ──
    // The scroll position as the photo at the top of the view: the first occurrence of the top
    // row (the group's first occurrence for a header) and the pixel offset into that row. It
    // survives a new column count, a collapsed group above, and the teardown of this view.
    function layoutReady() {
        return list.width > 0 && list.height > 0 && sections.columnCount === root.columnCount
    }

    function scrollAnchor() {
        if (pendingScrollAnchor) {
            return pendingScrollAnchor
        }
        if (!root.layoutReady()) {
            return currentScrollAnchor
        }
        const row = sections.RowAtOffset(list.contentY, headerHeight, photoRowHeight)
        if (row < 0) {
            return null
        }
        const info = sections.RowInfo(row)
        return {
            groupField: library.groupField,
            occurrence: Number(info.firstOccurrence),
            header: info.kind === 0,
            offset: list.contentY - sections.RowOffset(row, headerHeight, photoRowHeight)
        }
    }

    function applyScrollAnchor(anchor) {
        if (!anchor || anchor.groupField !== library.groupField) {
            return
        }
        const row = anchor.header
                    ? sections.GroupHeaderRow(sections.GroupForOccurrence(anchor.occurrence))
                    : sections.RowForOccurrence(anchor.occurrence)
        if (row < 0) {
            return
        }
        const rowHeight = sections.RowInfo(row).kind === 0 ? headerHeight : photoRowHeight
        const target = sections.RowOffset(row, headerHeight, photoRowHeight)
                     + Math.min(Math.max(0, Number(anchor.offset)), rowHeight - 1)
        const maxY = Math.max(0, sections.ContentHeight(headerHeight, photoRowHeight) - list.height)
        list.contentY = Math.max(0, Math.min(maxY, target))
    }

    // Restore a scroll anchor that this view (or an earlier instance of it) returned. It is
    // applied once the list has its size and the model has this view's column count.
    function restoreScrollAnchor(anchor) {
        pendingScrollAnchor = anchor || null
        applyPendingScrollAnchor()
    }

    function trackScrollAnchor() {
        if (!pendingScrollAnchor && root.layoutReady() && list.count > 0) {
            currentScrollAnchor = scrollAnchor()
        }
    }

    function applyPendingScrollAnchor() {
        if (!pendingScrollAnchor || !root.layoutReady() || list.count <= 0) {
            return
        }
        const anchor = pendingScrollAnchor
        pendingScrollAnchor = null
        applyScrollAnchor(anchor)
    }

    // Cross-workspace reveal: the library finds the photo's occurrence on the query worker and
    // the view scrolls just enough to show it (focusPositionReady).
    function scrollToElementAtTop(elementId) {
        if (Number(elementId) <= 0) {
            return false
        }
        library.RequestFocusPosition(Number(elementId))
        return true
    }

    function scrollToRow(row) {
        if (row < 0) {
            return
        }
        const top = sections.RowOffset(row, headerHeight, photoRowHeight)
        const bottom = top + (sections.RowInfo(row).kind === 0 ? headerHeight : photoRowHeight)
        if (top < list.contentY) {
            list.contentY = top
        } else if (bottom > list.contentY + list.height) {
            list.contentY = Math.max(0, bottom - list.height)
        }
    }

    // ── In-place change (import or delete) ──
    // The view keeps the first visible photo (or header) at its position on the screen: before
    // the change it records each created header and cell with its viewport position; after the
    // change it puts the row of the first recorded item that still exists at the same height,
    // from the real delegate position (ListView keeps the rows it shows in place when rows
    // above them change, so the row offsets of the model geometry can differ from contentY).
    // Then each item that moved slides from its old place, a new item fades in, and a removed
    // item fades out.
    function groupKeyText(title, unknown) {
        return unknown ? "u" : "t:" + title
    }

    function createdRows() {
        const rows = []
        const children = list.contentItem.children
        for (let i = 0; i < children.length; ++i) {
            const child = children[i]
            if (child && child.rowKind !== undefined && child.inUse && child.visible) {
                rows.push(child)
            }
        }
        rows.sort(function(a, b) { return a.y - b.y })
        return rows
    }

    // Headers and loaded cells of the created rows in visual order, with viewport positions.
    function changeEntries() {
        const entries = []
        const rows = createdRows()
        for (let r = 0; r < rows.length; ++r) {
            const row = rows[r]
            const rowTop = row.y - list.contentY
            const groupKey = groupKeyText(row.groupTitle, row.groupUnknown)
            if (row.rowKind === 0) {
                entries.push({ key: "h|" + groupKey, header: true, target: row,
                               title: row.groupTitle, unknown: row.groupUnknown, fileId: 0,
                               occurrence: -1, x: 0, y: rowTop, w: row.width, h: row.height,
                               rowTop: rowTop, thumb: "",
                               text: root.groupTitleText(row.groupTitle, row.groupUnknown) })
                continue
            }
            for (let c = 0; c < row.cellCount(); ++c) {
                const cell = row.cellAt(c)
                if (!cell || cell.fileId <= 0) {
                    continue
                }
                const p = cell.mapToItem(list, 0, 0)
                entries.push({ key: "p|" + groupKey + "|" + cell.fileId, header: false,
                               target: cell, title: row.groupTitle, unknown: row.groupUnknown,
                               fileId: cell.fileId, occurrence: cell.occurrence, x: p.x, y: p.y,
                               w: cell.width, h: cell.height, rowTop: rowTop,
                               thumb: cell.liveThumbUrl, text: "" })
            }
        }
        return entries
    }

    function finishChangeAnimations() {
        const rows = createdRows()
        for (let r = 0; r < rows.length; ++r) {
            rows[r].finishFlips()
        }
        for (let g = ghostLayer.children.length - 1; g >= 0; --g) {
            ghostLayer.children[g].destroy()
        }
    }

    function captureChange() {
        changeCapture = null
        changeApplying = true
        if (list.width <= 0 || list.height <= 0 || list.count <= 0) {
            return
        }
        finishChangeAnimations()
        const entries = changeEntries()
        const byKey = {}
        let focusEntry = null
        let rangeEntry = null
        for (let i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            entry.onScreen = entry.y + entry.h > 0 && entry.y < list.height
            byKey[entry.key] = entry
            if (!entry.header && entry.occurrence === focusOccurrence) {
                focusEntry = entry
            }
            if (!entry.header && entry.occurrence === anchorOccurrence) {
                rangeEntry = entry
            }
        }
        changeCapture = { entries: entries, byKey: byKey, focus: focusEntry, range: rangeEntry }
    }

    // Row that shows a recorded entry after the change; -1 when its photo or group left.
    function rowOfEntry(entry) {
        const group = sections.GroupIndexForTitle(entry.title, entry.unknown)
        if (group < 0) {
            return -1
        }
        if (entry.header) {
            return sections.GroupHeaderRow(group)
        }
        const occurrence = sections.LoadedOccurrenceOf(entry.fileId, group)
        return occurrence >= 0 ? sections.RowForOccurrence(occurrence) : -1
    }

    function occurrenceOfEntry(entry) {
        if (!entry) {
            return -1
        }
        const group = sections.GroupIndexForTitle(entry.title, entry.unknown)
        return group >= 0 ? Number(sections.LoadedOccurrenceOf(entry.fileId, group)) : -1
    }

    function keepChangeAnchor(entries) {
        for (let i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            if (entry.y + entry.h <= 0) {
                continue
            }
            const row = rowOfEntry(entry)
            if (row < 0) {
                continue
            }
            let item = list.itemAtIndex(row)
            if (!item) {
                list.positionViewAtIndex(row, ListView.Beginning)
                list.forceLayout()
                item = list.itemAtIndex(row)
            }
            if (!item) {
                return
            }
            const maxY = list.originY + Math.max(0, list.contentHeight - list.height)
            list.contentY = Math.max(list.originY, Math.min(maxY, item.y - entry.rowTop))
            list.forceLayout()
            return
        }
    }

    function applyChange() {
        const capture = changeCapture
        changeCapture = null
        changeApplying = false
        photoRevision++
        if (!capture) {
            visibleRowsTimer.restart()
            return
        }
        list.forceLayout()
        keepChangeAnchor(capture.entries)
        focusOccurrence = occurrenceOfEntry(capture.focus)
        anchorOccurrence = occurrenceOfEntry(capture.range)
        visibleRowsTimer.restart()

        const after = changeEntries()
        const shown = {}
        const moves = []
        const fades = []
        for (let i = 0; i < after.length; ++i) {
            const entry = after[i]
            shown[entry.key] = true
            const before = capture.byKey[entry.key]
            const onScreen = entry.y + entry.h > 0 && entry.y < list.height
            if (before) {
                const dx = before.x - entry.x
                const dy = before.y - entry.y
                if ((onScreen || before.onScreen) && (Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5)) {
                    moves.push({ target: entry.target, dx: dx, dy: dy })
                }
            } else if (onScreen) {
                fades.push(entry.target)
            }
        }
        const ghosts = []
        for (let j = 0; j < capture.entries.length; ++j) {
            const entry = capture.entries[j]
            if (entry.onScreen && !shown[entry.key]) {
                ghosts.push(entry)
            }
        }
        if (fades.length + ghosts.length > changeAnimationLimit) {
            return
        }
        for (let m = 0; m < moves.length; ++m) {
            moves[m].target.startFlip(moves[m].dx, moves[m].dy, 1)
        }
        for (let f = 0; f < fades.length; ++f) {
            fades[f].startFlip(0, 0, 0)
        }
        for (let g = 0; g < ghosts.length; ++g) {
            const entry = ghosts[g]
            changeGhostComponent.createObject(ghostLayer, {
                x: entry.header ? entry.x : entry.x + cellGap / 2,
                y: entry.header ? entry.y : entry.y + cellGap / 2,
                width: entry.header ? entry.w : entry.w - cellGap,
                height: entry.header ? entry.h : entry.h - cellGap,
                source: entry.thumb, title: entry.text
            })
        }
    }

    // The rows at the top and the bottom edge of the viewport. After a jump, ListView places the
    // rows from an estimated row height, so the rows on the screen are read from the delegates;
    // the model geometry is used only where no delegate is created yet.
    function requestVisibleRows() {
        if (list.count <= 0 || list.height <= 0) {
            return
        }
        let first = list.indexAt(1, list.contentY)
        let last = list.indexAt(1, list.contentY + list.height - 1)
        if (first < 0) {
            first = sections.RowAtOffset(list.contentY, headerHeight, photoRowHeight)
        }
        if (last < 0) {
            last = first >= 0 && list.itemAtIndex(first)
                   ? list.count - 1
                   : sections.RowAtOffset(list.contentY + list.height, headerHeight,
                                          photoRowHeight)
        }
        library.RequestSectionRows(Math.max(0, first), Math.max(first, last))
    }

    // ── Focus movement over photo cells (headers are skipped) ──
    function photoRowAfter(row, step) {
        for (let next = row + step; next >= 0 && next < list.count; next += step) {
            if (sections.RowInfo(next).kind === 1) {
                return next
            }
        }
        return -1
    }

    function moveFocus(key) {
        if (sections.occurrenceCount <= 0) {
            return -1
        }
        if (focusOccurrence < 0) {
            const firstRow = photoRowAfter(-1, 1)
            return firstRow >= 0 ? sections.RowInfo(firstRow).firstOccurrence : -1
        }
        const row = sections.RowForOccurrence(focusOccurrence)
        const info = sections.RowInfo(row)
        if (info.kind !== 1) {
            return focusOccurrence
        }
        const column = focusOccurrence - info.firstOccurrence
        if (key === Qt.Key_Right) {
            if (column + 1 < info.occurrenceCount) {
                return focusOccurrence + 1
            }
            const nextRow = photoRowAfter(row, 1)
            return nextRow >= 0 ? sections.RowInfo(nextRow).firstOccurrence : focusOccurrence
        }
        if (key === Qt.Key_Left) {
            if (column > 0) {
                return focusOccurrence - 1
            }
            const prevRow = photoRowAfter(row, -1)
            if (prevRow < 0) {
                return focusOccurrence
            }
            const prev = sections.RowInfo(prevRow)
            return prev.firstOccurrence + prev.occurrenceCount - 1
        }
        const targetRow = photoRowAfter(row, key === Qt.Key_Down ? 1 : -1)
        if (targetRow < 0) {
            return focusOccurrence
        }
        const target = sections.RowInfo(targetRow)
        return target.firstOccurrence + Math.min(column, target.occurrenceCount - 1)
    }

    function focusAt(occurrence, extend) {
        if (occurrence < 0) {
            return
        }
        focusOccurrence = occurrence
        scrollToRow(sections.RowForOccurrence(occurrence))
        const photo = photoFor(sections.FileIdAt(occurrence), photoRevision)
        const item = selectionItem(photo)
        if (extend && anchorOccurrence >= 0) {
            selectRange(anchorOccurrence, occurrence, false)
        } else if (item) {
            anchorOccurrence = occurrence
            root.replaceSelection([item])
        }
        if (item) {
            root.imageFocused(item, photos.rowByElementId(item.elementId))
        }
    }

    // Shift selection: the visible expanded order between two occurrences, read as ids on
    // the worker so unloaded photos are included without their thumbnails.
    function selectRange(fromOccurrence, toOccurrence, additive) {
        const ranges = sections.VisibleOccurrenceRanges(fromOccurrence, toOccurrence)
        pendingSelectAdditive = additive
        pendingSelectRequest = library.RequestOrderedFileIds(ranges)
    }

    function selectAll() {
        pendingSelectRequest = 0
        library.SelectAllInCurrentView()
    }

    Connections {
        target: root.library
        function onOrderedFileIdsReady(requestId, items) {
            if (requestId !== root.pendingSelectRequest) {
                return
            }
            root.pendingSelectRequest = 0
            if (root.pendingSelectAdditive) {
                root.selection.AddItems(items)
            } else {
                root.replaceSelection(items)
            }
        }
        function onFocusPositionReady(fileId, occurrence, sectionRow) {
            if (occurrence >= 0 && sectionRow >= 0) {
                root.focusOccurrence = occurrence
                root.scrollToRow(sectionRow)
            }
        }
        function onLibraryChangeAboutToApply() { root.captureChange() }
        function onLibraryChangeApplied() { root.applyChange() }
    }

    // During an in-place change the photo rows change in runs; the cells read the photos once,
    // when the change is complete (applyChange).
    Connections {
        target: root.photos
        function onModelReset() { root.photoRevision++ }
        function onRowsInserted() {
            if (!root.changeApplying) {
                root.photoRevision++
            }
        }
        function onRowsRemoved() {
            if (!root.changeApplying) {
                root.photoRevision++
            }
        }
        function onDataChanged() { root.photoRevision++ }
    }

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
            root.selectAll()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right
                || event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            root.focusAt(root.moveFocus(event.key), (event.modifiers & Qt.ShiftModifier) !== 0)
            event.accepted = true
            return
        }
        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                && root.focusOccurrence >= 0) {
            const item = root.selectionItem(
                root.photoFor(root.sections.FileIdAt(root.focusOccurrence), root.photoRevision))
            if (item) {
                appModules.workspaceRouter.openEditor(item.elementId, item.imageId)
            }
            event.accepted = true
        }
    }

    // Text action with the shared hover, focus, and accessible behavior.
    component TextAction: Item {
        id: textAction
        property string text: ""
        property string accessibleText: text
        signal activated()

        implicitWidth: actionLabel.implicitWidth + appTheme.spaceSm * 2
        implicitHeight: appTheme.inspectorHeaderActionSize
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: accessibleText
        Accessible.onPressAction: textAction.activated()
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                    || event.key === Qt.Key_Enter) {
                textAction.activated()
                event.accepted = true
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: appTheme.badgeRadius
            color: actionHit.containsMouse ? appTheme.buttonHoveredFillColor : "transparent"
            border.width: textAction.activeFocus ? 1 : 0
            border.color: appTheme.textMutedColor
        }
        Text {
            id: actionLabel
            anchors.centerIn: parent
            text: textAction.text
            color: actionHit.containsMouse ? appTheme.textColor : appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong
        }
        MouseArea {
            id: actionHit
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: textAction.activated()
        }
    }

    // One photo occurrence: reads its photo from the thumbnail model by file id and pins the
    // thumbnail through LibraryModule for (group, file) while its row is in use.
    component PhotoCell: Item {
        id: cell
        objectName: "albumSectionPhotoCell"
        property int occurrence: -1
        property int fileId: 0
        property string groupTitle: ""
        property bool groupUnknown: false
        property bool rowInUse: true
        readonly property var photo: root.photoFor(fileId, root.photoRevision)
        readonly property int imageId: photo ? Number(photo.imageId) : 0
        readonly property bool selected: fileId > 0 && root.isSelected(fileId)
        readonly property bool focused: root.activeFocus && root.focusOccurrence === occurrence
        readonly property int rating: photo ? Number(photo.rating) : 0
        readonly property bool edited: photo ? photo.isEdited === true : false
        readonly property bool hovered: cellHover.hovered || cellStars.pointerInside
        property string liveThumbUrl: photo ? photo.thumbUrl : ""
        property int pinnedFileId: 0
        property int pinnedImageId: 0
        property int pinnedMaxEdge: 0
        // In-place change: offset from the new cell to the old one.
        property real flipX: 0
        property real flipY: 0

        width: root.cellWidth
        height: root.photoRowHeight
        transform: Translate { x: cell.flipX; y: cell.flipY }

        function startFlip(dx, dy, fromOpacity) {
            flipAnimation.stop()
            flipX = dx
            flipY = dy
            opacity = fromOpacity
            flipAnimation.restart()
        }
        function finishFlip() {
            flipAnimation.stop()
            flipX = 0
            flipY = 0
            opacity = 1
        }
        ParallelAnimation {
            id: flipAnimation
            NumberAnimation {
                target: cell; property: "flipX"; to: 0
                duration: root.changeAnimationDuration; easing.type: Easing.OutCubic
            }
            NumberAnimation {
                target: cell; property: "flipY"; to: 0
                duration: root.changeAnimationDuration; easing.type: Easing.OutCubic
            }
            NumberAnimation {
                target: cell; property: "opacity"; to: 1
                duration: root.changeAnimationDuration; easing.type: Easing.OutCubic
            }
        }
        Accessible.role: Accessible.ListItem
        Accessible.name: {
            if (!photo) {
                return qsTr("Loading photo")
            }
            const parts = [photo.fileName,
                           rating > 0 ? qsTr("%n star(s)", "", rating) : qsTr("Unrated")]
            if (edited) {
                parts.push(qsTr("Edited"))
            }
            return parts.join(", ")
        }
        Accessible.selected: selected

        HoverHandler {
            id: cellHover
        }

        function releasePin() {
            if (pinnedFileId > 0 && pinnedImageId > 0) {
                root.library.SetOccurrenceThumbnailVisible(groupTitle, groupUnknown, pinnedFileId,
                                                           pinnedImageId, false, pinnedMaxEdge)
            }
            pinnedFileId = 0
            pinnedImageId = 0
            pinnedMaxEdge = 0
        }
        property bool alive: true
        function bindPin() {
            if (!alive) {
                return
            }
            const want = rowInUse && fileId > 0 && imageId > 0
            if (want && pinnedFileId === fileId && pinnedImageId === imageId
                    && pinnedMaxEdge === root.desiredMaxEdge) {
                return
            }
            releasePin()
            if (want) {
                pinnedFileId = fileId
                pinnedImageId = imageId
                pinnedMaxEdge = root.desiredMaxEdge
                root.library.SetOccurrenceThumbnailVisible(groupTitle, groupUnknown, fileId,
                                                           imageId, true, pinnedMaxEdge)
            }
        }
        // Pinning requests a thumbnail, which updates the thumbnail model at once; bind after
        // the current binding update so the cell does not re-enter its own photo binding.
        onFileIdChanged: Qt.callLater(cell.bindPin)
        onImageIdChanged: Qt.callLater(cell.bindPin)
        onRowInUseChanged: Qt.callLater(cell.bindPin)
        onPhotoChanged: liveThumbUrl = photo ? photo.thumbUrl : ""
        Component.onCompleted: Qt.callLater(cell.bindPin)
        Component.onDestruction: {
            alive = false
            releasePin()
        }
        Connections {
            target: root
            function onDesiredMaxEdgeChanged() { Qt.callLater(cell.bindPin) }
        }
        Connections {
            target: root.library
            function onThumbnailUpdated(elementId, url, loading, missingSource, errorText) {
                if (elementId === cell.fileId) {
                    cell.liveThumbUrl = url
                }
            }
        }

        Rectangle {
            // Above cellHit, so the star control takes its own clicks; the rest of the card
            // takes no pointer input and cellHit gets it.
            z: 1
            anchors.fill: parent
            anchors.margins: root.cellGap / 2
            radius: appTheme.panelRadius
            color: cell.hovered ? appTheme.hoverColor : appTheme.cardSurfaceColor
            // Monochrome selection: a neutral text-color outline, never an accent frame.
            border.width: cell.selected ? 2 : (cell.focused ? 1 : 0)
            border.color: cell.selected ? appTheme.textColor : appTheme.textMutedColor

            Rectangle {
                id: thumbWell
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: root.cardInset
                height: Math.max(32, parent.height - root.textAreaHeight - root.cardInset * 2)
                radius: appTheme.badgeRadius
                color: appTheme.bgBaseColor
                clip: true

                Image {
                    id: cellImage
                    anchors.fill: parent
                    anchors.margins: 2
                    source: cell.liveThumbUrl
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                    visible: cell.liveThumbUrl.length > 0
                }

                // Edit mark: a chip on the top-right corner of the photo.
                ThumbnailEditMark {
                    image: cellImage
                    compact: root.compactText
                    edited: cell.edited
                    editedAt: cell.photo ? cell.photo.editedAt : undefined
                    versionName: cell.photo && cell.photo.editVersionName
                                 ? String(cell.photo.editVersionName) : ""
                }
            }

            Item {
                id: captionArea
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: root.cardInset
                height: root.textAreaHeight
                visible: root.textAreaHeight > 0

                // File-name line: the name and (8 and 11 columns) the filled stars. The file
                // name elides first.
                Item {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 18

                    Label {
                        id: cellName
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, Math.max(0, parent.width
                                        - (nameTrailing.width > 0 ? nameTrailing.width + 4 : 0)))
                        text: cell.photo ? cell.photo.fileName : ""
                        color: appTheme.textColor
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        elide: Text.ElideRight
                    }

                    Row {
                        id: nameTrailing
                        anchors.left: cellName.right
                        anchors.leftMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        ThumbnailStarRating {
                            objectName: "albumSectionCompactStars"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: root.compactText && cell.rating > 0
                            photoId: cell.fileId
                            rating: cell.rating
                            interactive: false
                        }
                    }
                }

                // Date line (2 to 6 columns): the capture date and the star control.
                Item {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 18
                    visible: !root.compactText

                    Label {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, cellStars.x - 4)
                        text: cell.photo && cell.photo.captureDate
                              ? String(cell.photo.captureDate) : ""
                        color: appTheme.textMutedColor
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        elide: Text.ElideRight
                    }

                    ThumbnailStarRating {
                        id: cellStars
                        objectName: "albumSectionStars"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        photoId: cell.fileId
                        rating: cell.rating
                        interactive: root.starsInteractive
                        handlesPointer: true
                        cardHovered: cell.hovered
                        pixelSize: appTheme.fontSizeBody
                        onRatingRequested: function(rating) {
                            const item = root.selectionItem(cell.photo)
                            if (item) {
                                root.ratingRequested(item, rating)
                            }
                        }
                    }
                }
            }
        }

        MouseArea {
            id: cellHit
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: cell.photo ? Qt.PointingHandCursor : Qt.ArrowCursor
            onPressed: function(mouse) {
                root.forceActiveFocus()
                const item = root.selectionItem(cell.photo)
                if (!item) {
                    return
                }
                if (mouse.button === Qt.RightButton) {
                    root.imageFocused(item, root.photos.rowByElementId(item.elementId))
                    const scenePoint = cellHit.mapToItem(null, mouse.x, mouse.y)
                    root.contextMenuRequested(item, scenePoint.x, scenePoint.y)
                }
            }
            onClicked: function(mouse) {
                if (mouse.button !== Qt.LeftButton) {
                    return
                }
                const item = root.selectionItem(cell.photo)
                if (!item) {
                    return
                }
                root.focusOccurrence = cell.occurrence
                root.imageFocused(item, root.photos.rowByElementId(item.elementId))
                if (ShortcutRegistry.modifierMatches("library.extendSelection", mouse.modifiers)
                        && root.anchorOccurrence >= 0) {
                    root.selectRange(root.anchorOccurrence, cell.occurrence,
                                     ShortcutRegistry.modifierMatches("library.toggleSelection",
                                                                      mouse.modifiers))
                } else if (ShortcutRegistry.modifierMatches("library.toggleSelection",
                                                            mouse.modifiers)) {
                    root.imageSelectionChanged(item.elementId, item.imageId, item.fileName,
                                               item.isHdr === true, !cell.selected)
                    root.anchorOccurrence = cell.occurrence
                } else {
                    root.replaceSelection([item])
                    root.anchorOccurrence = cell.occurrence
                }
            }
            onDoubleClicked: function(mouse) {
                const item = root.selectionItem(cell.photo)
                if (item && mouse.button === Qt.LeftButton) {
                    appModules.workspaceRouter.openEditor(item.elementId, item.imageId)
                }
            }
            onWheel: function(wheel) {
                if (wheel.modifiers & Qt.ControlModifier) {
                    const delta = wheel.angleDelta.y > 0 ? 1 : -1
                    root.zoomChanged(Math.max(0, Math.min(root.zoomColumns.length - 1,
                                                          root.zoomLevel + delta)))
                    wheel.accepted = true
                    return
                }
                wheel.accepted = false
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: appTheme.inspectorHeaderActionSize + appTheme.spaceSm
            spacing: appTheme.spaceXs

            Item { Layout.fillWidth: true }
            TextAction {
                objectName: "albumSectionExpandAll"
                text: qsTr("Expand all")
                accessibleText: qsTr("Expand all groups")
                onActivated: root.sections.ExpandAll()
            }
            TextAction {
                objectName: "albumSectionCollapseAll"
                text: qsTr("Collapse all")
                accessibleText: qsTr("Collapse all groups")
                onActivated: root.sections.CollapseAll()
            }
        }

        ListView {
            id: list
            objectName: "albumSectionList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.sections
            reuseItems: true
            cacheBuffer: Math.max(0, height)
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            onContentYChanged: {
                visibleRowsTimer.restart()
                root.trackScrollAnchor()
            }
            onHeightChanged: {
                visibleRowsTimer.restart()
                Qt.callLater(root.applyPendingScrollAnchor)
            }
            onCountChanged: {
                visibleRowsTimer.restart()
                Qt.callLater(root.applyPendingScrollAnchor)
            }

            delegate: Item {
                id: rowItem
                required property int index
                required property int rowKind
                required property int groupIndex
                required property string groupTitle
                required property bool groupUnknown
                required property var photoCount
                required property bool collapsed
                required property var firstOccurrence
                required property var occurrenceCount
                required property var fileIds
                property bool inUse: true
                // In-place change of a header row (photo rows move their cells instead).
                property real flipX: 0
                property real flipY: 0

                width: ListView.view ? ListView.view.width : 0
                height: rowKind === 0 ? root.headerHeight : root.photoRowHeight
                transform: Translate { x: rowItem.flipX; y: rowItem.flipY }

                ListView.onPooled: {
                    inUse = false
                    finishFlips()
                }
                ListView.onReused: inUse = true

                function cellCount() {
                    return rowKind === 1 ? cellRepeater.count : 0
                }
                function cellAt(column) {
                    return cellRepeater.itemAt(column)
                }
                function startFlip(dx, dy, fromOpacity) {
                    headerFlipAnimation.stop()
                    flipX = dx
                    flipY = dy
                    opacity = fromOpacity
                    headerFlipAnimation.restart()
                }
                function finishFlips() {
                    headerFlipAnimation.stop()
                    flipX = 0
                    flipY = 0
                    opacity = 1
                    for (let c = 0; c < cellRepeater.count; ++c) {
                        const cell = cellRepeater.itemAt(c)
                        if (cell) {
                            cell.finishFlip()
                        }
                    }
                }
                ParallelAnimation {
                    id: headerFlipAnimation
                    NumberAnimation {
                        target: rowItem; property: "flipX"; to: 0
                        duration: root.changeAnimationDuration; easing.type: Easing.OutCubic
                    }
                    NumberAnimation {
                        target: rowItem; property: "flipY"; to: 0
                        duration: root.changeAnimationDuration; easing.type: Easing.OutCubic
                    }
                    NumberAnimation {
                        target: rowItem; property: "opacity"; to: 1
                        duration: root.changeAnimationDuration; easing.type: Easing.OutCubic
                    }
                }

                // Header: separate title, count, and disclosure roles.
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceSm
                    anchors.rightMargin: appTheme.spaceSm
                    visible: rowItem.rowKind === 0
                    spacing: appTheme.spaceSm

                    Item {
                        id: disclosure
                        Layout.preferredWidth: appTheme.inspectorHeaderActionSize
                        Layout.preferredHeight: appTheme.inspectorHeaderActionSize
                        activeFocusOnTab: rowItem.rowKind === 0
                        Accessible.role: Accessible.Button
                        Accessible.name: rowItem.collapsed
                                         ? qsTr("Expand %1").arg(headerTitle.fullText)
                                         : qsTr("Collapse %1").arg(headerTitle.fullText)
                        Accessible.onPressAction: toggle()
                        function toggle() {
                            root.sections.SetGroupCollapsed(rowItem.groupIndex, !rowItem.collapsed)
                        }
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                    || event.key === Qt.Key_Enter) {
                                disclosure.toggle()
                                event.accepted = true
                            }
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: appTheme.badgeRadius
                            color: disclosureHit.containsMouse ? appTheme.buttonHoveredFillColor
                                                               : "transparent"
                            border.width: disclosure.activeFocus ? 1 : 0
                            border.color: appTheme.textMutedColor
                        }
                        Text {
                            anchors.centerIn: parent
                            text: rowItem.collapsed ? "▸" : "▾"
                            color: appTheme.textMutedColor
                            font.pixelSize: appTheme.fontSizeBody
                        }
                        MouseArea {
                            id: disclosureHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: disclosure.toggle()
                        }
                    }

                    Label {
                        id: headerTitle
                        readonly property string fullText:
                            root.groupTitleText(rowItem.groupTitle, rowItem.groupUnknown)
                        Layout.fillWidth: true
                        text: fullText
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        color: appTheme.textColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeTitle
                        font.weight: appTheme.fontWeightStrong
                        Accessible.role: Accessible.Heading
                        Accessible.name: fullText
                    }

                    Label {
                        text: qsTr("%n photo(s)", "", Number(rowItem.photoCount))
                        color: appTheme.textMutedColor
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }
                }

                // Photo row: at most columnCount cells.
                Row {
                    visible: rowItem.rowKind === 1
                    Repeater {
                        id: cellRepeater
                        model: rowItem.rowKind === 1 ? Number(rowItem.occurrenceCount) : 0
                        delegate: PhotoCell {
                            required property int index
                            occurrence: Number(rowItem.firstOccurrence) + index
                            fileId: rowItem.fileIds && index < rowItem.fileIds.length
                                    ? Number(rowItem.fileIds[index]) : 0
                            groupTitle: rowItem.groupTitle
                            groupUnknown: rowItem.groupUnknown
                            rowInUse: rowItem.inUse
                        }
                    }
                }
            }
        }
    }

    // Removed photos and headers of an in-place change fade out here, over the list, at their
    // old viewport positions.
    Item {
        id: ghostLayer
        objectName: "albumSectionChangeGhosts"
        // The column layout fills the view, so the list position is a view position.
        x: list.x
        y: list.y
        width: list.width
        height: list.height
        clip: true
        z: 2
    }

    Component {
        id: changeGhostComponent
        Rectangle {
            id: ghost
            property string source: ""
            property string title: ""
            radius: title.length > 0 ? 0 : appTheme.panelRadius
            color: title.length > 0 ? "transparent" : appTheme.cardSurfaceColor
            Image {
                anchors.fill: parent
                anchors.margins: root.cardInset
                visible: ghost.title.length === 0 && ghost.source.length > 0
                source: ghost.source
                fillMode: Image.PreserveAspectFit
            }
            Label {
                anchors.left: parent.left
                anchors.leftMargin: appTheme.spaceSm * 2 + appTheme.inspectorHeaderActionSize
                anchors.verticalCenter: parent.verticalCenter
                visible: ghost.title.length > 0
                text: ghost.title
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightStrong
            }
            NumberAnimation on opacity {
                from: 1
                to: 0
                duration: root.changeAnimationDuration
                easing.type: Easing.OutCubic
                onFinished: ghost.destroy()
            }
        }
    }

    Timer {
        id: visibleRowsTimer
        interval: 0
        repeat: false
        onTriggered: root.requestVisibleRows()
    }
}
