import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Controls.impl
import QtQuick.Dialogs
import QtQuick.Layouts
import "CollectionTree.js" as CollectionTree
import "FileDrop.js" as FileDrop

// Collections sidebar: wordmark, utility tabs, All Photos, and one tree with two sections
// (#1078). Collections: the Library row (the library root) and the ordinary collections, which
// the user arranges (expand/collapse, inline create and rename, drag to move, keyboard). Disk:
// the mounted folders grouped by volume, which follow the disk.
ColumnLayout {
    id: panel
    objectName: "collectionsPanel"

    property var folderController
    property var theme
    property var host: null
    property bool backendInteractive: false
    property bool sortDescending: false
    property string activeUtilityTab: "search"
    property var maintenanceController: null
    readonly property var utilityTabs: [
        {
            tabId: "search",
            label: qsTr("Search"),
            iconSource: "qrc:/panel_icons/search.svg"
        },
        {
            tabId: "background-tasks",
            label: qsTr("Background Tasks"),
            iconSource: "qrc:/panel_icons/clock-play.svg"
        },
        {
            tabId: "maintenance",
            label: qsTr("Project Maintenance"),
            iconSource: "qrc:/panel_icons/database-cog.svg"
        }
    ]
    signal importRequested()
    // Import Folder (#1078). @p action: "" for Import Folder... of the Import menu, "mount" for
    // the + of the Disk section, "import" or "mount" for the buttons of a volume card.
    // @p folderUrl: the folder (DCIM of a card), or empty to ask with the system folder dialog,
    // which opens at @p startFolderUrl.
    signal importFolderRequested(string action, url folderUrl, url startFolderUrl)
    // Local files and folders dropped on the row of folderId (issue #757).
    signal filesDropped(var urls, int folderId)
    signal searchRequested()
    signal backgroundTasksRequested()
    signal projectMaintenanceRequested()

    readonly property var foldersModule: folderController
    readonly property int currentFolderId: foldersModule ? Number(foldersModule.currentFolderId) : 0
    // With currentFolderId 0: the Library row is selected rather than All Photos.
    readonly property bool libraryRootSelected: !!foldersModule
                                                && foldersModule.libraryRootSelected === true
    readonly property bool allPhotosSelected: currentFolderId === 0 && !libraryRootSelected
    // The card of a volume that arrived with no mount (#1076): {name, mountPoint, hasDcim,
    // rootUrl, dcimUrl}; empty when there is none.
    readonly property var arrivedVolume: foldersModule && foldersModule.arrivedVolume
                                         ? foldersModule.arrivedVolume : ({})
    readonly property bool arrivedVolumeShown: !!arrivedVolume && arrivedVolume.rootUrl !== undefined
                                               && String(arrivedVolume.rootUrl).length > 0
    onArrivedVolumeShownChanged: rebuildFolderRows()

    // Display rows: the Collections section header, the Library row and the ordinary collections
    // in tree order (CollectionTree.orderRows) with one draft row while a new collection is
    // named, then the Disk section header, the card of an arrived volume, and the volumes with
    // their mounts (CollectionTree.diskRows). A row with a rowKind ("section", "volume",
    // "volumeCard") is not a folder. Every row stays in the model; a collapsed branch hides its
    // rows.
    property var folderRows: []
    // elementId -> true for expanded folders. Element ids survive the tree reload that a create,
    // rename, or move causes; ui folder ids do not.
    property var expandedIds: ({})
    property var rowShown: []
    property string rowsSignature: ""
    // folderId -> photo count (-1 before the first count read). Kept apart from folderRows: a
    // count update after an import or a delete changes only the count labels, so it does not
    // reset the list or close an open inline editor.
    property var imageCounts: ({})

    // Inline editors. -1 means "none".
    property int draftParentFolderId: -1
    property int renamingElementId: -1
    property int pendingDeleteElementId: -1

    // Drag to move.
    property var dragSourceRow: null
    property bool dragActive: false
    property int dropTargetFolderId: -1
    property point dragPoint: Qt.point(0, 0)

    // File drop from the system file manager (issue #757). The host enables it while an
    // import may start; fileDropFolderId is the row under the drag, -1 when none takes it.
    property bool fileDropEnabled: false
    property int fileDropFolderId: -1

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    // Tree row geometry (#1028): a fixed chevron column and a fixed icon column, so the names
    // of leaf rows and parent rows align. The indent guide of a level runs through the centre
    // of the chevron column of that level.
    readonly property int treeRowInset: appTheme.spaceXs
    readonly property int treeChevronSize: 16
    readonly property int treeIconSize: 16
    // Width of a tree row: the tree width less the column of the scroll bar while the tree
    // scrolls.
    readonly property real treeRowWidth: folderList.contentHeight > folderList.height + 0.5
                                         ? Math.max(0, folderList.width - treeScrollBar.width)
                                         : folderList.width

    function rowIndent(row) {
        return Math.max(0, Number(row.treeDepth || 0)) * appTheme.collectionsTreeIndentStep
    }

    // True for a folder row (the Library row, a collection, a mounted folder); false for the
    // draft row, the section headers, the volume rows, and the volume card.
    function isFolderRow(row) {
        return !!row && !row.isDraft && !row.rowKind
    }

    function isRootRow(row) {
        return isFolderRow(row) && Number(row.folderId) === 0
    }

    // True when the tree pointer is over @p item (a control inside a row): the hover state of
    // the controls of the rows that are not folders, below the tree pointer handler.
    function treePointerOver(item, rowIndex) {
        if (!item || treeMouse.hoverIndex !== rowIndex) {
            return false
        }
        const point = item.mapFromItem(folderList, treeMouse.hoverPoint.x, treeMouse.hoverPoint.y)
        return point.x >= 0 && point.y >= 0 && point.x <= item.width && point.y <= item.height
    }

    // The x of the indent guide of tree level @p level (1 = top level), in row coordinates.
    function guideX(level) {
        return treeRowInset + (level - 1) * appTheme.collectionsTreeIndentStep
               + treeChevronSize / 2
    }

    // folderId of the collection whose indent guide is bright: the selected collection when it
    // is open, otherwise the collection that holds it. -1 when no guide is bright.
    readonly property int activeGuideFolderId: {
        const row = allPhotosSelected ? null : rowForFolder(currentFolderId)
        if (!row) {
            return -1
        }
        if (row.hasChildren === true && isExpanded(row)) {
            return Number(row.folderId)
        }
        if (Number(row.folderId) === 0 || row.mountRoot === true) {
            return -1
        }
        return Number(row.parentFolderId)
    }

    // The selected folder row; null while All Photos is selected.
    readonly property var currentRow: allPhotosSelected ? null : rowForFolder(currentFolderId)

    function indexOfFolder(folderId) {
        for (let i = 0; i < folderRows.length; ++i) {
            if (isFolderRow(folderRows[i]) && Number(folderRows[i].folderId) === Number(folderId)) {
                return i
            }
        }
        return -1
    }

    function indexOfElement(elementId) {
        for (let i = 0; i < folderRows.length; ++i) {
            if (isFolderRow(folderRows[i])
                    && Number(folderRows[i].elementId) === Number(elementId)) {
                return i
            }
        }
        return -1
    }

    function folderRowsOnly() {
        return folderRows.filter(function(row) { return isFolderRow(row) })
    }

    function rowForFolder(folderId) {
        const index = indexOfFolder(folderId)
        return index >= 0 ? folderRows[index] : null
    }

    function isExpanded(row) {
        if (!row || row.isDraft) {
            return false
        }
        if (row.rowKind === "volume") {
            return true
        }
        if (row.rowKind) {
            return false
        }
        if (Number(row.folderId) === 0) {
            return libraryExpanded
        }
        return expandedIds[String(Number(row.elementId))] === true
    }

    function setExpanded(elementId, expanded) {
        const key = String(Number(elementId))
        if ((expandedIds[key] === true) === expanded) {
            return
        }
        const next = Object.assign({}, expandedIds)
        if (expanded) {
            next[key] = true
        } else {
            delete next[key]
        }
        expandedIds = next
        recomputeShownRows()
    }

    function toggleExpanded(row) {
        if (!isFolderRow(row) || !row.hasChildren) {
            return
        }
        if (Number(row.folderId) === 0) {
            libraryExpanded = !libraryExpanded
            return
        }
        setExpanded(row.elementId, !isExpanded(row))
    }

    function recomputeShownRows() {
        const byFolderId = CollectionTree.rowsByFolderId(folderRowsOnly())
        const shown = []
        const ancestors = []
        let section = ""
        for (let i = 0; i < folderRows.length; ++i) {
            const row = folderRows[i]
            if (row.rowKind === "section") {
                section = String(row.section)
                shown.push(true)
                ancestors.push([])
                continue
            }
            const disk = section === "disk"
            let visible = disk ? diskSectionOpen : collectionsSectionOpen
            if (row.rowKind) {
                // A volume row and the volume card.
                shown.push(visible)
                ancestors.push([])
                continue
            }
            // The folders above the row, outermost first: the Library row (folderId 0) for a
            // collection; a volume marker (-1) and the mounted folders for a mounted folder.
            const chain = []
            let parentId = Number(row.folderId) === 0 ? -1 : Number(row.parentFolderId)
            let guard = 0
            while (parentId >= 0 && guard < 4096) {
                const parent = byFolderId[String(Number(parentId))]
                if (!parent || (disk && Number(parent.folderId) === 0)) {
                    break
                }
                chain.unshift(Number(parent.folderId))
                visible = visible && isExpanded(parent)
                if (Number(parent.folderId) === 0) {
                    break
                }
                parentId = Number(parent.parentFolderId)
                ++guard
            }
            if (disk) {
                chain.unshift(-1)
            }
            shown.push(visible)
            ancestors.push(chain)
        }
        rowAncestors = ancestors
        rowShown = shown
    }

    // Each section header folds its section; All Photos stays.
    property bool collectionsSectionOpen: true
    property bool diskSectionOpen: true
    // The Library row starts expanded.
    property bool libraryExpanded: true
    onCollectionsSectionOpenChanged: recomputeShownRows()
    onDiskSectionOpenChanged: recomputeShownRows()
    onLibraryExpandedChanged: recomputeShownRows()
    // folderIds of the folders above each row of folderRows, outermost first (-1 for the volume
    // of a mounted folder).
    property var rowAncestors: []

    // Expands every ancestor of the folder so its row is shown, then scrolls to it.
    function revealFolder(folderId) {
        const byFolderId = CollectionTree.rowsByFolderId(folderRowsOnly())
        let current = byFolderId[String(Number(folderId))]
        if (!current) {
            return
        }
        if (Number(folderId) === currentFolderId) {
            revealedElementId = Number(current.elementId)
        }
        // All Photos is the row above the tree: the tree does not move for it.
        if (Number(folderId) === 0 && !libraryRootSelected) {
            return
        }
        const mounted = Number(current.mountId || 0) > 0
        const next = Object.assign({}, expandedIds)
        let parent = byFolderId[String(Number(current.parentFolderId))]
        let guard = 0
        while (Number(current.folderId) !== 0 && parent && guard < 4096) {
            if (Number(parent.folderId) === 0) {
                if (!mounted) {
                    libraryExpanded = true
                }
                break
            }
            next[String(Number(parent.elementId))] = true
            parent = byFolderId[String(Number(parent.parentFolderId))]
            ++guard
        }
        expandedIds = next
        if (mounted) {
            diskSectionOpen = true
        } else {
            collectionsSectionOpen = true
        }
        recomputeShownRows()
        const index = indexOfFolder(folderId)
        if (index >= 0) {
            folderList.forceLayout()
            folderList.positionViewAtIndex(index, ListView.Contain)
        }
    }

    function imageCountOf(row) {
        if (!isFolderRow(row)) {
            return -1
        }
        const count = imageCounts[String(Number(row.folderId))]
        return count === undefined ? -1 : Number(count)
    }

    function rebuildFolderRows() {
        const source = sourceFolders
        if (!source || source.length === 0) {
            expandedIds = ({})
        }
        const counts = {}
        for (let i = 0; i < (source ? source.length : 0); ++i) {
            const count = source[i].imageCount
            counts[String(Number(source[i].folderId))] = count === undefined ? -1 : Number(count)
        }
        imageCounts = counts
        // Collections: the Library row and the ordinary collections. A mount is never below an
        // ordinary collection (#1076), so the result is one connected tree.
        const ordered = CollectionTree.orderRows(CollectionTree.ordinaryRows(source),
                                                 sortDescending)
        let rows = ordered
        if (draftParentFolderId >= 0) {
            let parentIndex = -1
            for (let i = 0; i < ordered.length; ++i) {
                if (Number(ordered[i].folderId) === draftParentFolderId) {
                    parentIndex = i
                    break
                }
            }
            if (parentIndex < 0) {
                draftParentFolderId = -1
            } else {
                const parentRow = ordered[parentIndex]
                let insertAt = parentIndex + 1
                while (insertAt < ordered.length
                       && Number(ordered[insertAt].treeDepth) > Number(parentRow.treeDepth)) {
                    ++insertAt
                }
                rows = ordered.slice(0, insertAt)
                        .concat([{
                            isDraft: true,
                            folderId: -1,
                            elementId: -1,
                            parentFolderId: draftParentFolderId,
                            treeDepth: Number(parentRow.treeDepth) + 1,
                            hasChildren: false,
                            name: ""
                        }])
                        .concat(ordered.slice(insertAt))
            }
        }
        if (rows.length > 0) {
            const disk = [{ rowKind: "section", section: "disk", treeDepth: 0 }]
            if (arrivedVolumeShown) {
                disk.push({ rowKind: "volumeCard", treeDepth: 0 })
            }
            rows = [{ rowKind: "section", section: "collections", treeDepth: 0 }]
                    .concat(rows, disk, CollectionTree.diskRows(source, sortDescending))
        }

        const signature = rows.map(function(row) {
            return [row.isDraft ? "draft" : (row.rowKind || ""), row.folderId, row.elementId,
                    row.parentFolderId, row.name, row.mountId, row.mountOffline,
                    row.mountMissingCount, row.volumeKey, row.volumeName, row.mountPoint,
                    row.offline].join(":")
        }).join("|")
        if (signature !== rowsSignature) {
            // A new model resets the ListView. The row at the top of the view is kept: it is
            // positioned again after the new rows and their shown state are laid out.
            const anchor = treeScrollAnchor()
            rowsSignature = signature
            folderRows = rows
            recomputeShownRows()
            restoreTreeScrollAnchor(anchor)
        } else {
            recomputeShownRows()
        }
    }

    // The shown row at the top of the tree and the part of it above the view. A raw contentY
    // does not survive a reset: the ListView estimates the positions of rows it has not
    // created, and the hidden rows have height 0.
    function treeScrollAnchor() {
        const index = folderList.indexAt(1, folderList.contentY + 0.5)
        const item = index >= 0 ? folderList.itemAtIndex(index) : null
        return {
            elementId: item && isFolderRow(folderRows[index]) ? Number(folderRows[index].elementId)
                                                              : -1,
            offset: item ? folderList.contentY - item.y : 0,
            y: folderList.contentY - folderList.originY
        }
    }

    function restoreTreeScrollAnchor(anchor) {
        folderList.forceLayout()
        const index = anchor.elementId >= 0 ? indexOfElement(anchor.elementId) : -1
        if (index < 0 || rowShown[index] !== true) {
            // The top row is gone or hidden: the view keeps its distance from the top, within
            // the height of the shown rows.
            let shown = 0
            for (let i = 0; i < rowShown.length; ++i) {
                shown += rowShown[i] === true ? 1 : 0
            }
            const maxY = Math.max(0, shown * appTheme.collectionsTreeRowHeight - folderList.height)
            folderList.contentY = folderList.originY + Math.max(0, Math.min(anchor.y, maxY))
            return
        }
        // At the end of the tree the view stops at the boundary and takes no offset.
        folderList.positionViewAtIndex(index, ListView.Beginning)
        const item = folderList.itemAtIndex(index)
        if (item && Math.abs(folderList.contentY - item.y) < 0.5) {
            folderList.contentY = item.y + anchor.offset
        }
    }

    // Indices of the shown folder rows, top to bottom: the Library row first.
    function shownFolderIndices() {
        const out = []
        for (let i = 0; i < folderRows.length; ++i) {
            if (rowShown[i] === true && isFolderRow(folderRows[i])) {
                out.push(i)
            }
        }
        return out
    }

    function selectRow(row) {
        if (!isFolderRow(row) || !foldersModule || !backendInteractive) {
            return
        }
        if (Number(row.folderId) === 0) {
            if (!libraryRootSelected) {
                foldersModule.SelectLibraryRoot()
            }
        } else if (Number(row.folderId) !== currentFolderId) {
            foldersModule.SelectFolder(Number(row.folderId))
        }
    }

    function moveSelection(step) {
        const shown = shownFolderIndices()
        const currentIndex = currentRow ? indexOfFolder(currentFolderId) : -1
        let position = shown.indexOf(currentIndex)
        if (position < 0) {
            // From All Photos, Down enters the tree at the Library row.
            if (step < 0) {
                return
            }
            position = 0
        } else {
            position = Math.max(0, Math.min(shown.length - 1, position + step))
        }
        if (shown.length > 0) {
            const index = shown[position]
            selectRow(folderRows[index])
            folderList.positionViewAtIndex(index, ListView.Contain)
        }
    }

    function handleTreeKey(event) {
        if (renamingElementId >= 0 || draftParentFolderId >= 0) {
            return false
        }
        const row = currentRow
        switch (event.key) {
        case Qt.Key_Up:
            moveSelection(-1)
            return true
        case Qt.Key_Down:
            moveSelection(1)
            return true
        case Qt.Key_Left:
            if (row && row.hasChildren && isExpanded(row)) {
                toggleExpanded(row)
            } else if (row && Number(row.folderId) !== 0 && row.mountRoot !== true) {
                // A mount root stands below its volume, not below the Library row.
                selectRow(rowForFolder(row.parentFolderId))
            }
            return true
        case Qt.Key_Right:
            if (row && row.hasChildren && !isExpanded(row)) {
                toggleExpanded(row)
            } else if (row && row.hasChildren) {
                const index = indexOfFolder(row.folderId)
                if (index + 1 < folderRows.length) {
                    selectRow(folderRows[index + 1])
                }
            }
            return true
        case Qt.Key_F2:
            beginRename(row)
            return true
        case Qt.Key_Delete:
        case Qt.Key_Backspace:
            requestDelete(row)
            return true
        }
        return false
    }

    // ── Create ──────────────────────────────────────────────────────────────

    function beginCreateCollection(parentFolderId) {
        if (!backendInteractive) {
            return
        }
        cancelRename()
        const parentRow = rowForFolder(parentFolderId)
        if (!parentRow || !structureEditable(parentRow)) {
            return
        }
        // Open the parent and every folder above it, so the draft row is in view.
        const byFolderId = CollectionTree.rowsByFolderId(folderRowsOnly())
        const next = Object.assign({}, expandedIds)
        let current = parentRow
        let guard = 0
        while (current && Number(current.folderId) !== 0 && guard < 4096) {
            next[String(Number(current.elementId))] = true
            current = byFolderId[String(Number(current.parentFolderId))]
            ++guard
        }
        expandedIds = next
        libraryExpanded = true
        collectionsSectionOpen = true
        draftParentFolderId = Number(parentFolderId)
        rebuildFolderRows()
        for (let i = 0; i < folderRows.length; ++i) {
            if (folderRows[i].isDraft) {
                folderList.positionViewAtIndex(i, ListView.Contain)
                break
            }
        }
    }

    function cancelDraftCollection() {
        if (draftParentFolderId < 0) {
            return
        }
        draftParentFolderId = -1
        rebuildFolderRows()
    }

    function commitDraftCollection(text) {
        if (draftParentFolderId < 0) {
            return
        }
        const parentFolderId = draftParentFolderId
        const trimmed = String(text || "").trim()
        // A tree reload reassigns ui ids, so the draft ends first.
        draftParentFolderId = -1
        if (trimmed.length === 0 || !foldersModule) {
            rebuildFolderRows()
            return
        }
        const createdElementId = Number(foldersModule.CreateFolderIn(parentFolderId, trimmed))
        rebuildFolderRows()
        if (createdElementId > 0) {
            // The row of the new collection comes with the next tree read.
            pendingRevealElementId = createdElementId
            revealPendingElement()
        }
    }

    // elementId of a new collection whose row is revealed when it arrives; -1 when none.
    property int pendingRevealElementId: -1

    function revealPendingElement() {
        if (pendingRevealElementId <= 0) {
            return
        }
        const index = indexOfElement(pendingRevealElementId)
        if (index >= 0) {
            pendingRevealElementId = -1
            revealFolder(folderRows[index].folderId)
        }
    }

    // ── Rename ──────────────────────────────────────────────────────────────

    // A mounted row takes no structure change: its tree follows its disk directory.
    function structureEditable(row) {
        return isFolderRow(row) && row.structureEditable !== false
    }

    function beginRename(row) {
        if (!row || row.isDraft || Number(row.folderId) === 0 || !backendInteractive
                || !structureEditable(row)) {
            return
        }
        cancelDraftCollection()
        renamingElementId = Number(row.elementId)
    }

    function cancelRename() {
        renamingElementId = -1
        folderList.forceActiveFocus()
    }

    function commitRename(text) {
        const index = indexOfElement(renamingElementId)
        renamingElementId = -1
        folderList.forceActiveFocus()
        if (index < 0 || !foldersModule) {
            return
        }
        const row = folderRows[index]
        const trimmed = String(text || "").trim()
        if (trimmed.length === 0 || trimmed === String(row.name)) {
            return
        }
        const elementId = Number(row.elementId)
        // The tree reloads inside RenameFolder; the renamed row may move in the sort order.
        if (foldersModule.RenameFolder(Number(row.folderId), trimmed)) {
            const renamedIndex = indexOfElement(elementId)
            if (renamedIndex >= 0) {
                revealFolder(folderRows[renamedIndex].folderId)
            }
        }
    }

    // ── Delete ──────────────────────────────────────────────────────────────

    function requestDelete(row) {
        if (!row || row.isDraft || Number(row.folderId) === 0 || !backendInteractive
                || !structureEditable(row)) {
            return
        }
        pendingDeleteElementId = Number(row.elementId)
        deleteDialog.openWith(row.hasChildren
            ? qsTr("Delete the collection \"%1\" and the collections inside it? The images stay in the project.").arg(row.name)
            : qsTr("Delete the collection \"%1\"? The images stay in the project.").arg(row.name))
    }

    function confirmDelete() {
        const index = indexOfElement(pendingDeleteElementId)
        pendingDeleteElementId = -1
        if (index >= 0 && foldersModule) {
            foldersModule.DeleteFolder(Number(folderRows[index].folderId))
        }
    }

    // ── Drag to move ────────────────────────────────────────────────────────

    function canDropOn(sourceRow, targetRow) {
        if (!sourceRow || !targetRow || !structureEditable(sourceRow)
                || !structureEditable(targetRow)) {
            return false
        }
        if (Number(targetRow.folderId) === Number(sourceRow.parentFolderId)) {
            return false
        }
        const byFolderId = CollectionTree.rowsByFolderId(folderRowsOnly())
        return !CollectionTree.isAtOrBelow(byFolderId, targetRow.folderId, sourceRow.folderId)
    }

    // The row at a point of the tree view.
    function rowAtListPoint(listX, listY) {
        if (listY < 0 || listY > folderList.height) {
            return null
        }
        const index = folderList.indexAt(listX, listY + folderList.contentY)
        return index >= 0 ? folderRows[index] : null
    }

    function updateDropTarget(listX, listY) {
        const target = rowAtListPoint(listX, listY)
        dropTargetFolderId = canDropOn(dragSourceRow, target) ? Number(target.folderId) : -1
    }

    function finishDrag() {
        const source = dragSourceRow
        const targetFolderId = dropTargetFolderId
        dragActive = false
        dragSourceRow = null
        dropTargetFolderId = -1
        if (!source || targetFolderId < 0 || !foldersModule) {
            return
        }
        const target = rowForFolder(targetFolderId)
        const sourceElementId = Number(source.elementId)
        if (foldersModule.MoveFolder(Number(source.folderId), targetFolderId)) {
            if (target && Number(target.folderId) !== 0) {
                setExpanded(target.elementId, true)
            }
            const index = indexOfElement(sourceElementId)
            if (index >= 0) {
                revealFolder(folderRows[index].folderId)
            }
        }
    }

    function cancelDrag() {
        dragActive = false
        dragSourceRow = null
        dropTargetFolderId = -1
    }

    // ── File drop to import ─────────────────────────────────────────────────

    // A collection row takes a file drop; the draft row and the row being renamed do not.
    // A mounted row takes its files from its disk directory, so it takes no file drop.
    function canTakeFileDrop(row) {
        return fileDropEnabled && isFolderRow(row)
               && Number(row.elementId) !== renamingElementId && structureEditable(row)
    }

    // ── Mounted folders ─────────────────────────────────────────────────────

    property int pendingLocateFolderId: -1
    property int pendingRemoveMissingFolderId: -1

    function openRowMenu(row) {
        menuRowElementId = Number(row.elementId)
        if (Number(row.mountId) > 0) {
            const menu = row.mountRoot === true ? mountRootMenu : mountedFolderMenu
            menu.targetRow = row
            menu.popup()
            return
        }
        rowMenu.targetRow = row
        rowMenu.popup()
    }


    function requestLocateFolder(row) {
        if (!row || row.mountRoot !== true || !backendInteractive) {
            return
        }
        pendingLocateFolderId = Number(row.folderId)
        locateFolderDialog.open()
    }

    function requestRemoveMissingPhotos(row) {
        if (!row || row.mountRoot !== true || Number(row.mountMissingCount) <= 0
                || !backendInteractive) {
            return
        }
        pendingRemoveMissingFolderId = Number(row.folderId)
        removeMissingDialog.openWith(
            qsTr("Remove %1 missing photos of \"%2\" from the project? Their edit history and Versions are removed.")
                .arg(Number(row.mountMissingCount)).arg(row.name))
    }

    function updateFileDropTarget(listX, listY) {
        const index = folderList.indexAt(listX, listY + folderList.contentY)
        const target = index >= 0 ? folderRows[index] : null
        fileDropFolderId = canTakeFileDrop(target) ? Number(target.folderId) : -1
        return fileDropFolderId >= 0
    }

    // The shell sets the width: the user drags the sidebar edge (#1028).
    property real panelWidth: appTheme.collectionsSidebarWidth
    // True while the folded sidebar shows over the workspace: a hairline outline separates it.
    property bool overlay: false
    width: panelWidth
    implicitWidth: panelWidth
    Layout.fillHeight: true
    spacing: 0
    clip: true

    // True while a menu or a dialog of the panel is open. The overlay sidebar of the folded
    // Library stays while it is true, also when the pointer is on the menu.
    readonly property bool popupOpen: rowMenu.visible || mountRootMenu.visible
                                      || mountedFolderMenu.visible || importMenu.visible
                                      || deleteDialog.visible || removeMissingDialog.visible
    readonly property bool hovered: panelHover.hovered
    // True while an inline editor or a drag is in progress.
    readonly property bool busy: dragActive || renamingElementId >= 0 || draftParentFolderId >= 0

    HoverHandler {
        id: panelHover
    }

    // Opens the import menu above @p anchorItem (the Import button, or the Import button of the
    // rail of the folded sidebar).
    function openImportMenu(anchorItem) {
        const target = anchorItem || importButton
        importMenu.parent = target
        importMenu.minimumWidth = target === importButton ? importButton.width : 0
        importMenu.x = target === importButton ? 0 : target.width + appTheme.spaceXs
        importMenu.y = target === importButton
                ? -importMenu.implicitHeight - appTheme.spaceXs
                : target.height - importMenu.implicitHeight
        importMenu.open()
    }

    // The tree scrolls only after a user action that targets a row: a selection (from the tree
    // or from outside it), New Collection, a committed rename, a drag move, and the first rows
    // of the panel. A reload the user did not start (mount scan, watcher, count refresh,
    // import, delete) keeps the scroll position (issue #1007).
    property bool revealOnNextRows: true

    Component.onCompleted: {
        rebuildFolderRows()
        if (folderRows.length > 0) {
            revealOnNextRows = false
            revealFolder(currentFolderId)
        }
    }
    onSortDescendingChanged: rebuildFolderRows()
    // React to the controller properties (NOTIFY FoldersChanged / FolderSelectionChanged).
    // A Connections handler named after those PascalCase signals does not reach them.
    readonly property var sourceFolders: foldersModule && foldersModule.folders
                                         ? foldersModule.folders : []
    onSourceFoldersChanged: {
        rebuildFolderRows()
        if (folderRows.length === 0) {
            // A closed project: the next project's rows are a first show again.
            revealOnNextRows = true
            revealedElementId = -1
            pendingRevealElementId = -1
        } else if (revealOnNextRows) {
            revealOnNextRows = false
            revealFolder(currentFolderId)
        }
        revealPendingElement()
    }
    // A reload can give the selected folder a new ui id; the tree scrolls only when the selected
    // folder itself changes.
    property int revealedElementId: -1
    onCurrentFolderIdChanged: {
        rebuildFolderRows()
        const row = rowForFolder(currentFolderId)
        const elementId = row ? Number(row.elementId) : -1
        if (elementId !== revealedElementId) {
            revealFolder(currentFolderId)
        }
    }
    onFileDropEnabledChanged: {
        if (!fileDropEnabled) {
            fileDropFolderId = -1
        }
    }
    onBackendInteractiveChanged: {
        if (!backendInteractive) {
            cancelDrag()
            renamingElementId = -1
            cancelDraftCollection()
        }
    }

    // One sidebar surface (#1028): wordmark, navigation rows, the Collections section with its
    // tree, and the Import button at the foot.
    Rectangle {
        id: collectionsSurface
        objectName: "collectionsSurface"
        Layout.fillWidth: true
        Layout.fillHeight: true
        radius: theme.panelRadius
        color: theme.colBgPanel
        border.width: panel.overlay ? 1 : 0
        border.color: appTheme.glassStrokeColor
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: appTheme.spaceSm
            anchors.rightMargin: appTheme.spaceSm
            anchors.topMargin: appTheme.spaceXs
            anchors.bottomMargin: appTheme.spaceSm
            spacing: 0

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 44

                Row {
                    id: identityCard
                    objectName: "collectionsIdentityCard"
                    anchors.left: parent.left
                    anchors.leftMargin: panel.treeRowInset
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 0
                    Label {
                        text: qsTr("Alcedo")
                        font.family: theme ? theme.headlineFontFamily : appTheme.headlineFontFamily
                        font.pixelSize: 17
                        font.weight: 700
                        color: theme ? theme.colAccentPrimary : appTheme.accentColor
                    }
                    Label {
                        text: " "
                        font.family: theme ? theme.headlineFontFamily : appTheme.headlineFontFamily
                        font.pixelSize: 17
                        font.weight: 700
                    }
                    Label {
                        text: qsTr("Studio")
                        font.family: theme ? theme.headlineFontFamily : appTheme.headlineFontFamily
                        font.pixelSize: 17
                        font.weight: 700
                        color: theme ? theme.colText : appTheme.textColor
                    }
                }
            }

            Repeater {
                model: panel.utilityTabs

                delegate: NavRow {
                    required property var modelData
                    objectName: modelData.tabId === "maintenance"
                                ? "projectMaintenanceLauncher"
                                : "collectionsUtility_" + modelData.tabId
                    enabled: modelData.tabId !== "maintenance"
                             || (!!panel.maintenanceController
                                 && panel.maintenanceController.available)
                    activeFocusOnTab: enabled && modelData.tabId === "maintenance"
                    Keys.onReturnPressed: activated()
                    Keys.onSpacePressed: activated()
                    Accessible.onPressAction: if (enabled) activated()
                    border.width: activeFocus ? 1 : 0
                    iconSource: modelData.iconSource
                    label: modelData.label
                    onActivated: {
                        panel.activeUtilityTab = modelData.tabId
                        if (modelData.tabId === "search") {
                            panel.searchRequested()
                        } else if (modelData.tabId === "background-tasks") {
                            panel.backgroundTasksRequested()
                        } else if (modelData.tabId === "maintenance") {
                            panel.projectMaintenanceRequested()
                        }
                    }
                }
            }

            // All Photos: every photo of the project, mounted photos included (#1078). The
            // Library row of the tree holds the photos directly in the library root.
            NavRow {
                id: allPhotosRow
                objectName: "collectionsAllPhotosRow"
                readonly property real count: panel.foldersModule
                                              && panel.foldersModule.allPhotosCount !== undefined
                                              ? Number(panel.foldersModule.allPhotosCount) : -1
                visible: panel.rowForFolder(0) !== null
                iconSource: "qrc:/panel_icons/image.svg"
                label: qsTr("All Photos")
                trailingText: count >= 0 ? count.toLocaleString(Qt.locale(), "f", 0) : ""
                selected: panel.allPhotosSelected
                onActivated: {
                    folderList.forceActiveFocus()
                    if (panel.foldersModule && panel.backendInteractive && !panel.allPhotosSelected) {
                        panel.foldersModule.SelectFolder(0)
                    }
                }
            }
            ListView {
                id: folderList
                objectName: "collectionsTree"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: appTheme.spaceXs
                clip: true
                spacing: 0
                model: panel.folderRows
                activeFocusOnTab: true
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                Accessible.role: Accessible.Tree
                Accessible.name: qsTr("Collections")

                // The shared scroll bar of the Inspector pages. It has its own column at the
                // right of the rows, so it never covers a count, and it sits above the tree
                // pointer handler (z 10), so it takes its own drags (#1028).
                ScrollBar.vertical: AlcedoScrollBar {
                    id: treeScrollBar
                    objectName: "collectionsTreeScrollBar"
                    z: 20
                }

                Keys.onShortcutOverride: function(event) {
                    if (!panel.dragActive && panel.renamingElementId < 0
                            && panel.draftParentFolderId < 0
                            && [Qt.Key_Up, Qt.Key_Down, Qt.Key_Left, Qt.Key_Right, Qt.Key_F2,
                                Qt.Key_Delete, Qt.Key_Backspace].indexOf(event.key) >= 0) {
                        event.accepted = true
                    }
                }
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Escape && panel.dragActive) {
                        panel.cancelDrag()
                        event.accepted = true
                        return
                    }
                    event.accepted = panel.handleTreeKey(event)
                }

                delegate: Item {
                    id: folderRow
                    required property var modelData
                    required property int index

                    readonly property bool shown: panel.rowShown[index] === true
                    // "" for a folder row and the draft row; "section", "volume", "volumeCard".
                    readonly property string rowKind: modelData.rowKind ? String(modelData.rowKind)
                                                                        : ""
                    readonly property bool isFolder: rowKind.length === 0
                    readonly property bool isDraft: modelData.isDraft === true
                    readonly property bool isRoot: isFolder && !isDraft
                                                   && Number(modelData.folderId) === 0
                    readonly property bool selected: isFolder && !isDraft
                        && Number(modelData.folderId) === panel.currentFolderId
                        && (!isRoot || panel.libraryRootSelected)
                    readonly property bool hovered: isFolder && !isDraft
                                                    && treeMouse.hoverIndex === index
                    readonly property bool dropTarget: isFolder && !isDraft
                        && ((panel.dragActive
                             && Number(modelData.folderId) === panel.dropTargetFolderId)
                            || Number(modelData.folderId) === panel.fileDropFolderId)
                    readonly property bool renaming: !isDraft
                        && Number(modelData.elementId) === panel.renamingElementId
                    readonly property bool editing: isDraft || renaming
                    readonly property bool mounted: isFolder && !isDraft
                                                    && Number(modelData.mountId) > 0
                    readonly property bool mountRoot: mounted && modelData.mountRoot === true
                    readonly property bool mountOffline: mounted && modelData.mountOffline === true
                    readonly property bool menuOpen: !isDraft
                        && Number(modelData.elementId) === panel.menuRowElementId
                    // On hover and while its menu is open, the more button takes the place of
                    // the count.
                    readonly property bool moreShown: !editing && !panel.dragActive
                                                      && (hovered || menuOpen)

                    objectName: isDraft ? "collectionDraftRow"
                              : rowKind === "section" ? "collectionsSection_" + modelData.section
                              : rowKind === "volume" ? "collectionsVolumeRow_" + modelData.volumeKey
                              : rowKind === "volumeCard" ? "collectionsVolumeCard"
                              : "collectionRow_" + String(modelData.elementId)
                    width: panel.treeRowWidth
                    height: !shown ? 0
                            : rowKind === "section" ? panel.sectionHeaderHeight(modelData.section)
                            : rowKind === "volumeCard" ? panel.volumeCardHeight
                            : appTheme.collectionsTreeRowHeight
                    visible: shown

                    Rectangle {
                        anchors.fill: parent
                        anchors.rightMargin: 2
                        visible: folderRow.isFolder
                        radius: appTheme.controlRadiusSmall
                        // Selected: a quiet neutral well (controlHoveredFillColor) under
                        // unchanged text. Drop target: hover wash plus a 1 px muted outline.
                        // Hover: the darker hoverColor wash.
                        color: folderRow.selected && !folderRow.editing
                               ? appTheme.controlHoveredFillColor
                               : (folderRow.dropTarget || folderRow.hovered || folderRow.menuOpen)
                                 ? appTheme.hoverColor
                                 : "transparent"
                        border.width: folderRow.dropTarget ? 1 : 0
                        border.color: appTheme.textMutedColor
                    }

                    // Indent guides: one line per collection above the row, through the
                    // chevron column of that collection. The guide of the collection that
                    // holds the selection is brighter.
                    Repeater {
                        model: folderRow.shown && folderRow.isFolder
                               ? (panel.rowAncestors[folderRow.index] || []) : []

                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            x: panel.guideX(index + 1)
                            width: 1
                            height: folderRow.height
                            color: Number(modelData) === panel.activeGuideFolderId
                                   ? appTheme.collectionsTreeGuideActiveColor
                                   : appTheme.collectionsTreeGuideColor
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: panel.treeRowInset + panel.rowIndent(folderRow.modelData)
                        anchors.rightMargin: appTheme.spaceXs
                        spacing: 0
                        visible: folderRow.isFolder

                        // Disclosure chevron column. A leaf row keeps the empty column, so its
                        // name aligns with the names of the rows that have children.
                        Item {
                            Layout.preferredWidth: panel.treeChevronSize
                            Layout.preferredHeight: panel.treeChevronSize
                            Layout.alignment: Qt.AlignVCenter

                            IconImage {
                                objectName: folderRow.isDraft
                                            ? ""
                                            : "collectionChevron_" + String(folderRow.modelData.elementId)
                                anchors.centerIn: parent
                                width: 12
                                height: 12
                                sourceSize: Qt.size(12, 12)
                                visible: !folderRow.isDraft && folderRow.modelData.hasChildren === true
                                source: "qrc:/panel_icons/chevron-right.svg"
                                color: appTheme.textMutedColor
                                rotation: panel.isExpanded(folderRow.modelData) ? 90 : 0
                            }
                        }

                        // Folder icon. The Library row has the library icon, a mount root the
                        // folder-symlink icon; the folders of a mount and the ordinary
                        // collections have the folder icon.
                        IconImage {
                            objectName: folderRow.isDraft
                                        ? ""
                                        : "collectionRowIcon_" + String(folderRow.modelData.elementId)
                            Layout.leftMargin: appTheme.spaceXs
                            Layout.rightMargin: appTheme.spaceSm
                            Layout.preferredWidth: panel.treeIconSize
                            Layout.preferredHeight: panel.treeIconSize
                            Layout.alignment: Qt.AlignVCenter
                            sourceSize: Qt.size(panel.treeIconSize, panel.treeIconSize)
                            source: folderRow.isRoot ? "qrc:/panel_icons/library.svg"
                                    : folderRow.mountRoot ? "qrc:/panel_icons/folder-symlink.svg"
                                                          : "qrc:/panel_icons/folder.svg"
                            // Muted as the editor side panel icons; the selected row brightens it.
                            color: folderRow.selected ? appTheme.textColor
                                                      : appTheme.textMutedColor
                            opacity: folderRow.mountOffline ? 0.45 : 1.0
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.alignment: Qt.AlignVCenter
                            visible: !folderRow.editing
                            // Uniform fixed-height tree rows elide (DESIGN.md truncation policy).
                            elide: Text.ElideRight
                            text: folderRow.isRoot ? qsTr("Library") : String(folderRow.modelData.name)
                            opacity: folderRow.mountOffline ? 0.45 : 1.0
                            color: appTheme.textColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeTitle
                            font.weight: folderRow.selected ? appTheme.fontWeightStrong
                                                            : appTheme.fontWeightRegular
                        }

                        // Photo count of the collection. An offline mount shows Offline in its
                        // place.
                        Label {
                            readonly property real count: panel.imageCountOf(folderRow.modelData)
                            objectName: folderRow.isDraft
                                        ? ""
                                        : "collectionCount_" + String(folderRow.modelData.elementId)
                            Layout.leftMargin: appTheme.spaceSm
                            Layout.alignment: Qt.AlignVCenter
                            visible: !folderRow.editing && !folderRow.moreShown
                                     && (count >= 0 || folderRow.mountOffline)
                            text: folderRow.mountOffline
                                  ? qsTr("Offline")
                                  : count >= 0 ? count.toLocaleString(Qt.locale(), "f", 0) : ""
                            // Selected: brightens to the name ink on the neutral well.
                            color: folderRow.selected ? appTheme.textColor
                                                      : appTheme.textMutedColor
                            font.family: folderRow.mountOffline ? appTheme.uiFontFamily
                                                                : appTheme.headlineFontFamily
                            font.features: { "tnum": 1 }
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: folderRow.mountOffline ? appTheme.fontWeightRegular
                                                                : appTheme.fontWeightStrong
                        }

                        // More button: opens the row menu (the tree pointer handler takes the
                        // click; see treeMouse.onMoreButton).
                        Rectangle {
                            objectName: folderRow.isDraft
                                        ? ""
                                        : "collectionMoreButton_" + String(folderRow.modelData.elementId)
                            Layout.leftMargin: appTheme.spaceSm
                            Layout.preferredWidth: panel.treeMoreSize
                            Layout.preferredHeight: panel.treeMoreSize
                            Layout.alignment: Qt.AlignVCenter
                            visible: folderRow.moreShown
                            radius: appTheme.controlRadiusSmall
                            color: folderRow.menuOpen || treeMouse.moreHoverIndex === folderRow.index
                                   ? appTheme.controlHoveredFillColor
                                   : "transparent"

                            IconImage {
                                anchors.centerIn: parent
                                width: 16
                                height: 16
                                sourceSize: Qt.size(16, 16)
                                source: "qrc:/panel_icons/dots.svg"
                                color: folderRow.menuOpen || treeMouse.moreHoverIndex === folderRow.index
                                       ? appTheme.textColor
                                       : appTheme.textMutedColor
                            }
                        }
                        Loader {
                            Layout.fillWidth: true
                            Layout.preferredHeight: appTheme.collectionsTreeRowHeight - 6
                            Layout.alignment: Qt.AlignVCenter
                            active: folderRow.editing && folderRow.shown
                            visible: active
                            // A plain TextInput in a hairline box: no style-provided field chrome.
                            sourceComponent: Rectangle {
                                radius: appTheme.controlRadiusSmall
                                color: appTheme.bgBaseColor
                                border.width: 1
                                border.color: nameEditor.activeFocus ? appTheme.textMutedColor
                                                                     : appTheme.dividerColor

                                TextInput {
                                    id: nameEditor
                                    objectName: folderRow.isDraft ? "collectionDraftField"
                                                                  : "collectionRenameField"
                                    anchors.fill: parent
                                    anchors.leftMargin: appTheme.spaceSm
                                    anchors.rightMargin: appTheme.spaceSm
                                    verticalAlignment: TextInput.AlignVCenter
                                    clip: true
                                    text: folderRow.isDraft ? "" : String(folderRow.modelData.name)
                                    selectByMouse: true
                                    color: appTheme.textColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeTitle
                                    selectedTextColor: appTheme.textColor
                                    selectionColor: panel.withAlpha(appTheme.textMutedColor, 0.6)
                                    property bool finished: false

                                    Text {
                                        anchors.fill: parent
                                        verticalAlignment: Text.AlignVCenter
                                        visible: nameEditor.text.length === 0
                                                 && nameEditor.preeditText.length === 0
                                        text: qsTr("Collection Name")
                                        color: appTheme.textMutedColor
                                        font: nameEditor.font
                                        elide: Text.ElideRight
                                    }

                                    function finish(accept) {
                                        if (finished) {
                                            return
                                        }
                                        finished = true
                                        // The commit reloads the tree, which destroys this
                                        // editor; run it after this handler returns.
                                        const value = text
                                        if (folderRow.isDraft) {
                                            if (accept) {
                                                Qt.callLater(panel.commitDraftCollection, value)
                                            } else {
                                                Qt.callLater(panel.cancelDraftCollection)
                                            }
                                        } else if (accept) {
                                            Qt.callLater(panel.commitRename, value)
                                        } else {
                                            Qt.callLater(panel.cancelRename)
                                        }
                                    }

                                    // The editor can appear while the tree model is reassigned,
                                    // which moves focus. It takes focus one turn later; only a
                                    // focus loss after that commits the name.
                                    property bool armed: false
                                    Timer {
                                        interval: 0
                                        running: true
                                        onTriggered: {
                                            nameEditor.forceActiveFocus()
                                            nameEditor.selectAll()
                                            nameEditor.armed = nameEditor.activeFocus
                                        }
                                    }
                                    onAccepted: finish(true)
                                    Keys.onEscapePressed: finish(false)
                                    onActiveFocusChanged: {
                                        if (!activeFocus && armed) {
                                            finish(true)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Section header (#1078): the title folds the section. Collections has sort
                    // and New Collection, shown on hover and while they are in use; Disk has a
                    // + that opens the Import Folder dialog with Mount Folder selected. The tree
                    // pointer handler passes the presses on these rows to their controls.
                    Item {
                        id: sectionContent
                        anchors.fill: parent
                        anchors.topMargin: disk ? appTheme.spaceSm : 0
                        visible: folderRow.rowKind === "section"

                        readonly property bool disk: folderRow.modelData.section === "disk"
                        readonly property bool open: disk ? panel.diskSectionOpen
                                                          : panel.collectionsSectionOpen
                        readonly property bool rowHovered: treeMouse.hoverIndex === folderRow.index
                        readonly property bool actionsShown: rowHovered
                            || (!disk && (panel.sortDescending || panel.draftParentFolderId >= 0))

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: panel.treeRowInset
                            spacing: appTheme.spaceXs

                            Item {
                                id: sectionToggle
                                objectName: sectionContent.disk ? "collectionsDiskSectionToggle"
                                                                : "collectionsSectionToggle"
                                Layout.preferredWidth: sectionTitle.implicitWidth
                                                       + panel.treeChevronSize + appTheme.spaceXs
                                Layout.fillHeight: true
                                readonly property bool hot: panel.treePointerOver(sectionToggle,
                                                                                  folderRow.index)
                                Accessible.role: Accessible.Button
                                Accessible.name: sectionTitle.text
                                Accessible.onPressAction: toggle()

                                function toggle() {
                                    if (sectionContent.disk) {
                                        panel.diskSectionOpen = !panel.diskSectionOpen
                                    } else {
                                        panel.collectionsSectionOpen = !panel.collectionsSectionOpen
                                    }
                                }

                                Label {
                                    id: sectionTitle
                                    anchors.left: parent.left
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: sectionContent.disk ? qsTr("Disk") : qsTr("Collections")
                                    color: sectionToggle.hot ? appTheme.textColor
                                                             : appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeBody
                                    font.weight: appTheme.fontWeightStrong
                                }

                                IconImage {
                                    anchors.left: sectionTitle.right
                                    anchors.leftMargin: appTheme.spaceXs
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 12
                                    height: 12
                                    sourceSize: Qt.size(12, 12)
                                    source: "qrc:/panel_icons/chevron-right.svg"
                                    color: appTheme.textMutedColor
                                    rotation: sectionContent.open ? 90 : 0
                                    opacity: sectionContent.rowHovered || !sectionContent.open ? 1 : 0
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: sectionToggle.toggle()
                                }
                            }

                            Item { Layout.fillWidth: true }

                            TreeRowButton {
                                id: sortButton
                                objectName: "collectionsSortButton"
                                visible: !sectionContent.disk
                                opacity: sectionContent.actionsShown ? 1 : 0
                                iconSource: "qrc:/panel_icons/sort.svg"
                                iconRotation: panel.sortDescending ? 180 : 0
                                active: panel.sortDescending
                                hot: panel.treePointerOver(sortButton, folderRow.index)
                                toolTip: panel.sortDescending ? qsTr("Sorted Z-A") : qsTr("Sorted A-Z")
                                onClicked: panel.sortDescending = !panel.sortDescending
                            }

                            TreeRowButton {
                                id: addCollectionButton
                                objectName: "collectionsAddButton"
                                visible: !sectionContent.disk
                                // A new collection goes inside the selected one; with All Photos
                                // selected it goes into Library.
                                readonly property var parentRow: panel.currentRow
                                                                 ? panel.currentRow
                                                                 : panel.rowForFolder(0)
                                enabled: panel.backendInteractive
                                         && panel.structureEditable(parentRow)
                                opacity: sectionContent.actionsShown ? 1 : 0
                                iconSource: "qrc:/panel_icons/folder-plus.svg"
                                active: panel.draftParentFolderId >= 0
                                hot: panel.treePointerOver(addCollectionButton, folderRow.index)
                                toolTip: qsTr("New collection")
                                onClicked: panel.beginCreateCollection(Number(parentRow.folderId))
                            }

                            TreeRowButton {
                                id: diskAddButton
                                objectName: "collectionsDiskAddButton"
                                visible: sectionContent.disk
                                enabled: panel.backendInteractive
                                iconSource: "qrc:/panel_icons/plus.svg"
                                hot: panel.treePointerOver(diskAddButton, folderRow.index)
                                toolTip: qsTr("Mount Folder")
                                onClicked: panel.importFolderRequested("mount", "", "")
                            }
                        }
                    }

                    // Volume row of the Disk section: drive icon, volume name, and the mount point,
                    // or Offline. An offline volume is dimmed.
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: panel.treeRowInset
                        anchors.rightMargin: appTheme.spaceSm
                        visible: folderRow.rowKind === "volume"
                        spacing: appTheme.spaceSm
                        opacity: folderRow.modelData.offline === true ? 0.55 : 1.0

                        readonly property string volumeName: String(folderRow.modelData.volumeName || "")
                        readonly property string mountPoint: String(folderRow.modelData.mountPoint || "")

                        IconImage {
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            Layout.alignment: Qt.AlignVCenter
                            sourceSize: Qt.size(16, 16)
                            source: "qrc:/panel_icons/drive.svg"
                            color: appTheme.textMutedColor
                        }

                        Label {
                            objectName: folderRow.rowKind === "volume"
                                        ? "collectionsVolumeName_" + folderRow.modelData.volumeKey : ""
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.alignment: Qt.AlignVCenter
                            elide: Text.ElideRight
                            text: parent.volumeName.length > 0
                                  ? parent.volumeName
                                  : (parent.mountPoint.length > 0
                                     ? parent.mountPoint
                                     : String(folderRow.modelData.volumeRoot || ""))
                            color: appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeBody
                            font.weight: appTheme.fontWeightStrong
                        }

                        Label {
                            objectName: folderRow.rowKind === "volume"
                                        ? "collectionsVolumeTag_" + folderRow.modelData.volumeKey : ""
                            Layout.alignment: Qt.AlignVCenter
                            visible: text.length > 0
                            text: folderRow.modelData.offline === true
                                  ? qsTr("Offline")
                                  : (parent.volumeName.length > 0 ? parent.mountPoint : "")
                            color: appTheme.textMutedColor
                            font.family: folderRow.modelData.offline === true
                                         ? appTheme.uiFontFamily : appTheme.headlineFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }
                    }

                    // Card of a volume that arrived with no mount (#1076): its name, mount point,
                    // and DCIM when present; Import... and Mount... open the Import Folder dialog
                    // for DCIM, or the system folder dialog at the volume root.
                    Rectangle {
                        id: volumeCard
                        anchors.fill: parent
                        anchors.topMargin: appTheme.spaceXs
                        anchors.bottomMargin: appTheme.spaceXs
                        anchors.rightMargin: 2
                        visible: folderRow.rowKind === "volumeCard"
                        radius: appTheme.controlRadius
                        color: appTheme.bgBaseColor
                        border.width: 1
                        border.color: appTheme.controlBorderColor

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: appTheme.spaceSm
                            spacing: appTheme.spaceXs

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: appTheme.spaceSm

                                IconImage {
                                    Layout.preferredWidth: 16
                                    Layout.preferredHeight: 16
                                    Layout.alignment: Qt.AlignVCenter
                                    sourceSize: Qt.size(16, 16)
                                    source: "qrc:/panel_icons/drive.svg"
                                    color: appTheme.textColor
                                }

                                Label {
                                    objectName: "collectionsVolumeCardName"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.alignment: Qt.AlignVCenter
                                    elide: Text.ElideRight
                                    text: String(panel.arrivedVolume.name || panel.arrivedVolume.mountPoint
                                                 || "")
                                    color: appTheme.textColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeTitle
                                    font.weight: appTheme.fontWeightStrong
                                }

                                TreeRowButton {
                                    id: volumeCardClose
                                    objectName: "collectionsVolumeCardClose"
                                    iconSource: "qrc:/panel_icons/close.svg"
                                    hot: panel.treePointerOver(volumeCardClose, folderRow.index)
                                    toolTip: qsTr("Close")
                                    onClicked: {
                                        if (panel.foldersModule) {
                                            panel.foldersModule.DismissArrivedVolume()
                                        }
                                    }
                                }
                            }

                            Row {
                                Layout.leftMargin: 16 + appTheme.spaceSm
                                spacing: appTheme.spaceSm

                                Label {
                                    text: String(panel.arrivedVolume.mountPoint || "")
                                    color: appTheme.textMutedColor
                                    font.family: appTheme.headlineFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }

                                Label {
                                    objectName: "collectionsVolumeCardDcim"
                                    visible: panel.arrivedVolume.hasDcim === true
                                    text: "DCIM"
                                    color: appTheme.textMutedColor
                                    font.family: appTheme.headlineFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: appTheme.spaceXs
                                spacing: appTheme.spaceXs

                                DialogActionButton {
                                    objectName: "collectionsVolumeCardImport"
                                    Layout.fillWidth: true
                                    kind: "normal"
                                    text: qsTr("Import...")
                                    enabled: panel.backendInteractive
                                    buttonWidth: 0
                                    buttonHeight: 28
                                    buttonRadius: appTheme.controlRadiusSmall
                                    font.pixelSize: appTheme.fontSizeBody
                                    font.weight: appTheme.fontWeightRegular
                                    onClicked: panel.openVolumeCard("import")
                                }

                                DialogActionButton {
                                    objectName: "collectionsVolumeCardMount"
                                    Layout.fillWidth: true
                                    kind: "normal"
                                    text: qsTr("Mount...")
                                    enabled: panel.backendInteractive
                                    buttonWidth: 0
                                    buttonHeight: 28
                                    buttonRadius: appTheme.controlRadiusSmall
                                    font.pixelSize: appTheme.fontSizeBody
                                    font.weight: appTheme.fontWeightRegular
                                    onClicked: panel.openVolumeCard("mount")
                                }
                            }
                        }
                    }
                }

                // One pointer handler for the whole tree: select, expand, open the row menu,
                // and drag a collection onto another one. It sits above the rows and steps
                // aside while an inline editor is open.
                MouseArea {
                    id: treeMouse
                    objectName: "collectionsTreeMouse"
                    // Declared inside the ListView but parented to the view, not to the
                    // scrolling content, so the coordinates are viewport coordinates.
                    parent: folderList
                    anchors.fill: parent
                    z: 10
                    enabled: panel.renamingElementId < 0 && panel.draftParentFolderId < 0
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    preventStealing: true

                    property int hoverIndex: -1
                    // The pointer in viewport coordinates: the hover state of the controls of the
                    // rows that are not folders (panel.treePointerOver).
                    property point hoverPoint: Qt.point(-1, -1)
                    // Index of the row whose more button is under the pointer, -1 when none.
                    property int moreHoverIndex: -1
                    property int pressIndex: -1
                    property point pressPoint: Qt.point(0, 0)
                    property bool suppressClick: false

                    function rowIndexAt(mouseX, mouseY) {
                        return folderList.indexAt(mouseX, mouseY + folderList.contentY)
                    }

                    function onDisclosure(index, mouseX) {
                        const row = panel.folderRows[index]
                        const start = panel.treeRowInset + panel.rowIndent(row)
                        return mouseX >= start - 2
                               && mouseX <= start + panel.treeChevronSize + 2
                    }

                    // The more button sits at the right end of a hovered row.
                    function onMoreButton(index, mouseX) {
                        const row = index >= 0 ? panel.folderRows[index] : null
                        if (!panel.isFolderRow(row)) {
                            return false
                        }
                        const right = panel.treeRowWidth - appTheme.spaceXs
                        return mouseX >= right - panel.treeMoreSize && mouseX <= right
                    }

                    onExited: {
                        hoverIndex = -1
                        moreHoverIndex = -1
                        hoverPoint = Qt.point(-1, -1)
                    }
                    onPressed: function(mouse) {
                        const index = rowIndexAt(mouse.x, mouse.y)
                        const pressed = index >= 0 ? panel.folderRows[index] : null
                        if (pressed && pressed.rowKind) {
                            // A section header, a volume row, or the volume card: its own
                            // controls take the press.
                            mouse.accepted = false
                            return
                        }
                        folderList.forceActiveFocus()
                        pressIndex = index
                        pressPoint = Qt.point(mouse.x, mouse.y)
                        suppressClick = false
                        if (mouse.button === Qt.RightButton) {
                            suppressClick = true
                            const row = pressIndex >= 0 ? panel.folderRows[pressIndex] : null
                            if (panel.isFolderRow(row) && panel.backendInteractive) {
                                panel.openRowMenu(row)
                            }
                        }
                    }
                    onPositionChanged: function(mouse) {
                        hoverPoint = Qt.point(mouse.x, mouse.y)
                        hoverIndex = rowIndexAt(mouse.x, mouse.y)
                        moreHoverIndex = onMoreButton(hoverIndex, mouse.x) ? hoverIndex : -1
                        if (!(mouse.buttons & Qt.LeftButton) || pressIndex < 0) {
                            return
                        }
                        if (!panel.dragActive) {
                            const dx = mouse.x - pressPoint.x
                            const dy = mouse.y - pressPoint.y
                            const row = panel.folderRows[pressIndex]
                            if (dx * dx + dy * dy > 36 && panel.isFolderRow(row)
                                    && Number(row.folderId) !== 0 && panel.backendInteractive
                                    && panel.structureEditable(row)) {
                                panel.dragSourceRow = row
                                panel.dragActive = true
                                suppressClick = true
                            }
                        }
                        if (panel.dragActive) {
                            panel.dragPoint = mapToItem(collectionsSurface, mouse.x, mouse.y)
                            panel.updateDropTarget(mouse.x, mouse.y)
                            autoScroll.edgeDelta = mouse.y < 24 ? -1
                                                 : (mouse.y > height - 24 ? 1 : 0)
                        }
                    }
                    onReleased: function(mouse) {
                        autoScroll.edgeDelta = 0
                        if (panel.dragActive) {
                            panel.finishDrag()
                        }
                    }
                    onCanceled: {
                        autoScroll.edgeDelta = 0
                        panel.cancelDrag()
                    }
                    onClicked: function(mouse) {
                        if (suppressClick || mouse.button !== Qt.LeftButton) {
                            return
                        }
                        const index = rowIndexAt(mouse.x, mouse.y)
                        if (index < 0 || index !== pressIndex) {
                            return
                        }
                        const row = panel.folderRows[index]
                        if (!panel.isFolderRow(row)) {
                            return
                        }
                        if (onDisclosure(index, mouse.x) && row.hasChildren === true) {
                            panel.toggleExpanded(row)
                            return
                        }
                        if (onMoreButton(index, mouse.x)) {
                            if (panel.backendInteractive) {
                                panel.openRowMenu(row)
                            }
                            return
                        }
                        panel.selectRow(row)
                    }
                    onDoubleClicked: function(mouse) {
                        const index = rowIndexAt(mouse.x, mouse.y)
                        if (index >= 0 && !onDisclosure(index, mouse.x)
                                && !onMoreButton(index, mouse.x)) {
                            panel.toggleExpanded(panel.folderRows[index])
                        }
                    }
                }

                // Drop of local files and folders onto a row: import into that collection.
                // Only text/uri-list drags reach it; the internal collection drag above is a
                // MouseArea drag and carries no URLs.
                DropArea {
                    id: fileDropArea
                    objectName: "collectionsFileDropArea"
                    parent: folderList
                    anchors.fill: parent
                    keys: ["text/uri-list"]

                    property var urls: []
                    property point point: Qt.point(0, 0)

                    function track(drag) {
                        point = Qt.point(drag.x, drag.y)
                        autoScroll.edgeDelta = drag.y < 24 ? -1 : (drag.y > height - 24 ? 1 : 0)
                        return panel.updateFileDropTarget(drag.x, drag.y)
                    }

                    // Entering takes every local file drag, so the moves keep arriving while
                    // the pointer crosses the gaps between rows; a move reports whether the row
                    // under it takes the drop. The action is always Copy: import reads the
                    // files in place, and a Move answer lets the file manager delete them.
                    onEntered: function(drag) {
                        urls = panel.fileDropEnabled ? FileDrop.localFileUrls(drag) : []
                        if (urls.length === 0) {
                            drag.accepted = false
                            return
                        }
                        track(drag)
                        drag.accept(Qt.CopyAction)
                    }
                    onPositionChanged: function(drag) {
                        if (urls.length > 0 && track(drag)) {
                            drag.accept(Qt.CopyAction)
                        } else {
                            drag.accepted = false
                        }
                    }
                    onExited: {
                        autoScroll.edgeDelta = 0
                        panel.fileDropFolderId = -1
                        urls = []
                    }
                    onDropped: function(drop) {
                        autoScroll.edgeDelta = 0
                        const target = panel.updateFileDropTarget(drop.x, drop.y)
                                       ? panel.fileDropFolderId : -1
                        const dropped = urls.length > 0 ? urls : FileDrop.localFileUrls(drop)
                        panel.fileDropFolderId = -1
                        urls = []
                        if (target < 0 || dropped.length === 0) {
                            drop.accepted = false
                            return
                        }
                        drop.accept(Qt.CopyAction)
                        panel.filesDropped(dropped, target)
                    }
                }

                Timer {
                    id: autoScroll
                    property int edgeDelta: 0
                    interval: 30
                    repeat: true
                    running: (panel.dragActive || fileDropArea.containsDrag) && edgeDelta !== 0
                    onTriggered: {
                        const maxY = Math.max(0, folderList.contentHeight - folderList.height)
                        folderList.contentY = Math.max(0, Math.min(maxY,
                            folderList.contentY + edgeDelta * 6))
                        if (fileDropArea.containsDrag) {
                            panel.updateFileDropTarget(fileDropArea.point.x, fileDropArea.point.y)
                            return
                        }
                        const local = collectionsSurface.mapToItem(folderList,
                                                                   panel.dragPoint.x,
                                                                   panel.dragPoint.y)
                        panel.updateDropTarget(local.x, local.y)
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: folderList.count === 0
                    text: qsTr("No collections yet")
                    color: panel.withAlpha(theme.colText, 0.55)
                    font.pixelSize: 13
                }
            }


            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                Layout.topMargin: appTheme.spaceXs
                Layout.leftMargin: -appTheme.spaceSm
                Layout.rightMargin: -appTheme.spaceSm
                color: appTheme.dividerColor
            }

            // Foot of the panel: one Import button (#1078 mock): the tray icon, the label, and an
            // up caret for its menu of Import Images and Import Folder.
            Button {
                id: importButton
                objectName: "collectionsImportButton"
                Layout.fillWidth: true
                Layout.topMargin: appTheme.spaceSm
                Layout.preferredHeight: 34
                text: qsTr("Import")
                // Neutral filled action: the light list well with its ink (colours from the
                // Main shell). The button stays enabled: its menu shows which actions apply.
                readonly property color inkColor: theme.filledActionInk(enabled)
                Material.foreground: inkColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightStrong
                padding: 0
                topInset: 0
                bottomInset: 0
                leftInset: 0
                rightInset: 0
                enabled: panel.backendInteractive
                onClicked: panel.openImportMenu(importButton)

                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: theme.filledActionFill(importButton.enabled,
                                                  importButton.hovered || importMenu.visible,
                                                  importButton.down)
                    Behavior on color {
                        enabled: !appTheme.reduceMotion
                        ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
                    }
                }

                contentItem: Item {
                    implicitWidth: importContent.implicitWidth
                    implicitHeight: 34

                    Row {
                        id: importContent
                        anchors.centerIn: parent
                        spacing: appTheme.spaceSm

                        IconImage {
                            objectName: "collectionsImportButtonIcon"
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16
                            height: 16
                            sourceSize: Qt.size(16, 16)
                            source: "qrc:/panel_icons/tray-download.svg"
                            color: importButton.inkColor
                        }

                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: importButton.text
                            font: importButton.font
                            color: importButton.inkColor
                        }

                        // The menu opens above the button.
                        IconImage {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 12
                            height: 12
                            sourceSize: Qt.size(12, 12)
                            source: "qrc:/panel_icons/chevron-right.svg"
                            rotation: -90
                            opacity: 0.7
                            color: importButton.inkColor
                        }
                    }
                }
            }
        }

        // Floating label of the dragged collection; follows the pointer.
        Rectangle {
            objectName: "collectionDragLabel"
            z: 100
            visible: panel.dragActive && panel.dragSourceRow !== null
            x: panel.dragPoint.x + 12
            y: panel.dragPoint.y - height / 2
            width: Math.min(dragLabelText.implicitWidth + appTheme.spaceSm * 2, collectionsSurface.width * 0.8)
            height: appTheme.collectionsTreeRowHeight
            radius: appTheme.controlRadiusSmall
            color: appTheme.bgBaseColor
            border.width: 1
            border.color: appTheme.dividerColor

            Label {
                id: dragLabelText
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: panel.dragSourceRow ? String(panel.dragSourceRow.name) : ""
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeTitle
            }
        }
    }

    // Import Images puts photos into the selected collection: a mounted selection takes none
    // (Mounted Folder Plan, D6 and D7). Import Folder imports into Library then (#1078).
    readonly property bool footActionsEnabled: backendInteractive
                                               && structureEditable(rowForFolder(currentFolderId))

    function sectionHeaderHeight(section) {
        return 30 + (section === "disk" ? appTheme.spaceSm : 0)
    }
    readonly property int volumeCardHeight: 108

    // The Import... and Mount... buttons of the volume card: the Import Folder dialog for DCIM,
    // or the system folder dialog at the volume root when the volume has no DCIM. The action
    // stops when the volume of the card is no longer at its mount point (#1087).
    function openVolumeCard(action) {
        const volume = arrivedVolume
        const dcim = volume.hasDcim === true && volume.dcimUrl ? String(volume.dcimUrl) : ""
        const root = volume.rootUrl ? String(volume.rootUrl) : ""
        if (!foldersModule || !foldersModule.TakeArrivedVolume()) {
            return
        }
        importFolderRequested(action, dcim, dcim.length > 0 ? "" : root)
    }

    // Width and height of the more button of a tree row.
    readonly property int treeMoreSize: 22
    // elementId of the row whose menu is open, -1 when none.
    property int menuRowElementId: -1

    // Icon button of a tree row that is not a folder (section header, volume card). The tree
    // pointer handler lies above the rows, so the owner sets `hot` from the pointer position
    // (panel.treePointerOver); the handler passes the press to the button.
    component TreeRowButton: Rectangle {
        id: rowButton
        property url iconSource
        property string toolTip: ""
        property bool hot: false
        property bool active: false
        property real iconRotation: 0
        signal clicked()

        Layout.preferredWidth: 24
        Layout.preferredHeight: 24
        Layout.alignment: Qt.AlignVCenter
        implicitWidth: 24
        implicitHeight: 24
        radius: appTheme.controlRadiusSmall
        color: enabled && (hot || active) ? appTheme.controlFillColor : "transparent"
        Accessible.role: Accessible.Button
        Accessible.name: toolTip
        Accessible.onPressAction: rowButton.clicked()

        IconImage {
            anchors.centerIn: parent
            width: 14
            height: 14
            sourceSize: Qt.size(14, 14)
            source: rowButton.iconSource
            // Idle icons are muted, as in the editor side panels.
            color: rowButton.enabled && (rowButton.hot || rowButton.active)
                   ? appTheme.textColor : appTheme.textMutedColor
            rotation: rowButton.iconRotation

            Behavior on rotation {
                enabled: !appTheme.reduceMotion
                NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
            }
        }

        MouseArea {
            anchors.fill: parent
            enabled: rowButton.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: rowButton.clicked()
        }

        ToolTip.visible: hot && toolTip.length > 0
        ToolTip.text: toolTip
    }

    // Navigation row above the tree (Search, Background Tasks, All Photos): the height and the
    // states of a tree row, with an icon in the chevron column.
    component NavRow: Rectangle {
        id: navRow
        property url iconSource
        property string label
        property string trailingText
        property bool selected: false
        property bool dropTarget: false
        signal activated()
        signal contextRequested()

        Layout.fillWidth: true
        Layout.preferredHeight: appTheme.collectionsTreeRowHeight
        radius: appTheme.controlRadiusSmall
        color: selected ? appTheme.controlHoveredFillColor
                        : (navMouse.containsMouse || dropTarget) ? appTheme.hoverColor
                                                                 : "transparent"
        border.width: dropTarget ? 1 : 0
        border.color: appTheme.textMutedColor
        Accessible.role: Accessible.Button
        Accessible.name: label

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: appTheme.spaceXs
            anchors.rightMargin: appTheme.spaceSm
            spacing: appTheme.spaceSm

            IconImage {
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                Layout.alignment: Qt.AlignVCenter
                sourceSize: Qt.size(16, 16)
                source: navRow.iconSource
                // Muted as the editor side panel icons; selection and hover brighten it.
                color: navRow.selected || navMouse.containsMouse ? appTheme.textColor
                                                                 : appTheme.textMutedColor
            }

            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.alignment: Qt.AlignVCenter
                elide: Text.ElideRight
                text: navRow.label
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: navRow.selected ? appTheme.fontWeightStrong
                                             : appTheme.fontWeightRegular
            }

            Label {
                Layout.alignment: Qt.AlignVCenter
                visible: navRow.trailingText.length > 0
                text: navRow.trailingText
                color: navRow.selected ? appTheme.textColor : appTheme.textMutedColor
                font.family: appTheme.headlineFontFamily
                font.features: { "tnum": 1 }
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
            }
        }

        MouseArea {
            id: navMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.PointingHandCursor
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton) {
                    navRow.contextRequested()
                } else {
                    navRow.activated()
                }
            }
        }
    }

    AppContextMenu {
        id: importMenu
        objectName: "collectionsImportMenu"
        property real minimumWidth: 0
        width: Math.max(implicitWidth, minimumWidth)

        AppMenuItem {
            objectName: "collectionsImportImagesItem"
            text: qsTr("Import Images...")
            enabled: panel.footActionsEnabled
            onTriggered: panel.importRequested()
        }
        AppMenuItem {
            objectName: "collectionsImportFolderItem"
            text: qsTr("Import Folder...")
            enabled: panel.backendInteractive
            onTriggered: panel.importFolderRequested("", "", "")
        }
    }
    AppContextMenu {
        id: rowMenu
        objectName: "collectionRowMenu"
        onClosed: panel.menuRowElementId = -1
        property var targetRow: null
        readonly property bool targetIsRoot: targetRow !== null && Number(targetRow.folderId) === 0

        AppMenuItem {
            objectName: "collectionMenuNewChild"
            text: rowMenu.targetIsRoot ? qsTr("New collection") : qsTr("New Collection Inside")
            onTriggered: panel.beginCreateCollection(Number(rowMenu.targetRow.folderId))
        }

        AppMenuItem {
            objectName: "collectionMenuRename"
            text: qsTr("Rename")
            enabled: !rowMenu.targetIsRoot
            onTriggered: panel.beginRename(rowMenu.targetRow)
        }
        AppMenuSeparator {}
        AppMenuItem {
            objectName: "collectionMenuDelete"
            text: qsTr("Delete collection")
            enabled: !rowMenu.targetIsRoot
            onTriggered: panel.requestDelete(rowMenu.targetRow)
        }
    }

    // The row menu of a mount root (Mounted Folder Plan, sections 2.2 and MF8).
    AppContextMenu {
        id: mountRootMenu
        objectName: "collectionMountRootMenu"
        onClosed: panel.menuRowElementId = -1
        property var targetRow: null
        readonly property bool offline: targetRow !== null && targetRow.mountOffline === true

        AppMenuItem {
            objectName: "collectionMenuSynchronize"
            text: qsTr("Synchronize")
            enabled: !mountRootMenu.offline
            onTriggered: panel.foldersModule.SynchronizeMount(Number(mountRootMenu.targetRow.folderId))
        }
        AppMenuItem {
            objectName: "collectionMenuLocateFolder"
            text: qsTr("Locate Folder...")
            enabled: mountRootMenu.offline
            onTriggered: panel.requestLocateFolder(mountRootMenu.targetRow)
        }
        AppMenuItem {
            objectName: "collectionMenuShowInFileManager"
            text: qsTr("Show in File Manager")
            enabled: !mountRootMenu.offline
            onTriggered: panel.foldersModule.ShowMountedFolderInFileManager(
                             Number(mountRootMenu.targetRow.folderId))
        }
        AppMenuSeparator {}
        AppMenuItem {
            objectName: "collectionMenuRemoveMissingPhotos"
            text: qsTr("Remove Missing Photos")
            enabled: mountRootMenu.targetRow !== null
                     && Number(mountRootMenu.targetRow.mountMissingCount) > 0
            onTriggered: panel.requestRemoveMissingPhotos(mountRootMenu.targetRow)
        }
        AppMenuSeparator {}
        AppMenuItem {
            objectName: "collectionMenuUnmount"
            text: qsTr("Unmount")
            onTriggered: panel.foldersModule.Unmount(Number(mountRootMenu.targetRow.folderId))
        }
    }

    // The row menu of a mounted folder below the mount root.
    AppContextMenu {
        id: mountedFolderMenu
        objectName: "collectionMountedFolderMenu"
        onClosed: panel.menuRowElementId = -1
        property var targetRow: null

        AppMenuItem {
            objectName: "collectionMenuShowMountedFolder"
            text: qsTr("Show in File Manager")
            enabled: mountedFolderMenu.targetRow !== null
                     && mountedFolderMenu.targetRow.mountOffline !== true
            onTriggered: panel.foldersModule.ShowMountedFolderInFileManager(
                             Number(mountedFolderMenu.targetRow.folderId))
        }
    }


    FolderDialog {
        id: locateFolderDialog
        title: qsTr("Locate Folder")
        onAccepted: {
            const folderId = panel.pendingLocateFolderId
            panel.pendingLocateFolderId = -1
            if (folderId >= 0 && panel.foldersModule) {
                panel.foldersModule.LocateMountFolder(folderId, selectedFolder)
            }
        }
        onRejected: panel.pendingLocateFolderId = -1
    }

    DeleteConfirmDialog {
        id: removeMissingDialog
        objectName: "collectionRemoveMissingDialog"
        parent: Overlay.overlay
        theme: panel.host
        host: panel.host
        blurSource: panel.host ? panel.host.dialogBlurSource : null
        appendSourceFilesNote: false
        confirmEnabled: panel.backendInteractive
        onCancelled: panel.pendingRemoveMissingFolderId = -1
        onConfirmed: {
            const folderId = panel.pendingRemoveMissingFolderId
            panel.pendingRemoveMissingFolderId = -1
            if (folderId >= 0 && panel.foldersModule) {
                panel.foldersModule.RemoveMissingPhotos(folderId)
            }
        }
    }

    DeleteConfirmDialog {
        id: deleteDialog
        objectName: "collectionDeleteDialog"
        parent: Overlay.overlay
        theme: panel.host
        host: panel.host
        blurSource: panel.host ? panel.host.dialogBlurSource : null
        appendSourceFilesNote: false
        confirmEnabled: panel.backendInteractive
        onCancelled: panel.pendingDeleteElementId = -1
        onConfirmed: panel.confirmDelete()
    }
}
