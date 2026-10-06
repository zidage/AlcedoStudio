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

    property var selectedImagesById: ({})
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
    readonly property int textAreaHeight: visualColumns >= 14 ? 0 : 18
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

    signal imageSelectionChanged(int elementId, int imageId, string fileName, bool isHdr,
                                 bool selected)
    signal replaceSelection(var items)
    signal imageFocused(var item, int index)
    signal contextMenuRequested(var item, real sceneX, real sceneY)
    signal zoomChanged(int zoomLevel)

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
        return Object.prototype.hasOwnProperty.call(selectedImagesById, String(Number(elementId)))
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

    function requestVisibleRows() {
        if (list.count <= 0 || list.height <= 0) {
            return
        }
        const first = sections.RowAtOffset(list.contentY, headerHeight, photoRowHeight)
        const last = sections.RowAtOffset(list.contentY + list.height, headerHeight,
                                          photoRowHeight)
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
        pendingSelectAdditive = false
        pendingSelectRequest = library.RequestAllFileIds()
    }

    Connections {
        target: root.library
        function onOrderedFileIdsReady(requestId, items) {
            if (requestId !== root.pendingSelectRequest) {
                return
            }
            root.pendingSelectRequest = 0
            if (root.pendingSelectAdditive) {
                root.replaceSelection(Object.values(root.selectedImagesById).concat(items))
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
    }

    Connections {
        target: root.photos
        function onModelReset() { root.photoRevision++ }
        function onRowsInserted() { root.photoRevision++ }
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
        property string liveThumbUrl: photo ? photo.thumbUrl : ""
        property int pinnedFileId: 0
        property int pinnedImageId: 0
        property int pinnedMaxEdge: 0

        width: root.cellWidth
        height: root.photoRowHeight
        Accessible.role: Accessible.ListItem
        Accessible.name: photo ? photo.fileName : qsTr("Loading photo")
        Accessible.selected: selected

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
            anchors.fill: parent
            anchors.margins: root.cellGap / 2
            radius: appTheme.panelRadius
            color: cellHit.containsMouse ? appTheme.hoverColor : appTheme.cardSurfaceColor
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
                    anchors.fill: parent
                    anchors.margins: 2
                    source: cell.liveThumbUrl
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                    visible: cell.liveThumbUrl.length > 0
                }
            }

            Label {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: root.cardInset
                height: root.textAreaHeight
                visible: root.textAreaHeight > 0
                text: cell.photo ? cell.photo.fileName : ""
                color: appTheme.textColor
                font.family: appTheme.dataFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
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

                width: ListView.view ? ListView.view.width : 0
                height: rowKind === 0 ? root.headerHeight : root.photoRowHeight

                ListView.onPooled: inUse = false
                ListView.onReused: inUse = true

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

    Timer {
        id: visibleRowsTimer
        interval: 0
        repeat: false
        onTriggered: root.requestVisibleRows()
    }
}
