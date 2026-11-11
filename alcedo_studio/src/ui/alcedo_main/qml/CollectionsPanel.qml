import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Controls.impl
import QtQuick.Layouts
import "CollectionTree.js" as CollectionTree
import "FileDrop.js" as FileDrop

// Collections sidebar: wordmark, utility tabs, and a compact file-browser tree of the
// project collections (expand/collapse, inline create and rename, drag to move, keyboard).
ColumnLayout {
    id: panel
    objectName: "collectionsPanel"

    property var folderController
    property var theme
    property var host: null
    property bool backendInteractive: false
    property bool sortDescending: false
    property string activeUtilityTab: "search"
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
        }
    ]
    signal importRequested()
    signal importFromFolderRequested()
    // Local files and folders dropped on the row of folderId (issue #757).
    signal filesDropped(var urls, int folderId)
    signal searchRequested()
    signal backgroundTasksRequested()

    readonly property var foldersModule: folderController
    readonly property int currentFolderId: foldersModule ? Number(foldersModule.currentFolderId) : 0

    // Display rows in tree order (CollectionTree.orderRows), plus one draft row while a new
    // collection is named. Every row stays in the model; a collapsed branch hides its rows.
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

    function rowIndent(row) {
        return Math.max(0, Number(row.treeDepth) - 1) * appTheme.collectionIndentStep
    }

    function indexOfFolder(folderId) {
        for (let i = 0; i < folderRows.length; ++i) {
            if (!folderRows[i].isDraft && Number(folderRows[i].folderId) === Number(folderId)) {
                return i
            }
        }
        return -1
    }

    function indexOfElement(elementId) {
        for (let i = 0; i < folderRows.length; ++i) {
            if (!folderRows[i].isDraft && Number(folderRows[i].elementId) === Number(elementId)) {
                return i
            }
        }
        return -1
    }

    function rowForFolder(folderId) {
        const index = indexOfFolder(folderId)
        return index >= 0 ? folderRows[index] : null
    }

    function isExpanded(row) {
        if (!row || row.isDraft) {
            return false
        }
        // The root row always shows its children.
        return Number(row.folderId) === 0 || expandedIds[String(Number(row.elementId))] === true
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
        if (!row || row.isDraft || Number(row.folderId) === 0 || !row.hasChildren) {
            return
        }
        setExpanded(row.elementId, !isExpanded(row))
    }

    function recomputeShownRows() {
        const byFolderId = CollectionTree.rowsByFolderId(folderRows.filter(function(row) {
            return !row.isDraft
        }))
        const shown = []
        for (let i = 0; i < folderRows.length; ++i) {
            const row = folderRows[i]
            let visible = true
            let parentId = !row.isDraft && Number(row.folderId) === 0
                ? -1 : Number(row.parentFolderId)
            let guard = 0
            while (visible && parentId >= 0 && guard < 4096) {
                const parent = byFolderId[String(Number(parentId))]
                if (!parent || Number(parent.folderId) === 0) {
                    break
                }
                visible = isExpanded(parent)
                parentId = Number(parent.parentFolderId)
                ++guard
            }
            shown.push(visible)
        }
        rowShown = shown
    }

    // Expands every ancestor of the folder so its row is shown, then scrolls to it.
    function revealFolder(folderId) {
        const ordered = folderRows.filter(function(row) { return !row.isDraft })
        const byFolderId = CollectionTree.rowsByFolderId(ordered)
        let current = byFolderId[String(Number(folderId))]
        if (!current) {
            return
        }
        const next = Object.assign({}, expandedIds)
        let parent = byFolderId[String(Number(current.parentFolderId))]
        let guard = 0
        while (Number(current.folderId) !== 0 && parent && Number(parent.folderId) !== 0
               && guard < 4096) {
            next[String(Number(parent.elementId))] = true
            parent = byFolderId[String(Number(parent.parentFolderId))]
            ++guard
        }
        expandedIds = next
        recomputeShownRows()
        const index = indexOfFolder(folderId)
        if (index >= 0) {
            folderList.positionViewAtIndex(index, ListView.Contain)
        }
    }

    function imageCountOf(row) {
        if (!row || row.isDraft) {
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
        const ordered = CollectionTree.orderRows(source, sortDescending)
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

        const signature = rows.map(function(row) {
            return [row.isDraft ? "draft" : "", row.folderId, row.elementId, row.parentFolderId,
                    row.name].join(":")
        }).join("|")
        if (signature !== rowsSignature) {
            const keepY = folderList.contentY
            rowsSignature = signature
            folderRows = rows
            folderList.contentY = Math.max(0, Math.min(keepY,
                folderList.contentHeight - folderList.height))
        }
        recomputeShownRows()
    }

    // Indices of the shown, non-draft rows, top to bottom.
    function shownFolderIndices() {
        const out = []
        for (let i = 0; i < folderRows.length; ++i) {
            if (rowShown[i] === true && !folderRows[i].isDraft) {
                out.push(i)
            }
        }
        return out
    }

    function selectRow(row) {
        if (!row || row.isDraft || !foldersModule || !backendInteractive) {
            return
        }
        if (Number(row.folderId) !== currentFolderId) {
            foldersModule.SelectFolder(Number(row.folderId))
        }
    }

    function moveSelection(step) {
        const shown = shownFolderIndices()
        const currentIndex = indexOfFolder(currentFolderId)
        let position = shown.indexOf(currentIndex)
        if (position < 0) {
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
        const row = rowForFolder(currentFolderId)
        switch (event.key) {
        case Qt.Key_Up:
            moveSelection(-1)
            return true
        case Qt.Key_Down:
            moveSelection(1)
            return true
        case Qt.Key_Left:
            if (row && row.hasChildren && isExpanded(row) && Number(row.folderId) !== 0) {
                setExpanded(row.elementId, false)
            } else if (row && Number(row.folderId) !== 0) {
                selectRow(rowForFolder(row.parentFolderId))
            }
            return true
        case Qt.Key_Right:
            if (row && row.hasChildren && !isExpanded(row)) {
                setExpanded(row.elementId, true)
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
        if (!parentRow) {
            return
        }
        // Open the parent and every folder above it, so the draft row is in view.
        const byFolderId = CollectionTree.rowsByFolderId(folderRows.filter(function(row) {
            return !row.isDraft
        }))
        const next = Object.assign({}, expandedIds)
        let current = parentRow
        let guard = 0
        while (current && Number(current.folderId) !== 0 && guard < 4096) {
            next[String(Number(current.elementId))] = true
            current = byFolderId[String(Number(current.parentFolderId))]
            ++guard
        }
        expandedIds = next
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
        // The tree reloads inside CreateFolderIn and reassigns ui ids, so the draft ends first.
        draftParentFolderId = -1
        if (trimmed.length === 0 || !foldersModule) {
            rebuildFolderRows()
            return
        }
        const createdElementId = Number(foldersModule.CreateFolderIn(parentFolderId, trimmed))
        rebuildFolderRows()
        if (createdElementId > 0) {
            const index = indexOfElement(createdElementId)
            if (index >= 0) {
                revealFolder(folderRows[index].folderId)
            }
        }
    }

    // ── Rename ──────────────────────────────────────────────────────────────

    function beginRename(row) {
        if (!row || row.isDraft || Number(row.folderId) === 0 || !backendInteractive) {
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
        foldersModule.RenameFolder(Number(row.folderId), trimmed)
    }

    // ── Delete ──────────────────────────────────────────────────────────────

    function requestDelete(row) {
        if (!row || row.isDraft || Number(row.folderId) === 0 || !backendInteractive) {
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
        if (!sourceRow || !targetRow || targetRow.isDraft) {
            return false
        }
        if (Number(targetRow.folderId) === Number(sourceRow.parentFolderId)) {
            return false
        }
        const byFolderId = CollectionTree.rowsByFolderId(folderRows.filter(function(row) {
            return !row.isDraft
        }))
        return !CollectionTree.isAtOrBelow(byFolderId, targetRow.folderId, sourceRow.folderId)
    }

    function updateDropTarget(listX, listY) {
        const index = folderList.indexAt(listX, listY + folderList.contentY)
        const target = index >= 0 ? folderRows[index] : null
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
    function canTakeFileDrop(row) {
        return fileDropEnabled && row !== null && row !== undefined && !row.isDraft
               && Number(row.elementId) !== renamingElementId
    }

    function updateFileDropTarget(listX, listY) {
        const index = folderList.indexAt(listX, listY + folderList.contentY)
        const target = index >= 0 ? folderRows[index] : null
        fileDropFolderId = canTakeFileDrop(target) ? Number(target.folderId) : -1
        return fileDropFolderId >= 0
    }

    width: appTheme.collectionsSidebarWidth
    implicitWidth: appTheme.collectionsSidebarWidth
    Layout.preferredWidth: appTheme.collectionsSidebarWidth
    Layout.minimumWidth: appTheme.collectionsSidebarWidth
    Layout.maximumWidth: appTheme.collectionsSidebarWidth
    Layout.fillHeight: true
    spacing: 12
    clip: true

    Component.onCompleted: {
        rebuildFolderRows()
        revealFolder(currentFolderId)
    }
    onSortDescendingChanged: rebuildFolderRows()
    // React to the controller properties (NOTIFY FoldersChanged / FolderSelectionChanged).
    // A Connections handler named after those PascalCase signals does not reach them.
    readonly property var sourceFolders: foldersModule && foldersModule.folders
                                         ? foldersModule.folders : []
    onSourceFoldersChanged: {
        rebuildFolderRows()
        revealFolder(currentFolderId)
    }
    onCurrentFolderIdChanged: {
        rebuildFolderRows()
        revealFolder(currentFolderId)
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

    Rectangle {
        id: collectionsSurface
        objectName: "collectionsSurface"
        Layout.fillWidth: true
        Layout.fillHeight: true
        radius: theme.panelRadius
        color: Qt.darker(theme.colBgPanel, 1.08)
        border.width: 1
        border.color: panel.withAlpha(theme.colText, 0.05)
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 12

            Row {
                id: identityCard
                objectName: "collectionsIdentityCard"
                Layout.fillWidth: true
                spacing: 0
                Label {
                    text: qsTr("Alcedo")
                    font.family: theme ? theme.headlineFontFamily : appTheme.headlineFontFamily
                    font.pixelSize: 19
                    font.weight: 700
                    color: theme ? theme.colAccentPrimary : appTheme.accentColor
                }
                Label {
                    text: " "
                    font.family: theme ? theme.headlineFontFamily : appTheme.headlineFontFamily
                    font.pixelSize: 19
                    font.weight: 700
                }
                Label {
                    text: qsTr("Studio")
                    font.family: theme ? theme.headlineFontFamily : appTheme.headlineFontFamily
                    font.pixelSize: 19
                    font.weight: 700
                    color: theme ? theme.colText : appTheme.textColor
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                Repeater {
                    model: panel.utilityTabs

                    delegate: Rectangle {
                        id: utilityTab
                        required property var modelData

                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(32, utilityTabRow.implicitHeight
                                                         + appTheme.spaceXs)
                        radius: 8
                        color: utilityMouse.pressed
                               ? panel.withAlpha(theme.colHover, 0.34)
                               : (utilityMouse.containsMouse
                                  ? panel.withAlpha(theme.colHover, 0.24)
                                  : "transparent")

                        RowLayout {
                            id: utilityTabRow
                            anchors.fill: parent
                            anchors.leftMargin: 2
                            anchors.rightMargin: 8
                            spacing: 10

                            Image {
                                Layout.preferredWidth: 17
                                Layout.preferredHeight: 17
                                source: modelData.iconSource
                                sourceSize.width: 17
                                sourceSize.height: 17
                                fillMode: Image.PreserveAspectFit
                                mipmap: false
                                smooth: false
                            }

                            Label {
                                Layout.fillWidth: true
                                text: modelData.label
                                color: panel.withAlpha(theme.colText, 0.92)
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: 13
                                font.weight: 600
                                wrapMode: Text.Wrap
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        MouseArea {
                            id: utilityMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                panel.activeUtilityTab = modelData.tabId
                                if (modelData.tabId === "search") {
                                    panel.searchRequested()
                                } else if (modelData.tabId === "background-tasks") {
                                    panel.backgroundTasksRequested()
                                }
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: qsTr("LOCAL FOLDERS")
                    color: panel.withAlpha(theme.colText, 0.5)
                    font.pixelSize: 11
                    font.letterSpacing: 1.2
                    font.weight: 600
                }

                Item { Layout.fillWidth: true }

                Button {
                    id: sortButton
                    implicitWidth: 28
                    implicitHeight: 28
                    Layout.minimumWidth: 28
                    Layout.preferredWidth: 28
                    Layout.maximumWidth: 28
                    Layout.minimumHeight: 28
                    Layout.preferredHeight: 28
                    Layout.maximumHeight: 28
                    Layout.alignment: Qt.AlignVCenter
                    leftPadding: 0
                    rightPadding: 0
                    topPadding: 0
                    bottomPadding: 0
                    leftInset: 0
                    rightInset: 0
                    topInset: 0
                    bottomInset: 0
                    Material.foreground: theme.colText
                    onClicked: panel.sortDescending = !panel.sortDescending

                    background: Rectangle {
                        implicitWidth: 28
                        implicitHeight: 28
                        radius: width / 2
                        color: sortButton.hovered || panel.sortDescending
                               ? panel.withAlpha(theme.colHover, 0.55)
                               : "transparent"
                    }

                    contentItem: Item {
                        implicitWidth: 28
                        implicitHeight: 28
                        Image {
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            source: "qrc:/panel_icons/sort.svg"
                            sourceSize.width: 16
                            sourceSize.height: 16
                            fillMode: Image.PreserveAspectFit
                            mipmap: false
                            smooth: false
                            rotation: panel.sortDescending ? 180 : 0

                            Behavior on rotation {
                                NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                            }
                        }
                    }

                    ToolTip.visible: hovered
                    ToolTip.text: panel.sortDescending ? qsTr("Sorted Z-A") : qsTr("Sorted A-Z")
                }

                Button {
                    id: addCollectionButton
                    objectName: "collectionsAddButton"
                    implicitWidth: 28
                    implicitHeight: 28
                    Layout.minimumWidth: 28
                    Layout.preferredWidth: 28
                    Layout.maximumWidth: 28
                    Layout.minimumHeight: 28
                    Layout.preferredHeight: 28
                    Layout.maximumHeight: 28
                    Layout.alignment: Qt.AlignVCenter
                    leftPadding: 0
                    rightPadding: 0
                    topPadding: 0
                    bottomPadding: 0
                    leftInset: 0
                    rightInset: 0
                    topInset: 0
                    bottomInset: 0
                    enabled: panel.backendInteractive
                    Material.foreground: theme.colText
                    // A new collection goes inside the selected one.
                    onClicked: panel.beginCreateCollection(panel.currentFolderId)

                    background: Rectangle {
                        implicitWidth: 28
                        implicitHeight: 28
                        radius: width / 2
                        color: addCollectionButton.hovered || panel.draftParentFolderId >= 0
                               ? panel.withAlpha(theme.colHover, 0.55)
                               : "transparent"
                    }

                    contentItem: Item {
                        implicitWidth: 28
                        implicitHeight: 28
                        Image {
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            source: "qrc:/panel_icons/folder-plus.svg"
                            sourceSize.width: 16
                            sourceSize.height: 16
                            fillMode: Image.PreserveAspectFit
                            mipmap: false
                            smooth: false
                        }
                    }

                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("New collection")
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: panel.withAlpha(theme.colText, 0.06)
            }

            ListView {
                id: folderList
                objectName: "collectionsTree"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 0
                model: panel.folderRows
                activeFocusOnTab: true
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                Accessible.role: Accessible.Tree
                Accessible.name: qsTr("Collections")

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

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
                    readonly property bool isDraft: modelData.isDraft === true
                    readonly property bool isRoot: !isDraft && Number(modelData.folderId) === 0
                    readonly property bool selected: !isDraft
                        && Number(modelData.folderId) === panel.currentFolderId
                    readonly property bool hovered: !isDraft && treeMouse.hoverIndex === index
                    readonly property bool dropTarget: !isDraft
                        && ((panel.dragActive
                             && Number(modelData.folderId) === panel.dropTargetFolderId)
                            || Number(modelData.folderId) === panel.fileDropFolderId)
                    readonly property bool renaming: !isDraft
                        && Number(modelData.elementId) === panel.renamingElementId
                    readonly property bool editing: isDraft || renaming

                    objectName: isDraft ? "collectionDraftRow"
                                        : "collectionRow_" + String(modelData.elementId)
                    width: ListView.view.width
                    height: shown ? appTheme.collectionRowHeight : 0
                    visible: shown

                    Rectangle {
                        anchors.fill: parent
                        anchors.rightMargin: 2
                        radius: appTheme.controlRadiusSmall
                        // Selected: a quiet neutral well (controlHoveredFillColor) under
                        // unchanged text. Drop target: hover wash plus a 1 px muted outline.
                        // Hover: the darker hoverColor wash.
                        color: folderRow.selected && !folderRow.editing
                               ? appTheme.controlHoveredFillColor
                               : (folderRow.dropTarget || folderRow.hovered)
                                 ? appTheme.hoverColor
                                 : "transparent"
                        border.width: folderRow.dropTarget ? 1 : 0
                        border.color: appTheme.textMutedColor
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: appTheme.spaceXs + panel.rowIndent(folderRow.modelData)
                        anchors.rightMargin: appTheme.spaceSm
                        spacing: appTheme.spaceXs

                        // Disclosure triangle; the same glyphs as the Library section headers.
                        Label {
                            Layout.preferredWidth: appTheme.collectionIndentStep
                            Layout.alignment: Qt.AlignVCenter
                            horizontalAlignment: Text.AlignHCenter
                            text: !folderRow.isRoot && folderRow.modelData.hasChildren === true
                                  ? (panel.isExpanded(folderRow.modelData) ? "▾" : "▸")
                                  : ""
                            color: appTheme.textMutedColor
                            font.pixelSize: appTheme.fontSizeBody
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            visible: !folderRow.editing
                            // Uniform fixed-height tree rows elide (DESIGN.md truncation policy).
                            elide: Text.ElideRight
                            text: folderRow.isRoot ? qsTr("全部图片") : String(folderRow.modelData.name)
                            color: appTheme.textColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeTitle
                            font.weight: folderRow.selected ? appTheme.fontWeightStrong
                                                            : appTheme.fontWeightRegular
                        }

                        // Photo count of the collection; the root row counts the project.
                        Label {
                            readonly property real count: panel.imageCountOf(folderRow.modelData)
                            objectName: folderRow.isDraft
                                        ? ""
                                        : "collectionCount_" + String(folderRow.modelData.elementId)
                            Layout.alignment: Qt.AlignVCenter
                            visible: !folderRow.editing && count >= 0
                            text: count >= 0 ? count.toLocaleString(Qt.locale(), "f", 0) : ""
                            // Selected: brightens to the name ink on the neutral well.
                            color: folderRow.selected ? appTheme.textColor
                                                      : appTheme.textMutedColor
                            font.family: appTheme.dataFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }

                        Loader {
                            Layout.fillWidth: true
                            Layout.preferredHeight: appTheme.collectionRowHeight - 4
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
                    property int pressIndex: -1
                    property point pressPoint: Qt.point(0, 0)
                    property bool suppressClick: false

                    function rowIndexAt(mouseX, mouseY) {
                        return folderList.indexAt(mouseX, mouseY + folderList.contentY)
                    }

                    function onDisclosure(index, mouseX) {
                        const row = panel.folderRows[index]
                        const start = appTheme.spaceXs + panel.rowIndent(row)
                        return mouseX >= start - 2
                               && mouseX <= start + appTheme.collectionIndentStep + 2
                    }

                    onExited: hoverIndex = -1
                    onPressed: function(mouse) {
                        folderList.forceActiveFocus()
                        pressIndex = rowIndexAt(mouse.x, mouse.y)
                        pressPoint = Qt.point(mouse.x, mouse.y)
                        suppressClick = false
                        if (mouse.button === Qt.RightButton) {
                            suppressClick = true
                            const row = pressIndex >= 0 ? panel.folderRows[pressIndex] : null
                            if (row && !row.isDraft && panel.backendInteractive) {
                                rowMenu.targetRow = row
                                rowMenu.popup()
                            }
                        }
                    }
                    onPositionChanged: function(mouse) {
                        hoverIndex = rowIndexAt(mouse.x, mouse.y)
                        if (!(mouse.buttons & Qt.LeftButton) || pressIndex < 0) {
                            return
                        }
                        if (!panel.dragActive) {
                            const dx = mouse.x - pressPoint.x
                            const dy = mouse.y - pressPoint.y
                            const row = panel.folderRows[pressIndex]
                            if (dx * dx + dy * dy > 36 && row && !row.isDraft
                                    && Number(row.folderId) !== 0 && panel.backendInteractive) {
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
                        if (onDisclosure(index, mouse.x) && row.hasChildren === true) {
                            panel.toggleExpanded(row)
                            return
                        }
                        panel.selectRow(row)
                    }
                    onDoubleClicked: function(mouse) {
                        const index = rowIndexAt(mouse.x, mouse.y)
                        if (index >= 0 && !onDisclosure(index, mouse.x)) {
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
        }

        // Floating label of the dragged collection; follows the pointer.
        Rectangle {
            objectName: "collectionDragLabel"
            z: 100
            visible: panel.dragActive && panel.dragSourceRow !== null
            x: panel.dragPoint.x + 12
            y: panel.dragPoint.y - height / 2
            width: Math.min(dragLabelText.implicitWidth + appTheme.spaceSm * 2, collectionsSurface.width * 0.8)
            height: appTheme.collectionRowHeight
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

    Button {
        id: importFromFolderBtn
        Layout.fillWidth: true
        Layout.preferredHeight: 46
        text: qsTr("Import From Folder")
        enabled: backendInteractive
        icon.source: "qrc:/panel_icons/folder-open.svg"
        icon.width: 16
        icon.height: 16
        // Neutral filled action: the light list well with its ink (colours from the Main shell).
        readonly property color inkColor: theme.filledActionInk(enabled)
        icon.color: inkColor
        display: AbstractButton.TextBesideIcon
        onClicked: panel.importFromFolderRequested()

        background: Rectangle {
            radius: 8
            color: theme.filledActionFill(importFromFolderBtn.enabled,
                                          importFromFolderBtn.hovered,
                                          importFromFolderBtn.down)
            Behavior on color {
                enabled: !appTheme.reduceMotion
                ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
            }
        }

        contentItem: IconLabel {
            spacing: importFromFolderBtn.spacing
            mirrored: importFromFolderBtn.mirrored
            display: importFromFolderBtn.display
            icon: importFromFolderBtn.icon
            text: importFromFolderBtn.text
            font: importFromFolderBtn.font
            color: importFromFolderBtn.inkColor
        }
    }

    Button {
        id: importImagesBtn
        Layout.fillWidth: true
        Layout.preferredHeight: 42
        text: qsTr("Import Images")
        enabled: backendInteractive
        icon.source: "qrc:/panel_icons/import.svg"
        icon.width: 14
        icon.height: 14
        icon.color: theme.colText
        display: AbstractButton.TextBesideIcon
        Material.foreground: theme.colText
        onClicked: panel.importRequested()

        background: Rectangle {
            radius: 8
            opacity: importImagesBtn.enabled ? 1.0 : 0.5
            color: theme.secondaryButtonFill(importImagesBtn.enabled,
                                             importImagesBtn.hovered,
                                             importImagesBtn.pressed)
            Behavior on color {
                enabled: !appTheme.reduceMotion
                ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
            }
        }
    }

    AppContextMenu {
        id: rowMenu
        objectName: "collectionRowMenu"
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
