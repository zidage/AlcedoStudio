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
    property var imageContextMenu: null
    property var adjustmentTransferDialog: null
    property var deleteConfirmDialog: null

    property var pendingDeleteTargets: []
    property var pendingRatingTarget: ({})
    // Every image the context menu acts on. A rating picked from the menu goes
    // to all of them when more than one is selected.
    property var pendingRatingTargets: []
    property var pendingAdjustmentSource: ({})
    property var pendingAdjustmentPasteTargets: []
    property var focusedImageTarget: ({})
    property var focusedImageInspection: ({ success: false, tiles: [] })
    // Where the shared image menu was last opened from ("library" |
    // "editor-filmstrip"). The editor filmstrip shows the shared Library
    // selection, but its menu always targets exactly the clicked image.
    property string menuOrigin: "library"
    property bool _pendingEditorLibraryListSync: false
    property bool _pendingEditorLibraryListReveal: false
    // The workspace of the previous route, to detect a return to the editor.
    property string _lastWorkspace: appModules.workspaceRouter
                                    ? String(appModules.workspaceRouter.workspace || "library")
                                    : "library"

    // Push the focused image element id and the pending delete targets into the
    // interaction-policy controller so its cached Q_PROPERTYs (which the inspector
    // edit checks and the delete action bind to) re-evaluate on PolicyChanged.
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
        if (!host.backendInteractive) {
            return
        }
        // The backend reads every photo of the query; a page that still loads does not matter.
        appModules.library.SelectAllInCurrentView()
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
        const targets = appModules.libraryMutations.ResolveTargets(clickedItem, !fromFilmstrip)
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
        root.pendingRatingTargets = targets
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
        const fromProject = appModules.libraryMutations.DeleteScope() === "project"
        let text = ""
        if (count === 1) {
            text = fromProject
                    ? qsTr("Delete this image from project?")
                    : qsTr("Remove this image from this album?")
        } else {
            text = fromProject
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
        return root.rowInCurrentLibraryList(elementId) >= 0
    }

    function rowInCurrentLibraryList(elementId) {
        const target = Number(elementId || 0)
        if (target <= 0 || !appModules || !appModules.library
                || !appModules.library.thumbnailModel) {
            return -1
        }
        const model = appModules.library.thumbnailModel
        if (model.rowByElementId) {
            const loaded = Number(model.rowByElementId(target))
            if (loaded >= 0) {
                return loaded
            }
        }
        if (appModules.library.IndexOfElementInCurrentView) {
            return Number(appModules.library.IndexOfElementInCurrentView(target))
        }
        return -1
    }

    function firstLibraryImage() {
        if (!appModules || !appModules.library || !appModules.library.thumbnailModel) {
            return null
        }
        const model = appModules.library.thumbnailModel
        if (Number(model.count) <= 0) {
            return null
        }
        return root.selectionItemFromThumbnailRow(model.getItemAt(0))
    }

    function revealLibraryListImage(elementId, index) {
        if (!host || Number(elementId || 0) <= 0) {
            return
        }
        if (host.requestFilmstripScrollToElement) {
            host.requestFilmstripScrollToElement(elementId, index)
        }
        if (host.requestLibraryScrollToElement) {
            host.requestLibraryScrollToElement(elementId, index)
        }
    }

    // revealOpenImage: scroll the Library grid and the filmstrip to the open
    // image when it stays. A return from the Library passes false; the strip
    // keeps its own scroll position then.
    function requestEditorLibraryListSync(revealOpenImage) {
        if (!appModules || !appModules.workspaceRouter
                || String(appModules.workspaceRouter.workspace || "") !== "editor") {
            return
        }
        root._pendingEditorLibraryListSync = true
        if (revealOpenImage !== false) {
            root._pendingEditorLibraryListReveal = true
        }
        Qt.callLater(root.flushEditorLibraryListSync)
    }

    function flushEditorLibraryListSync() {
        if (!root._pendingEditorLibraryListSync) {
            return
        }
        if (!appModules || !appModules.workspaceRouter
                || String(appModules.workspaceRouter.workspace || "") !== "editor") {
            root._pendingEditorLibraryListSync = false
            root._pendingEditorLibraryListReveal = false
            return
        }
        // Compare against the accepted result only: wait while a library query
        // or a page read runs.
        const library = appModules.library
        const model = library ? library.thumbnailModel : null
        if ((library && library.queryUpdating) || (model && model.loading)) {
            return
        }
        const session = appModules.editorSession
        const state = session ? String(session.sessionState || "") : ""
        if (state === "Saving" || state === "Switching") {
            return
        }
        if (!root.applyEditorLibraryListSync(root._pendingEditorLibraryListReveal)) {
            // The close waits for the save check; a change of canSelectImage or
            // sessionState flushes again.
            return
        }
        root._pendingEditorLibraryListSync = false
        root._pendingEditorLibraryListReveal = false
    }

    // After collection or filter changes, the editor must follow the same
    // filtered library list the grid uses. Keep the open image when it is
    // still in that list. When the list does not contain it, close it: the
    // editor shows no image and releases the image's resources. The close is
    // an image switch to no image, so it waits for the same save check as a
    // filmstrip switch (actions.canSelectImage), and the session saves the
    // image before it closes. Returns false when the close must wait.
    function applyEditorLibraryListSync(revealOpenImage) {
        if (!appModules || !appModules.workspaceRouter
                || String(appModules.workspaceRouter.workspace || "") !== "editor") {
            return true
        }
        const model = appModules.library ? appModules.library.thumbnailModel : null
        if (!model) {
            return true
        }
        const session = appModules.editorSession
        const currentElementId = session ? Number(session.elementId || 0) : 0
        if (currentElementId <= 0) {
            return true
        }
        const row = root.rowInCurrentLibraryList(currentElementId)
        if (row >= 0) {
            if (revealOpenImage) {
                const item = model.getItemAt ? model.getItemAt(row) : null
                root.revealLibraryListImage(currentElementId, row)
                if (item) {
                    root.setFocusedImage(root.selectionItemFromThumbnailRow(item))
                }
            }
            return true
        }

        if (session.actions && !Boolean(session.actions.canSelectImage)) {
            return false
        }
        if (!appModules.workspaceRouter.openEditor(0, 0)) {
            // Agent control refuses user image switches; the image stays open.
            return true
        }
        root.setFocusedImage(null)
        return true
    }

    // QML handlers run only for signals whose names start with a lower-case
    // letter. The backend signals here (StateChanged, AvailabilityChanged,
    // QueryStateChanged, RouteChanged) start with an upper-case letter, so these
    // Connections use the change handlers of the properties that they notify.
    Connections {
        target: appModules.editorSession
        ignoreUnknownSignals: true
        function onSessionStateChanged() { root.flushEditorLibraryListSync() }
    }

    Connections {
        target: appModules.editorSession ? appModules.editorSession.actions : null
        ignoreUnknownSignals: true
        function onCanSelectImageChanged() { root.flushEditorLibraryListSync() }
    }

    // Every accepted library result (filter, search, collection, or sort
    // change) checks the open image against the new list. The editor keeps
    // its filmstrip scroll position: no reveal.
    Connections {
        target: appModules.library
        ignoreUnknownSignals: true
        function onQueryUpdatingChanged() {
            if (appModules.library && !appModules.library.queryUpdating) {
                root.requestEditorLibraryListSync(false)
            }
            root.flushEditorLibraryListSync()
        }
    }

    Connections {
        target: appModules.library.thumbnailModel
        ignoreUnknownSignals: true
        function onLoadingChanged() { root.flushEditorLibraryListSync() }
    }

    // A filter change in the Library does not sync the editor (it is hidden).
    // When the user returns to the editor, check the open image against the
    // filtered list.
    Connections {
        target: appModules.workspaceRouter
        ignoreUnknownSignals: true
        function onWorkspaceChanged() {
            const workspace = String(appModules.workspaceRouter.workspace || "library")
            const entered = workspace === "editor" && root._lastWorkspace !== "editor"
            root._lastWorkspace = workspace
            if (entered) {
                root.requestEditorLibraryListSync(false)
            }
        }
    }

    function runDeleteTargets() {
        if (!root.pendingDeleteTargets || root.pendingDeleteTargets.length === 0) {
            return
        }
        // The delete runs on a worker as a background task. When it starts, a deleted editor
        // image closes; when it is complete, the backend prunes the selection and the export
        // queue, and onDeleteTargetsFinished below clears a deleted focused image.
        const result = appModules.libraryMutations.StartDeleteTargetsAsUser(
            root.pendingDeleteTargets)
        if (result && result.started !== true && result.message) {
            host.showSnackbar(result.message)
        }
        root.pendingDeleteTargets = []
    }

    Connections {
        target: appModules.libraryMutations
        function onDeleteTargetsFinished(result) { root.handleDeleteFinished(result) }
    }

    function handleDeleteFinished(result) {
        if (result && result.success === false && result.message) {
            host.showSnackbar(result.message)
        }
        const deletedIds = (result && result.deletedElementIds) ? result.deletedElementIds : []
        if (deletedIds.length > 0) {
            const focusedElementId = Number(root.focusedImageTarget.elementId || 0)
            const focusedFileId = Number(root.focusedImageTarget.fileId || 0)
            for (let i = 0; i < deletedIds.length; ++i) {
                const deletedId = Number(deletedIds[i])
                if (deletedId === focusedElementId || deletedId === focusedFileId) {
                    root.setFocusedImage(null)
                    break
                }
            }
        }
    }

    function requestSetImageRating(rating) {
        if (!root.pendingRatingTarget || Number(root.pendingRatingTarget.imageId) <= 0) {
            return
        }
        // A slider produces only valid values; the clamp keeps the old QML behavior.
        const normalizedRating = Math.max(0, Math.min(5, Number(rating)))
        // A rating picked from the menu goes to every target when several are selected.
        const targets = root.pendingRatingTargets && root.pendingRatingTargets.length > 1
                ? root.pendingRatingTargets
                : [root.pendingRatingTarget]
        const result = appModules.libraryMutations.RateTargetsAsUser(targets, normalizedRating)
        if (result && result.batch === true) {
            // Batch path: the library shows the new stars at once; the save runs as a
            // background task and onImageRatingsFinished reports the result.
            if (result.started === true) {
                root.pendingRatingTarget = Object.assign({}, root.pendingRatingTarget, {
                    rating: normalizedRating
                })
                return
            }
            if (result.message) {
                host.showSnackbar(result.message)
            }
            return
        }
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

    Connections {
        target: appModules.images
        ignoreUnknownSignals: true
        function onImageRatingsFinished(result) {
            const focusedElementId = Number(root.focusedImageTarget.elementId || 0)
            const ratedIds = result && result.ratedElementIds ? result.ratedElementIds : []
            const failedIds = result && result.failedElementIds ? result.failedElementIds : []
            const touched = ratedIds.concat(failedIds)
            for (let i = 0; i < touched.length; ++i) {
                if (focusedElementId > 0 && Number(touched[i]) === focusedElementId) {
                    root.refreshFocusedImageInspection()
                    break
                }
            }
            if (result && result.message) {
                host.showSnackbar(result.message)
            }
        }
    }

    // A star click on a Library card rates that one photo, not the selection, after the
    // interaction policy check (an AI scoring run or a rating batch can hold the rating).
    function requestSetCardImageRating(item, rating) {
        const elementId = Number(item ? (item.elementId || item.fileId || 0) : 0)
        const imageId = Number(item ? (item.imageId || 0) : 0)
        if (elementId <= 0 || imageId <= 0) {
            return
        }
        const policy = appModules.interactionPolicy.EvaluateEditImageRating(elementId)
        if (policy && policy.allowed === false) {
            if (policy.reason) {
                host.showSnackbar(policy.reason)
            }
            return
        }
        const normalizedRating = Math.max(0, Math.min(5, Number(rating)))
        const result = appModules.libraryMutations.RateTargetsAsUser([{
            elementId: elementId,
            imageId: imageId
        }], normalizedRating)
        if (result && result.success === true
                && Number(root.focusedImageTarget.imageId || 0) === imageId) {
            root.focusedImageTarget = Object.assign({}, root.focusedImageTarget, {
                rating: Number(result.rating)
            })
            root.refreshFocusedImageInspection()
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
        const result = appModules.libraryMutations.RateTargetsAsUser([{
            elementId: Number(root.focusedImageTarget.elementId || root.focusedImageTarget.fileId),
            imageId: Number(root.focusedImageTarget.imageId)
        }], normalizedRating)
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
        adjustmentTransferDialog.open()
    }
}