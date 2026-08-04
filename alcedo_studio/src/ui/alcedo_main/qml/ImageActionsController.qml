import QtQuick
import QtQml

// Album/image interaction logic extracted from Main.qml: focused-image
// inspection, delete/rating workflows, adjustment copy/paste, and the
// context-menu assembly. Stateless — all mutable state stays on the host
// (Main) and is reached via `host.*`; the selection/export state objects and
// the dialogs are passed in as properties so bare references resolve locally.
// appModules and languageManager are global context properties.
Item {
    id: root

    property var host: null
    property var selectionState: null
    property var exportQueueState: null
    property var imageContextMenu: null
    property var adjustmentTransferDialog: null
    property var deleteConfirmDialog: null

    property var pendingDeleteTargets: []
    property var pendingRatingTarget: ({})
    property var pendingAdjustmentSource: ({})
    property var pendingAdjustmentPasteTargets: []
    property var focusedImageTarget: ({})
    property var focusedImageInspection: ({ success: false, tiles: [] })
    // Where the shared image menu was last opened from ("library" |
    // "editor-filmstrip"). The editor filmstrip has no multi-selection surface,
    // so its menu always targets exactly the clicked image.
    property string menuOrigin: "library"

    // Push the focused image element id and the pending delete targets into the
    // interaction-policy controller so its cached Q_PROPERTYs (which the inspector
    // edit gates and the delete action bind to) re-evaluate on PolicyChanged.
    // focusedImageTarget is the reliable elementId source (focusedImageInspection
    // may omit it on the success branch).
    Binding {
        target: appModules.interactionPolicy
        property: "focusedElementId"
        value: root.focusedImageTarget ? Number(root.focusedImageTarget.elementId || 0) : 0
    }
    Binding {
        target: appModules.interactionPolicy
        property: "pendingDeleteTargets"
        value: root.pendingDeleteTargets
    }

    function selectionItemFromThumbnailRow(row) {
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
            folderId: Number(row.folderId || appModules.folders.currentFolderId),
            scopeType: row.scopeType ? String(row.scopeType) : "",
            fileName: row.fileName ? row.fileName : qsTr("(unnamed)"),
            rating: Number(row.rating),
            isHdr: row.isHdr === true
        }
    }

    function setFocusedImage(item) {
        if (!item || Number(item.imageId) <= 0) {
            root.focusedImageTarget = ({})
            root.focusedImageInspection = ({ success: false, tiles: [] })
            return
        }
        root.focusedImageTarget = {
            elementId: Number(item.elementId || item.fileId),
            fileId: Number(item.fileId || item.elementId),
            imageId: Number(item.imageId),
            folderId: Number(item.folderId || appModules.folders.currentFolderId),
            scopeType: item.scopeType ? String(item.scopeType) : "",
            fileName: item.fileName ? item.fileName : qsTr("(unnamed)"),
            rating: Number(item.rating || 0),
            isHdr: item.isHdr === true
        }
        root.refreshFocusedImageInspection()
    }

    function refreshFocusedImageInspection() {
        if (!root.focusedImageTarget || Number(root.focusedImageTarget.imageId) <= 0) {
            root.focusedImageInspection = ({ success: false, tiles: [] })
            return
        }
        const result = appModules.images.GetFocusedImageInspection(
            Number(root.focusedImageTarget.elementId || root.focusedImageTarget.fileId),
            Number(root.focusedImageTarget.imageId))
        if (!result || result.success !== true) {
            root.focusedImageInspection = Object.assign({}, root.focusedImageTarget, {
                success: false,
                tiles: []
            })
            if (result && result.message) {
                host.showSnackbar(result.message)
            }
            return
        }
        root.focusedImageInspection = result
        root.focusedImageTarget = Object.assign({}, root.focusedImageTarget, {
            fileId: Number(result.fileId || result.elementId || root.focusedImageTarget.fileId),
            imageId: Number(result.imageId || root.focusedImageTarget.imageId),
            fileName: result.title ? String(result.title) : root.focusedImageTarget.fileName,
            rating: Number(result.rating || 0)
        })
    }

    function analysisResultTouchesFocusedImage() {
        if (!root.focusedImageTarget || Number(root.focusedImageTarget.imageId) <= 0
                || !appModules.imageAnalysis
                || !appModules.imageAnalysis.lastResults) {
            return false
        }
        const results = appModules.imageAnalysis.lastResults
        const targetElementId = Number(root.focusedImageTarget.elementId || 0)
        const targetFileId = Number(root.focusedImageTarget.fileId || 0)
        const targetImageId = Number(root.focusedImageTarget.imageId || 0)
        for (let i = 0; i < results.length; ++i) {
            const row = results[i]
            if (!row) {
                continue
            }
            const elementId = Number(row.elementId || 0)
            const imageId = Number(row.imageId || 0)
            if ((elementId > 0 && (elementId === targetElementId || elementId === targetFileId))
                    || (imageId > 0 && imageId === targetImageId)) {
                return true
            }
        }
        return false
    }

    function selectAllCurrentAlbum() {
        if (!host.backendInteractive || appModules.library.thumbnailModel.loading) {
            return
        }
        const total = Number(appModules.library.totalCount)
        if (total <= 0) {
            selectionState.clearSelectedImages()
            return
        }

        appModules.library.LoadThumbnailsThroughIndex(total - 1)
        const rows = appModules.library.thumbnailModel.getItemsInRange(0, total - 1)
        const items = []
        for (let i = 0; i < rows.length; ++i) {
            const item = root.selectionItemFromThumbnailRow(rows[i])
            if (item) {
                items.push(item)
            }
        }
        selectionState.replaceSelectedImages(items)
    }

    function singleTargetFor(item) {
        return [{
            elementId: Number(item.elementId),
            fileId: Number(item.fileId || item.elementId),
            imageId: Number(item.imageId),
            folderId: Number(item.folderId || appModules.folders.currentFolderId),
            scopeType: item.scopeType ? String(item.scopeType) : "",
            fileName: item.fileName ? item.fileName : qsTr("(unnamed)")
        }]
    }

    function resolveDeleteTargets(clickedItem) {
        if (host.selectedCount > 0) {
            return Object.values(host.selectedImagesById)
        }
        if (!clickedItem) {
            return []
        }
        return singleTargetFor(clickedItem)
    }

    function albumTargetActions() {
        const rows = appModules.folders.folders ? appModules.folders.folders : []
        const actions = []
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (!row) {
                continue
            }
            const folderId = Number(row.folderId)
            if (folderId === 0 || folderId === Number(appModules.folders.currentFolderId)) {
                continue
            }
            actions.push({
                id: "add-to-album:" + String(folderId),
                label: qsTr("Add to %1").arg(row.name ? String(row.name) : qsTr("Album")),
                enabled: root.pendingDeleteTargets.length > 0
            })
        }
        return actions
    }

    function openImageContextMenu(clickedItem, sceneX, sceneY, origin) {
        if (!host.backendInteractive) {
            return
        }
        if (!clickedItem) {
            return
        }
        const fromFilmstrip = origin === "editor-filmstrip"
        root.menuOrigin = fromFilmstrip ? "editor-filmstrip" : "library"
        const targets = fromFilmstrip ? singleTargetFor(clickedItem)
                                      : resolveDeleteTargets(clickedItem)
        if (!targets || targets.length === 0) {
            return
        }
        root.setFocusedImage(clickedItem)
        root.pendingDeleteTargets = targets
        const ratingResult = appModules.images.GetImageRating(
            Number(clickedItem.elementId),
            Number(clickedItem.imageId))
        root.pendingRatingTarget = {
            elementId: Number(clickedItem.elementId),
            fileId: Number(clickedItem.fileId || clickedItem.elementId),
            imageId: Number(clickedItem.imageId),
            fileName: clickedItem.fileName ? clickedItem.fileName : qsTr("(unnamed)"),
            rating: ratingResult && ratingResult.success === true
                    ? Number(ratingResult.rating)
                    : Number(clickedItem.rating || 0)
        }
        root.pendingAdjustmentSource = {
            elementId: Number(clickedItem.elementId),
            fileId: Number(clickedItem.fileId || clickedItem.elementId),
            imageId: Number(clickedItem.imageId),
            fileName: clickedItem.fileName ? clickedItem.fileName : qsTr("(unnamed)")
        }
        root.pendingAdjustmentPasteTargets = targets
        imageContextMenu.openAt(sceneX, sceneY)
    }

    function requestDeleteConfirmation() {
        const count = root.pendingDeleteTargets.length
        if (count <= 0) {
            return
        }
        let text = ""
        if (count === 1) {
            text = Number(appModules.folders.currentFolderId) === 0
                    ? qsTr("Delete this image from project?")
                    : qsTr("Remove this image from this album?")
        } else {
            text = Number(appModules.folders.currentFolderId) === 0
                    ? qsTr("Delete %1 images from project?").arg(count)
                    : qsTr("Remove %1 images from this album?").arg(count)
        }
        deleteConfirmDialog.openWith(text)
    }

    function runAddTargetsToAlbum(targetFolderId) {
        if (!root.pendingDeleteTargets || root.pendingDeleteTargets.length === 0) {
            return
        }
        const result = appModules.images.AddImagesToFolder(root.pendingDeleteTargets, Number(targetFolderId))
        if (result && result.message) {
            host.showSnackbar(result.message)
        }
    }

    // Phase 4A-Fix: a deleted image is only "still in the library" if the
    // current folder's thumbnail model still lists it. The check is deliberately
    // folder-scoped (the editor filmstrip will be the current library list, so a
    // restored image must be in view); a global existence query belongs with the
    // Phase 5B first-frame loader.
    function editorImageStillExists(elementId) {
        if (!appModules || !appModules.library || !appModules.library.thumbnailModel) {
            return false
        }
        return appModules.library.thumbnailModel.rowByElementId(Number(elementId)) >= 0
    }

    // Phase 4A-Fix: deleting the image currently loaded in the editor must end
    // that image's session, clear its id, and drop the editor to the empty state
    // while staying in the editor workspace. It also forgets the last-edited
    // image so re-entering the editor does not resurrect a deleted image.
    function handleEditorImageDeleted(deletedIds) {
        if (!appModules.editorSession || !appModules.workspaceRouter) {
            return
        }
        // Only the editor workspace owns the live edit session; a delete issued
        // from the library must not touch the (empty) editor state.
        if (appModules.workspaceRouter.workspace !== "editor") {
            return
        }
        const editorElementId = Number(appModules.editorSession.elementId || 0)
        if (editorElementId <= 0) {
            return
        }
        let touchedEditor = false
        for (let i = 0; i < deletedIds.length; ++i) {
            if (Number(deletedIds[i]) === editorElementId) {
                touchedEditor = true
                break
            }
        }
        if (!touchedEditor) {
            return
        }
        // openEditor(0,0) while already in editor finalizes the active session
        // and reopens with no image (active + hasImage=false), so the empty-state
        // prompt shows without tearing the editor workspace down.
        appModules.editorSession.clearLastEditedImage()
        appModules.workspaceRouter.openEditor(0, 0)
    }

    function runDeleteTargets() {
        if (!root.pendingDeleteTargets || root.pendingDeleteTargets.length === 0) {
            return
        }
        const result = appModules.images.DeleteImages(root.pendingDeleteTargets)
        const deletedIds = (result && result.deletedElementIds) ? result.deletedElementIds : []
        if (deletedIds.length > 0) {
            selectionState.pruneDeletedElements(deletedIds)
            exportQueueState.pruneDeletedElements(deletedIds)
            const focusedElementId = Number(root.focusedImageTarget.elementId || 0)
            const focusedFileId = Number(root.focusedImageTarget.fileId || 0)
            for (let i = 0; i < deletedIds.length; ++i) {
                const deletedId = Number(deletedIds[i])
                if (deletedId === focusedElementId || deletedId === focusedFileId) {
                    root.setFocusedImage(null)
                    break
                }
            }
            root.handleEditorImageDeleted(deletedIds)
        }
        root.pendingDeleteTargets = []
    }

    function requestSetImageRating(rating) {
        if (!root.pendingRatingTarget || Number(root.pendingRatingTarget.imageId) <= 0) {
            return
        }
        const normalizedRating = Math.max(0, Math.min(5, Number(rating)))
        const result = appModules.images.SetImageRating(
            Number(root.pendingRatingTarget.elementId),
            Number(root.pendingRatingTarget.imageId),
            normalizedRating)
        if (result && result.success === true) {
            root.pendingRatingTarget = Object.assign({}, root.pendingRatingTarget, {
                rating: Number(result.rating)
            })
            if (Number(root.focusedImageTarget.imageId || 0)
                    === Number(root.pendingRatingTarget.imageId || 0)) {
                root.refreshFocusedImageInspection()
            }
        }
        if (result && result.message) {
            host.showSnackbar(result.message)
        }
    }

    function requestSetFocusedImageRating(rating) {
        if (!root.focusedImageTarget || Number(root.focusedImageTarget.imageId) <= 0) {
            return
        }
        const normalizedRating = Math.max(0, Math.min(5, Number(rating)))
        const result = appModules.images.SetImageRating(
            Number(root.focusedImageTarget.elementId || root.focusedImageTarget.fileId),
            Number(root.focusedImageTarget.imageId),
            normalizedRating)
        if (result && result.success === true) {
            root.focusedImageTarget = Object.assign({}, root.focusedImageTarget, {
                rating: Number(result.rating)
            })
            root.refreshFocusedImageInspection()
        }
        if (result && result.message) {
            host.showSnackbar(result.message)
        }
    }

    function requestSaveFocusedDescription(caption) {
        if (!root.focusedImageTarget || Number(root.focusedImageTarget.fileId
                || root.focusedImageTarget.elementId) <= 0) {
            return
        }
        const result = appModules.images.SetImageDescription(
            Number(root.focusedImageTarget.fileId || root.focusedImageTarget.elementId),
            String(caption || ""))
        if (result && result.success === true) {
            root.refreshFocusedImageInspection()
        }
        if (result && result.message) {
            host.showSnackbar(result.message)
        }
    }

    function requestSaveFocusedRatingReason(reasons) {
        if (!root.focusedImageTarget || Number(root.focusedImageTarget.fileId
                || root.focusedImageTarget.elementId) <= 0) {
            return
        }
        const result = appModules.images.SetImageRatingReasons(
            Number(root.focusedImageTarget.fileId || root.focusedImageTarget.elementId),
            String(reasons || ""))
        if (result && result.success === true) {
            root.refreshFocusedImageInspection()
        }
        if (result && result.message) {
            host.showSnackbar(result.message)
        }
    }

    function requestCopyAdjustments() {
        if (!root.pendingAdjustmentSource || Number(root.pendingAdjustmentSource.elementId) <= 0) {
            return
        }
        const result = appModules.adjustmentTransfer.PrepareCopy(
            Number(root.pendingAdjustmentSource.elementId))
        if (!result || result.success !== true) {
            if (result && result.message) {
                host.showSnackbar(result.message)
            }
            return
        }
        adjustmentTransferDialog.mode = "copy"
        adjustmentTransferDialog.sourceTitle = result.sourceTitle ? String(result.sourceTitle) : ""
        adjustmentTransferDialog.sourceVersions = result.versions ? result.versions : []
        adjustmentTransferDialog.selectedSourceVersionId =
            result.activeVersionId ? String(result.activeVersionId) : ""
        adjustmentTransferDialog.targetCount = 0
        adjustmentTransferDialog.adjustmentRows = result.items ? result.items : []
        adjustmentTransferDialog.open()
    }
}