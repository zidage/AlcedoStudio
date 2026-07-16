import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Effects

ApplicationWindow {
    id: root
    width: 1200
    height: 760
    minimumWidth: 960
    minimumHeight: 640
    visible: true
    visibility: Window.Windowed
    title: qsTr("Alcedo Studio")
    flags: Qt.Window | Qt.FramelessWindowHint
    font.family: appTheme.uiFontFamily

    readonly property bool windowMaximized: visibility === Window.Maximized || visibility === Window.FullScreen
    readonly property real maximizedInset: 0
    // Snap radius — animating it together with the OS resize causes layout jitter.
    readonly property real windowCornerRadius: windowMaximized ? 0 : 12

    // Theme palette — borderless, luminance-separated zones
    readonly property color toneGold: appTheme.toneGold
    readonly property color toneWine: appTheme.toneWine
    readonly property color toneSteel: appTheme.toneSteel
    readonly property color toneGraphite: appTheme.toneGraphite
    readonly property color toneMist: appTheme.toneMist
    readonly property color toneAmber: appTheme.toneGold
    readonly property color toneAccentSecondary: appTheme.accentSecondaryColor

    readonly property color colBgDeep: appTheme.bgDeepColor        // floating modals / popovers — topmost layer
    readonly property color colBgBase: appTheme.bgBaseColor        // sunken inputs
    readonly property color colBgPanel: appTheme.bgPanelColor      // side panels & header/footer
    readonly property color colBgCanvas: appTheme.bgCanvasColor    // gap / outer canvas behind blocks
    readonly property int panelRadius: appTheme.panelRadius        // uniform rounded-corner radius
    readonly property color colBorder: "transparent"     // NO borders by default
    readonly property color colText: appTheme.textColor
    readonly property color colTextMuted: appTheme.textMutedColor
    readonly property color colAccentPrimary: appTheme.accentColor
    readonly property color colAccentSecondary: appTheme.accentSecondaryColor
    readonly property color colAccentSoft: appTheme.accentColor
    readonly property color colDanger: appTheme.dangerColor
    readonly property color colDangerTint: appTheme.dangerTintColor
    readonly property color colSelectedTint: appTheme.selectedTintColor
    readonly property color colHover: appTheme.hoverColor          // subtle hover tint
    readonly property color colDivider: appTheme.dividerColor
    readonly property color colGlassPanel: appTheme.glassPanelColor
    readonly property color colGlassStroke: appTheme.glassStrokeColor
    readonly property color colOverlay: appTheme.overlayColor
    readonly property string dataFontFamily: appTheme.dataFontFamily
    readonly property string headlineFontFamily: appTheme.headlineFontFamily
    readonly property int controlRadius: 10
    readonly property color colButtonPrimary: "#457B9D"
    readonly property color colButtonSecondary: "#3A3F44"
    readonly property color colButtonHighlight: "#E9C46A"
    readonly property color colButtonBorder: Qt.rgba(
        colButtonHighlight.r,
        colButtonHighlight.g,
        colButtonHighlight.b,
        0.20)
    readonly property color colButtonSecondaryBorder: Qt.rgba(
        root.colText.r,
        root.colText.g,
        root.colText.b,
        0.12)

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    function nauticalButtonFill(enabled, hovered, pressed) {
        if (!enabled) {
            return withAlpha(colButtonPrimary, 0.45)
        }
        if (pressed) {
            return Qt.darker(colButtonPrimary, 1.18)
        }
        if (hovered) {
            return Qt.lighter(colButtonPrimary, 1.08)
        }
        return colButtonPrimary
    }

    function secondaryButtonFill(enabled, hovered, pressed) {
        if (!enabled) {
            return withAlpha(colButtonSecondary, 0.55)
        }
        if (pressed) {
            return Qt.darker(colButtonSecondary, 1.14)
        }
        if (hovered) {
            return Qt.lighter(colButtonSecondary, 1.08)
        }
        return colButtonSecondary
    }

    Material.theme: Material.Dark
    Material.primary: root.colAccentSecondary
    Material.accent: root.colAccentPrimary
    Material.background: root.colBgPanel
    Material.foreground: root.colText
    // Transparent root surface so DWM does not draw a frame/border around our rounded content.
    color: "transparent"

    property bool inspectorVisible: true
    property real inspectorWidth: 300
    readonly property real inspectorMinWidth: 300
    readonly property real inspectorMaxWidth: 600
    readonly property real leftPaneWidth: 276
    readonly property real centerPaneMinWidth: 560
    readonly property real mainFrameHorizontalMargins: 24
    readonly property real contentRowSpacingTotal: 36
    readonly property real inspectorAdaptiveMaxWidth: Math.max(
        0,
        root.width
        - leftPaneWidth
        - centerPaneMinWidth
        - mainFrameHorizontalMargins
        - contentRowSpacingTotal
        - 5)
    property bool gridMode: true
    readonly property int defaultGridZoomLevel: 4
    property int gridZoomLevel: defaultGridZoomLevel  // 0..7, maps to column counts 2/3/4/5/6/8/11/14
    readonly property bool backendInteractive: appModules.project.serviceReady
                                               && !appModules.project.projectLoading
                                               && !appModules.project.acceleratorPreparing
    readonly property var selectedImagesById: selectionState.selectedImagesById
    readonly property var exportQueueById: exportQueueState.exportQueueById
    readonly property var exportPreviewRows: exportQueueState.exportPreviewRows
    readonly property var semanticGeneration: appModules.semanticGeneration
    readonly property int selectedCount: selectionState.selectedCount
    readonly property int exportQueueCount: exportQueueState.exportQueueCount
    readonly property var languageOptions: languageManager.availableLanguages
    property var pendingDeleteTargets: []
    property var pendingRatingTarget: ({})
    property var pendingAdjustmentSource: ({})
    property var pendingAdjustmentPasteTargets: []
    property var focusedImageTarget: ({})
    property var focusedImageInspection: ({ success: false, tiles: [] })
    property string deleteConfirmText: ""
    property string snackbarText: ""
    property bool importSessionObserved: false
    property bool exportSessionObserved: false
    property int lastObservedExportCompleted: 0
    property bool projectLaunchPending: false
    property bool welcomeDismissedForLaunch: false
    readonly property bool projectLoadingOverlayVisible: root.projectLaunchPending || appModules.project.projectLoading
    readonly property bool projectLaunchBusy: root.projectLoadingOverlayVisible || root.pendingProjectLaunchAction !== null
    property var pendingProjectLaunchAction: null
    property bool restoreWelcomeOnProjectLaunchFailure: false
    onWelcomeDismissedForLaunchChanged: updateWelcomeDialogVisibility()

    // Phase 2: push the focused image element id and the pending delete targets
    // into the interaction-policy controller so its cached Q_PROPERTYs (which the
    // inspector edit gates and the delete action bind to) re-evaluate on
    // PolicyChanged. focusedImageTarget is the reliable elementId source
    // (focusedImageInspection may omit it on the success branch).
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

    Component.onCompleted: {
        updateWelcomeDialogVisibility()
        acceleratorPreparationStartTimer.start()
    }

    Timer {
        id: acceleratorPreparationStartTimer
        interval: 16
        repeat: false
        onTriggered: appModules.project.StartAcceleratorPreparation()
    }

    Timer {
        id: projectLaunchTimer
        interval: 16
        repeat: false
        onTriggered: {
            const loadAction = root.pendingProjectLaunchAction
            root.pendingProjectLaunchAction = null
            const started = loadAction ? loadAction() : false
            if (!started && !appModules.project.projectLoading) {
                root.projectLaunchPending = false
                if (root.restoreWelcomeOnProjectLaunchFailure) {
                    root.welcomeDismissedForLaunch = false
                }
                root.updateWelcomeDialogVisibility()
            }
            root.restoreWelcomeOnProjectLaunchFailure = false
        }
    }

    function showSnackbar(messageText) {
        if (!messageText || String(messageText).trim().length === 0) {
            return
        }
        root.snackbarText = String(messageText)
        snackbarTimer.restart()
        if (!notificationSnackbar.opened) {
            notificationSnackbar.open()
        }
    }

    function requestSaveProject() {
        const ok = appModules.project.SaveProject()
        if (ok) {
            showSnackbar(appModules.project.serviceMessage)
        }
    }

    function languageIndexForCode(code) {
        for (let i = 0; i < languageOptions.length; ++i) {
            if (languageOptions[i].code === code) {
                return i
            }
        }
        return 0
    }

    function openSettingsDialog(category) {
        settingsDialog.requestedCategory = category === undefined ? 0 : category
        settingsDialog.open()
    }

    function openAdvancedAnalysisDialog() {
        const targets = selectionState.currentSelectedItems()
        advancedContentAnalysisDialog.openWithTargets(targets)
        if (targets.length <= 0) {
            root.showSnackbar(qsTr("Select at least one image to analyze."))
        }
    }

    function dismissWelcomeForProjectLaunch() {
        root.welcomeDismissedForLaunch = true
    }

    function beginProjectLaunch(loadAction) {
        if (appModules.project.acceleratorPreparing) {
            return
        }
        root.restoreWelcomeOnProjectLaunchFailure = !appModules.project.serviceReady
        root.pendingProjectLaunchAction = loadAction
        root.dismissWelcomeForProjectLaunch()
        root.updateWelcomeDialogVisibility()
        if (!welcomeDialog.opened && !welcomeDialog.visible) {
            root.startPendingProjectLaunch()
        }
    }

    function startPendingProjectLaunch() {
        if (!root.pendingProjectLaunchAction) {
            return
        }
        root.projectLaunchPending = true
        projectLaunchTimer.restart()
    }

    function updateWelcomeDialogVisibility() {
        const shouldShowWelcome = !root.welcomeDismissedForLaunch
                                  && !appModules.project.serviceReady
                                  && !appModules.project.projectLoading
        if (shouldShowWelcome) {
            if (!welcomeDialog.opened) {
                welcomeDialog.open()
            }
        } else if (welcomeDialog.opened) {
            welcomeDialog.close()
        }
    }

    function selectionShortcutBlocked() {
        return exportDialog.opened
               || settingsDialog.opened
               || adjustmentTransferDialog.opened
               || nikonHeRecoveryDialog.opened
               || semanticGenerationDialog.opened
               || advancedContentAnalysisDialog.opened
               || deleteConfirmDialog.opened
               || welcomeDialog.opened
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
                root.showSnackbar(result.message)
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
        if (!root.backendInteractive || appModules.library.thumbnailModel.loading) {
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

    function resolveDeleteTargets(clickedItem) {
        if (root.selectedCount > 0) {
            return Object.values(root.selectedImagesById)
        }
        if (!clickedItem) {
            return []
        }
        return [{
            elementId: Number(clickedItem.elementId),
            fileId: Number(clickedItem.fileId || clickedItem.elementId),
            imageId: Number(clickedItem.imageId),
            folderId: Number(clickedItem.folderId || appModules.folders.currentFolderId),
            scopeType: clickedItem.scopeType ? String(clickedItem.scopeType) : "",
            fileName: clickedItem.fileName ? clickedItem.fileName : qsTr("(unnamed)")
        }]
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

    function openImageContextMenu(clickedItem, sceneX, sceneY) {
        if (!root.backendInteractive) {
            return
        }
        if (!clickedItem) {
            return
        }
        const targets = resolveDeleteTargets(clickedItem)
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
        if (count === 1) {
            root.deleteConfirmText = Number(appModules.folders.currentFolderId) === 0
                    ? qsTr("Delete this image from project?")
                    : qsTr("Remove this image from this album?")
        } else {
            root.deleteConfirmText = Number(appModules.folders.currentFolderId) === 0
                    ? qsTr("Delete %1 images from project?").arg(count)
                    : qsTr("Remove %1 images from this album?").arg(count)
        }
        deleteConfirmDialog.open()
    }

    function runAddTargetsToAlbum(targetFolderId) {
        if (!root.pendingDeleteTargets || root.pendingDeleteTargets.length === 0) {
            return
        }
        const result = appModules.images.AddImagesToFolder(root.pendingDeleteTargets, Number(targetFolderId))
        if (result && result.message) {
            root.showSnackbar(result.message)
        }
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
            root.showSnackbar(result.message)
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
            root.showSnackbar(result.message)
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
            root.showSnackbar(result.message)
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
            root.showSnackbar(result.message)
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
                root.showSnackbar(result.message)
            }
            return
        }
        adjustmentTransferDialog.mode = "copy"
        adjustmentTransferDialog.sourceTitle = result.sourceTitle ? String(result.sourceTitle) : ""
        adjustmentTransferDialog.targetCount = 0
        adjustmentTransferDialog.adjustmentRows = result.items ? result.items : []
        adjustmentTransferDialog.open()
    }

    function requestPasteAdjustments() {
        if (!appModules.adjustmentTransfer.packageAvailable) {
            return
        }
        if (!root.pendingAdjustmentPasteTargets || root.pendingAdjustmentPasteTargets.length === 0) {
            return
        }
        adjustmentTransferDialog.mode = "paste"
        adjustmentTransferDialog.pasteStrategy = "merge"
        adjustmentTransferDialog.sourceTitle =
                appModules.adjustmentTransfer.packageSourceTitle
        adjustmentTransferDialog.targetCount = root.pendingAdjustmentPasteTargets.length
        adjustmentTransferDialog.adjustmentRows =
                appModules.adjustmentTransfer.packageSummary
        adjustmentTransferDialog.open()
    }

    ExportQueueState {
        id: exportQueueState
    }

    QtObject {
        id: selectionState
        property var selectedImagesById: ({})
        readonly property int selectedCount: Object.keys(selectedImagesById).length

        function keyForElement(elementId) {
            return String(Number(elementId))
        }

        function setImageSelected(elementId, imageId, fileName, isHdr, selected) {
            const key = keyForElement(elementId)
            const already = Object.prototype.hasOwnProperty.call(selectedImagesById, key)
            if (selected === already) {
                return
            }

            const next = Object.assign({}, selectedImagesById)
            if (selected) {
                next[key] = {
                    elementId: Number(elementId),
                    fileId: Number(elementId),
                    imageId: Number(imageId),
                    fileName: fileName ? fileName : qsTr("(unnamed)"),
                    isHdr: isHdr === true
                }
            } else {
                delete next[key]
            }
            selectedImagesById = next
        }

        function clearSelectedImages() {
            selectedImagesById = ({})
        }

        function replaceSelectedImages(items) {
            const next = {}
            for (let i = 0; i < items.length; ++i) {
                const item = items[i]
                const key = keyForElement(item.elementId)
                next[key] = {
                    elementId: Number(item.elementId),
                    fileId: Number(item.fileId || item.elementId),
                    imageId: Number(item.imageId),
                    fileName: item.fileName ? item.fileName : qsTr("(unnamed)"),
                    isHdr: item.isHdr === true
                }
            }
            selectedImagesById = next
        }

        function currentSelectedItems() {
            const rows = Object.values(selectedImagesById)
            const items = []
            for (let i = 0; i < rows.length; ++i) {
                const item = rows[i]
                const rowIndex = appModules.library.thumbnailModel.rowByElementId(Number(item.elementId))
                if (rowIndex >= 0) {
                    const current = appModules.library.thumbnailModel.getItemAt(rowIndex)
                    if (current && Number(current.elementId) === Number(item.elementId)) {
                        items.push({
                            elementId: Number(current.elementId),
                            fileId: Number(current.fileId || current.elementId),
                            imageId: Number(current.imageId),
                            fileName: current.fileName ? current.fileName : qsTr("(unnamed)"),
                            isHdr: current.isHdr === true
                        })
                        continue
                    }
                }
                items.push({
                    elementId: Number(item.elementId),
                    fileId: Number(item.fileId || item.elementId),
                    imageId: Number(item.imageId),
                    fileName: item.fileName ? item.fileName : qsTr("(unnamed)"),
                    isHdr: item.isHdr === true
                })
            }
            return items
        }

        function pruneDeletedElements(elementIds) {
            if (!elementIds || elementIds.length === 0) {
                return
            }

            const deleted = {}
            for (let i = 0; i < elementIds.length; ++i) {
                deleted[keyForElement(elementIds[i])] = true
            }

            const nextSelected = {}
            const selectedRows = Object.values(selectedImagesById)
            for (let i = 0; i < selectedRows.length; ++i) {
                const row = selectedRows[i]
                const key = keyForElement(row.elementId)
                if (!deleted[key]) {
                    nextSelected[key] = row
                }
            }
            selectedImagesById = nextSelected
        }
    }

    FileDialog {
        id: importDialog
        title: qsTr("Select Images")
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            qsTr("RAW Images (*.raw *.dng *.nef *.cr2 *.cr3 *.arw *.rw2 *.raf *.3fr *.fff)"),
            qsTr("All Files (*)")
        ]
        onAccepted: {
            const files = []
            for (let i = 0; i < selectedFiles.length; ++i) {
                files.push(selectedFiles[i].toString())
            }
            appModules.importExport.StartImport(files)
        }
    }

    AlbumExportDialog {
        id: exportDialog
        blurSource: mainContent
        selectedCount: root.selectedCount
        exportQueueCount: root.exportQueueCount
        exportPreviewRows: root.exportPreviewRows
        hdrExportAvailable: exportQueueState.hasHdrItems()
        onAddSelectedToQueueRequested: {
            exportQueueState.addTargets(selectionState.currentSelectedItems())
            selectionState.clearSelectedImages()
        }
        onClearQueueRequested: exportQueueState.clearQueue()
        onEnsurePreviewRequested: exportQueueState.refreshExportPreview()
        onStartExportRequested: function(outDir, sdrResizeEnabled, sdrMaxSide, ultraHdrMaxSide, sdrFormat, sdrQuality, sdrBitDepth, sdrPngLevel, sdrTiffComp, ultraHdrQuality, ultraHdrDitherEnabled) {
            appModules.importExport.StartExportWithSplitOptionsForTargets(
                outDir,
                sdrResizeEnabled,
                sdrMaxSide,
                ultraHdrMaxSide,
                sdrFormat,
                sdrQuality,
                sdrBitDepth,
                sdrPngLevel,
                sdrTiffComp,
                ultraHdrQuality,
                ultraHdrDitherEnabled,
                exportQueueState.exportQueueTargets())
        }
    }

    SettingDialog {
        id: settingsDialog
        z: 30
        blurSource: mainContent
        cornerRadius: root.windowCornerRadius
        languageOptions: root.languageOptions
        primaryAccent: root.colButtonPrimary
        secondaryAccent: root.colAccentSecondary
        textColor: root.colText
        mutedTextColor: root.colTextMuted
        panelColor: root.colBgPanel
        canvasColor: root.colBgCanvas
        overlayColor: root.colOverlay
        hoverColor: root.colHover
        dividerColor: root.colDivider
        dangerColor: root.colDanger
        panelBorderColor: root.withAlpha(root.colText, 0.08)
        headlineFontFamily: root.headlineFontFamily
        onMessageRequested: function(message) {
            root.showSnackbar(message)
        }
        onSemanticGenerationBackgroundRequested: {
            semanticGenerationDialog.runInBackground()
        }
    }

    AdjustmentTransferDialog {
        id: adjustmentTransferDialog
        blurSource: mainContent
        cornerRadius: root.windowCornerRadius
        onCopyAccepted: function(selectedKeys) {
            const result = appModules.adjustmentTransfer.Copy(
                Number(root.pendingAdjustmentSource.elementId),
                selectedKeys)
            if (result && result.message) {
                root.showSnackbar(result.message)
            }
        }
        onPasteAccepted: function(strategy) {
            const result = appModules.adjustmentTransfer.Paste(
                root.pendingAdjustmentPasteTargets,
                strategy)
            if (result && result.message) {
                root.showSnackbar(result.message)
            }
            root.pendingAdjustmentPasteTargets = []
        }
        onPasteDiscarded: {
            appModules.adjustmentTransfer.Discard()
            root.pendingAdjustmentPasteTargets = []
        }
    }

    ImageContextMenu {
        id: imageContextMenu
        ratingEnabled: Number(root.pendingRatingTarget.imageId) > 0
        currentRating: Math.max(0, Math.min(5, Number(root.pendingRatingTarget.rating || 0)))
        actions: [
            {
                id: "copy-adjustments",
                label: qsTr("Copy Adjustments"),
                enabled: Number(root.pendingAdjustmentSource.elementId) > 0
            },
            {
                id: "paste-adjustments",
                label: qsTr("Paste Adjustments"),
                enabled: appModules.adjustmentTransfer.packageAvailable
                         && root.pendingAdjustmentPasteTargets.length > 0
            },
            {
                id: "delete",
                label: Number(appModules.folders.currentFolderId) === 0 ? qsTr("Delete") : qsTr("Remove from Album"),
                // Phase 2: blocked while the selected images are in an active
                // analysis set (DeleteImages lock). The reason is exposed on the
                // policy controller as pendingDeleteReason.
                enabled: root.pendingDeleteTargets.length > 0
                          && appModules.interactionPolicy.canDeletePendingTargets
            }
        ].concat(root.albumTargetActions())
        onRatingRequested: function(rating) {
            imageContextMenu.close()
            root.requestSetImageRating(rating)
        }
        onActionRequested: function(actionId) {
            imageContextMenu.close()
            if (actionId === "copy-adjustments") {
                requestCopyAdjustments()
                return
            }
            if (actionId === "paste-adjustments") {
                requestPasteAdjustments()
                return
            }
            if (actionId === "delete") {
                requestDeleteConfirmation()
                return
            }
            if (String(actionId).indexOf("add-to-album:") === 0) {
                root.runAddTargetsToAlbum(Number(String(actionId).split(":")[1]))
            }
        }
    }

    Connections {
        target: languageManager
        function onLanguageChanged() {
            root.refreshFocusedImageInspection()
        }
    }

    Connections {
        target: appModules.imageAnalysis
        ignoreUnknownSignals: true
        function onStateChanged() {
            if (!appModules.imageAnalysis.running
                    && root.analysisResultTouchesFocusedImage()) {
                root.refreshFocusedImageInspection()
            }
        }
    }

    NikonHeRecoveryDialog {
        id: nikonHeRecoveryDialog
        parent: Overlay.overlay
        backgroundSource: mainContent
        recoveryActive: appModules.nikonHeRecovery.nikonHeRecoveryActive
        recoveryBusy: appModules.nikonHeRecovery.nikonHeRecoveryBusy
        recoveryPhase: appModules.nikonHeRecovery.nikonHeRecoveryPhase
        recoveryStatus: appModules.nikonHeRecovery.nikonHeRecoveryStatus
        unsupportedFiles: appModules.nikonHeRecovery.nikonHeUnsupportedFiles
        converterPath: appModules.nikonHeRecovery.nikonHeConverterPath
        converterPathFromDefault: appModules.nikonHeRecovery.nikonHeConverterPathFromDefault
        showImportProgress: appModules.importExport.importRunning && appModules.nikonHeRecovery.nikonHeRecoveryActive
        importCompleted: appModules.importExport.importCompleted
        importTotal: appModules.importExport.importTotal
        importFailed: appModules.importExport.importFailed
        onBrowseRequested: appModules.nikonHeRecovery.BrowseNikonHeConverter()
        onConvertRequested: appModules.nikonHeRecovery.StartNikonHeConversion()
        onExitRequested: appModules.nikonHeRecovery.ExitNikonHeRecovery()
    }

    AdvancedContentAnalysisDialog {
        id: advancedContentAnalysisDialog
        parent: Overlay.overlay
        blurSource: mainContent
        backend: appModules
        analysisController: appModules.imageAnalysis
        profileController: appModules.aiProviderProfiles
        interactionPolicy: appModules.interactionPolicy
        backendInteractive: root.backendInteractive
        onMessageRequested: function(message) {
            root.showSnackbar(message)
        }
    }

    SemanticGenerationDialog {
        id: semanticGenerationDialog
        parent: Overlay.overlay
        backgroundSource: mainContent
        promptVisible: root.semanticGeneration.promptVisible
        generationRunning: root.semanticGeneration.running
        pendingCount: root.semanticGeneration.pendingCount
        total: root.semanticGeneration.total
        embedded: root.semanticGeneration.embedded
        skipped: root.semanticGeneration.skipped
        failed: root.semanticGeneration.failed
        canceled: root.semanticGeneration.canceled
        statusText: root.semanticGeneration.statusText
        onStartRequested: function(rememberChoice) {
            if (rememberChoice) {
                root.semanticGeneration.SetImportPreference("always")
            }
            root.semanticGeneration.StartPendingGeneration(false)
        }
        onSkipRequested: function(rememberChoice) {
            root.semanticGeneration.SkipPendingGeneration(rememberChoice)
        }
        onCancelRequested: root.semanticGeneration.CancelGeneration()
    }

    ActivateModelDialog {
        id: activateModelDialog
        parent: Overlay.overlay
        backgroundSource: mainContent
        promptVisible: root.semanticGeneration.activatePromptVisible
        onOpenSettingsRequested: {
            root.semanticGeneration.DismissActivatePrompt()
            root.openSettingsDialog(3) // 3 == "Local Content Recognition" (model install/activate)
        }
        onDismissed: root.semanticGeneration.DismissActivatePrompt()
    }

    Popup {
        id: deleteConfirmDialog
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        width: Math.min(root.width - 36, 520)
        height: deleteConfirmContent.implicitHeight + 36
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)

        Overlay.modal: Item {
            anchors.fill: parent

            MultiEffect {
                anchors.fill: parent
                source: mainContent
                blurEnabled: true
                blur: 0.6
                blurMax: 64
                saturation: -0.2
            }

            Rectangle {
                anchors.fill: parent
                color: root.colOverlay
            }

            MouseArea { anchors.fill: parent; hoverEnabled: true }
        }

        background: Rectangle {
            radius: 14
            color: root.colBgPanel
            border.width: 0
        }

        onClosed: {
            root.deleteConfirmText = ""
        }

        contentItem: ColumnLayout {
            id: deleteConfirmContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 18
            spacing: 12

            Label {
                text: qsTr("Confirm Deletion")
                font.family: root.headlineFontFamily
                font.pixelSize: 24
                font.weight: 700
                color: root.colText
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.deleteConfirmText.length > 0
                      ? qsTr("%1\nOriginal source files on disk will be kept.")
                            .arg(root.deleteConfirmText)
                      : ""
                color: root.colText
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Item { Layout.fillWidth: true }

                Button {
                    id: deleteCancelButton
                    text: qsTr("Cancel")
                    Material.background: root.colButtonSecondary
                    Material.foreground: root.colText
                    onClicked: {
                        root.pendingDeleteTargets = []
                        deleteConfirmDialog.close()
                    }
                }

                Button {
                    id: deleteConfirmButton
                    text: qsTr("Delete")
                    // Phase 2: gated by the same policy as the context-menu delete
                    // action — stays disabled while the targets are being analyzed.
                    enabled: appModules.interactionPolicy.canDeletePendingTargets
                    Material.background: root.colDanger
                    Material.foreground: root.colText
                    onClicked: {
                        deleteConfirmDialog.close()
                        root.runDeleteTargets()
                    }
                }
            }
        }
    }

    Connections {
        target: appModules.project
        ignoreUnknownSignals: true
        function onProjectChanged() {
            root.projectLaunchPending = false
            root.welcomeDismissedForLaunch = false
            root.updateWelcomeDialogVisibility()
            selectionState.clearSelectedImages()
            exportQueueState.clearQueue()
            root.pendingDeleteTargets = []
            root.pendingRatingTarget = ({})
            root.setFocusedImage(null)
            deleteConfirmDialog.close()
            root.showSnackbar(appModules.project.serviceMessage)

            // Auto-maximize when a project is successfully opened.
            if (appModules.project.serviceReady && !root.windowMaximized && !maximizeTransition.running) {
                maximizeTransition.targetMaximize = true
                maximizeTransition.start()
            }
        }
        function onServiceStateChanged() {
            root.updateWelcomeDialogVisibility()
        }
        function onProjectLoadStateChanged() {
            root.projectLaunchPending = false
            if (!appModules.project.projectLoading) {
                root.welcomeDismissedForLaunch = false
            }
            root.updateWelcomeDialogVisibility()
        }
    }

    Connections {
        target: appModules.folders
        ignoreUnknownSignals: true
        function onFolderSelectionChanged() {
            selectionState.clearSelectedImages()
            root.pendingDeleteTargets = []
            root.pendingRatingTarget = ({})
            root.setFocusedImage(null)
            deleteConfirmDialog.close()
        }
    }

    Connections {
        target: appModules.library
        ignoreUnknownSignals: true
        function onThumbnailsChanged() {
            if (exportDialog.visible) {
                exportQueueState.refreshExportPreview()
            }
        }
    }

    Connections {
        target: appModules.importExport
        ignoreUnknownSignals: true
        function onImportStateChanged() {
            if (appModules.importExport.importRunning) {
                root.importSessionObserved = true
                return
            }
            if (!root.importSessionObserved) {
                return
            }
            root.importSessionObserved = false
            root.showSnackbar(qsTr("Imported %1 image(s).").arg(appModules.importExport.importCompleted))
        }
        function onExportStateChanged() {
            if (appModules.importExport.exportInFlight) {
                root.exportSessionObserved = true
                if (appModules.importExport.exportCompleted > root.lastObservedExportCompleted) {
                    exportQueueState.pruneCompleted(appModules.importExport.exportItemStatuses)
                    root.lastObservedExportCompleted = appModules.importExport.exportCompleted
                }
                return
            }
            exportQueueState.pruneCompleted(appModules.importExport.exportItemStatuses)
            root.lastObservedExportCompleted = 0
            if (!root.exportSessionObserved) {
                return
            }
            root.exportSessionObserved = false
            root.showSnackbar(qsTr("Exported %1 image(s).").arg(appModules.importExport.exportSucceeded))
        }
    }

    Popup {
        id: notificationSnackbar
        parent: Overlay.overlay
        modal: false
        focus: false
        closePolicy: Popup.NoAutoClose
        padding: 12
        width: Math.min(root.width - 24, 760)
        x: Math.round((root.width - width) / 2)
        y: root.height - height - 16

        background: Rectangle {
            radius: 10
            color: root.colGlassPanel
            border.width: 1
            border.color: root.colGlassStroke
        }

        contentItem: Label {
            text: root.snackbarText
            color: root.colText
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }

        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 120 }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 120 }
        }
    }

    Timer {
        id: snackbarTimer
        interval: 2600
        repeat: false
        onTriggered: notificationSnackbar.close()
    }

    Item {
        id: mainContent
        anchors.fill: parent
        anchors.margins: root.maximizedInset
        clip: true

        Rectangle {
            anchors.fill: parent
            color: root.colBgCanvas
            radius: root.windowCornerRadius
        }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        Rectangle {
            id: topToolbar
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            radius: root.panelRadius
            color: root.colGlassPanel
            border.width: 1
            border.color: root.colGlassStroke
            z: 1

            // Drag the window from any empty area of the toolbar; double-click toggles maximize.
            TapHandler {
                acceptedButtons: Qt.LeftButton
                gesturePolicy: TapHandler.DragThreshold
                onDoubleTapped: root.toggleMaximizeAnimated()
            }
            DragHandler {
                target: null
                grabPermissions: PointerHandler.CanTakeOverFromAnything
                onActiveChanged: if (active) root.startSystemMove()
            }

            RowLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 20
                anchors.rightMargin: 0
                spacing: 10
                Row {
                    spacing: 0
                    Label { text: qsTr("Alcedo"); font.family: root.headlineFontFamily; font.pixelSize: 19; font.weight: 700; color: root.colAccentPrimary }
                    Label { text: " "; font.family: root.headlineFontFamily; font.pixelSize: 19; font.weight: 700 }
                    Label { text: qsTr("Studio"); font.family: root.headlineFontFamily; font.pixelSize: 19; font.weight: 700; color: root.colText }
                }
                Item { Layout.preferredWidth: 12 }

                // ── File menu ──
                Button {
                    id: fileMenuButton
                    text: qsTr("File")
                    flat: true
                    Material.foreground: root.colText
                    onClicked: fileMenu.open()

                    Menu {
                        id: fileMenu
                        x: 0
                        y: fileMenuButton.height + 4

                        MenuItem {
                            text: qsTr("Load Project")
                            enabled: !root.projectLaunchBusy && !appModules.project.acceleratorPreparing
                            onTriggered: root.beginProjectLaunch(function() {
                                return appModules.project.PromptAndLoadProject()
                            })
                        }
                        MenuItem {
                            text: qsTr("Create Project")
                            enabled: !root.projectLaunchBusy && !appModules.project.acceleratorPreparing
                            onTriggered: root.beginProjectLaunch(function() {
                                return appModules.project.PromptAndCreateProject()
                            })
                        }
                        MenuSeparator {
                        }
                        MenuItem {
                            text: qsTr("Save Project")
                            enabled: root.backendInteractive
                            onTriggered: root.requestSaveProject()
                        }
                    }
                }

                Button {
                    id: settingsPopoutButton
                    text: qsTr("Settings")
                    flat: true
                    Material.foreground: root.colText
                    onClicked: root.openSettingsDialog()
                }

                Item { Layout.fillWidth: true }
                Button {
                    id: inspectorToggleButton
                    checkable: false
                    flat: true
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: 42
                    display: AbstractButton.IconOnly
                    property real iconRotationTarget: inspectorVisible ? 180 : 0
                    icon.source: "qrc:/panel_icons/inspector-expand.svg"
                    icon.width: 24
                    icon.height: 24
                    icon.color: inspectorVisible
                                ? root.colAccentPrimary
                                : (inspectorToggleButton.hovered ? root.colText : root.colTextMuted)
                    Material.foreground: icon.color
                    ToolTip.visible: hovered
                    ToolTip.text: inspectorVisible ? qsTr("Collapse Inspector") : qsTr("Expand Inspector")
                    background: Rectangle {
                        radius: root.controlRadius
                        color: "transparent"
                        border.width: 0
                    }
                    onContentItemChanged: {
                        inspectorIconRotate.target = contentItem
                        if (contentItem) {
                            contentItem.transformOrigin = Item.Center
                            contentItem.rotation = iconRotationTarget
                        }
                    }
                    onIconRotationTargetChanged: {
                        if (contentItem) {
                            inspectorIconRotate.stop()
                            inspectorIconRotate.to = iconRotationTarget
                            inspectorIconRotate.start()
                        }
                    }
                    Component.onCompleted: {
                        if (contentItem) {
                            contentItem.transformOrigin = Item.Center
                            contentItem.rotation = iconRotationTarget
                        }
                    }
                    NumberAnimation {
                        id: inspectorIconRotate
                        property: "rotation"
                        duration: 170
                        easing.type: Easing.OutCubic
                    }
                    onClicked: inspectorVisible = !inspectorVisible
                }

                // ── Frameless window caption buttons ──
                Item { Layout.preferredWidth: 8 }

                Row {
                    id: windowCaptionButtons
                    Layout.preferredHeight: 56
                    spacing: 0

                    component CaptionButton: Rectangle {
                        id: capBtn
                        property string iconName: "minimize"
                        property color hoverColor: root.colHover
                        property color iconColor: root.colText
                        signal activated()
                        width: 46
                        height: 56
                        color: capMA.containsMouse
                               ? hoverColor
                               : "transparent"

                        Canvas {
                            id: captionIcon
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            antialiasing: true

                            Connections {
                                target: capBtn
                                function onIconNameChanged() { captionIcon.requestPaint() }
                                function onIconColorChanged() { captionIcon.requestPaint() }
                            }

                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                ctx.strokeStyle = capBtn.iconColor
                                ctx.lineWidth = 1.35
                                ctx.lineCap = "square"
                                ctx.lineJoin = "miter"

                                if (capBtn.iconName === "minimize") {
                                    ctx.beginPath()
                                    ctx.moveTo(4, 8)
                                    ctx.lineTo(12, 8)
                                    ctx.stroke()
                                } else if (capBtn.iconName === "maximize") {
                                    ctx.strokeRect(4.5, 4.5, 7, 7)
                                } else if (capBtn.iconName === "restore") {
                                    ctx.strokeRect(6.5, 4.5, 6, 6)
                                    ctx.strokeRect(3.5, 7.5, 6, 6)
                                } else if (capBtn.iconName === "close") {
                                    ctx.lineCap = "round"
                                    ctx.beginPath()
                                    ctx.moveTo(4.5, 4.5)
                                    ctx.lineTo(11.5, 11.5)
                                    ctx.moveTo(11.5, 4.5)
                                    ctx.lineTo(4.5, 11.5)
                                    ctx.stroke()
                                }
                            }
                        }

                        MouseArea {
                            id: capMA
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.ArrowCursor
                            onClicked: capBtn.activated()
                        }
                    }

                    CaptionButton {
                        iconName: "minimize"
                        onActivated: minimizeAnimation.start()
                    }
                    CaptionButton {
                        iconName: root.visibility === Window.Maximized ? "restore" : "maximize"
                        onActivated: root.toggleMaximizeAnimated()
                    }
                    CaptionButton {
                        iconName: "close"
                        hoverColor: root.colDanger
                        onActivated: root.close()
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            CollectionsPanel {
                Layout.preferredWidth: root.leftPaneWidth
                Layout.minimumWidth: root.leftPaneWidth
                Layout.maximumWidth: root.leftPaneWidth
                Layout.fillHeight: true
                backend: appModules
                theme: root
                backendInteractive: root.backendInteractive
                selectedCount: root.selectedCount
                onImportRequested: importDialog.open()
                onSearchRequested: globalSearchDialog.openFromCollection()
                onAdvancedAnalysisRequested: root.openAdvancedAnalysisDialog()
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: root.centerPaneMinWidth
                spacing: 10

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: root.panelRadius
                    color: root.colGlassPanel
                    border.width: 1
                    border.color: root.colGlassStroke
                    clip: true

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    anchors.topMargin: 10
                    anchors.bottomMargin: 0
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        Label { text: qsTr("Browser"); color: root.colTextMuted; font.pixelSize: 13; font.weight: 600 }
                        Item { Layout.fillWidth: true }

                        // ── Zoom slider ──
                        Item {
                            Layout.preferredWidth: 180
                            Layout.preferredHeight: 36

                            TapHandler {
                                acceptedButtons: Qt.LeftButton
                                onDoubleTapped: root.gridZoomLevel = root.defaultGridZoomLevel
                            }

                            RowLayout {
                                anchors.fill: parent
                                spacing: 8

                                Label {
                                    text: "-"
                                    color: root.colTextMuted
                                    font.family: root.dataFontFamily
                                    font.pixelSize: 18
                                    font.weight: 600
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.gridZoomLevel = Math.min(7, root.gridZoomLevel + 1)
                                    }
                                }

                                Slider {
                                    id: zoomSlider
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    from: 0
                                    to: 7
                                    stepSize: 1
                                    value: root.gridZoomLevel
                                    onValueChanged: root.gridZoomLevel = Math.round(value)
                                    background: Rectangle {
                                        x: zoomSlider.leftPadding
                                        y: zoomSlider.topPadding + zoomSlider.availableHeight / 2 - height / 2
                                        implicitWidth: zoomSlider.availableWidth
                                        implicitHeight: 4
                                        width: zoomSlider.availableWidth
                                        height: implicitHeight
                                        radius: 2
                                        color: Qt.rgba(root.colBgBase.r, root.colBgBase.g, root.colBgBase.b, 0.98)
                                        Rectangle {
                                            width: zoomSlider.visualPosition * parent.width
                                            height: parent.height
                                            color: root.colAccentPrimary
                                            radius: 2
                                        }
                                    }
                                    handle: Rectangle {
                                        x: zoomSlider.leftPadding + zoomSlider.visualPosition * (zoomSlider.availableWidth - width)
                                        y: zoomSlider.topPadding + zoomSlider.availableHeight / 2 - height / 2
                                        implicitWidth: 14
                                        implicitHeight: 14
                                        radius: 7
                                        color: root.colAccentPrimary
                                        border.width: 1
                                        border.color: root.colAccentSecondary
                                    }
                                }

                                Label {
                                    text: "+"
                                    color: root.colText
                                    font.family: root.dataFontFamily
                                    font.pixelSize: 18
                                    font.weight: 600
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.gridZoomLevel = Math.max(0, root.gridZoomLevel - 1)
                                    }
                                }
                            }
                        }

                        Item { Layout.preferredWidth: 10 }
                        Item {
                            id: viewModeSwitch
                            Layout.preferredWidth: 132
                            Layout.preferredHeight: 36

                            Rectangle {
                                id: viewModeTrack
                                anchors.fill: parent
                                radius: height / 2
                                color: Qt.rgba(root.colBgBase.r, root.colBgBase.g, root.colBgBase.b, 0.98)
                                border.width: 1
                                border.color: root.colDivider
                            }

                            Rectangle {
                                id: viewModeThumb
                                width: parent.width / 2 - 4
                                height: parent.height - 4
                                y: 2
                                x: root.gridMode ? 2 : parent.width - width - 2
                                radius: height / 2
                                color: root.colAccentPrimary
                                border.width: 1
                                border.color: Qt.rgba(
                                    root.colAccentSecondary.r,
                                    root.colAccentSecondary.g,
                                    root.colAccentSecondary.b,
                                    0.52)
                                Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                            }

                            Row {
                                anchors.fill: parent
                                spacing: 0

                                Item {
                                    width: parent.width / 2
                                    height: parent.height

                                    Image {
                                        id: gridModeIconSource
                                        anchors.centerIn: parent
                                        width: 20
                                        height: 20
                                        source: "qrc:/panel_icons/layout-grid.svg"
                                        visible: false
                                        asynchronous: true
                                    }

                                    MultiEffect {
                                        anchors.fill: gridModeIconSource
                                        source: gridModeIconSource
                                        colorization: 1.0
                                        colorizationColor: "white"
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.gridMode = true
                                    }
                                }

                                Item {
                                    width: parent.width / 2
                                    height: parent.height

                                    Image {
                                        id: listModeIconSource
                                        anchors.centerIn: parent
                                        width: 20
                                        height: 20
                                        source: "qrc:/panel_icons/list.svg"
                                        visible: false
                                        asynchronous: true
                                    }

                                    MultiEffect {
                                        anchors.fill: listModeIconSource
                                        source: listModeIconSource
                                        colorization: 1.0
                                        colorizationColor: "white"
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.gridMode = false
                                    }
                                }
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        Loader {
                            anchors.fill: parent
                            active: appModules.library.shownCount > 0
                            sourceComponent: gridMode ? gridComp : listComp
                        }

                        Column {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            visible: appModules.library.shownCount === 0
                            spacing: 8
                            Label {
                                text: appModules.project.serviceReady ? qsTr("No Photos Yet") : qsTr("Open or Create a Project")
                                font.family: root.headlineFontFamily
                                color: root.colText
                                font.pixelSize: 22
                                font.weight: 700
                            }
                            Label {
                                text: appModules.project.serviceReady
                                      ? qsTr("Import your images for RAW adjustments.")
                                      : qsTr("Use File > Load Project or File > Create Project to choose .alcd files.")
                                color: root.colTextMuted
                                font.pixelSize: 12
                            }
                            Button {
                                id: emptyStateLoadButton
                                visible: !appModules.project.serviceReady
                                text: qsTr("Load Project")
                                Material.background: root.colButtonPrimary
                                Material.foreground: root.colText
                                onClicked: root.beginProjectLaunch(function() {
                                    return appModules.project.PromptAndLoadProject()
                                })
                            }
                        }
                    }
                }
                } // close album card Rectangle

            } // close center block wrapper

            // ── inspector panel + overlay resize handle ──
            Item {
                id: inspectorContainer
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: root.inspectorAdaptiveMaxWidth
                Layout.preferredWidth: inspectorVisible
                                       ? Math.min(root.inspectorWidth, root.inspectorAdaptiveMaxWidth)
                                       : 0
                Behavior on Layout.preferredWidth { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: root.panelRadius
                        color: root.colBgPanel
                        border.width: 0
                        clip: true

                        InspectorPanel {
                            anchors.fill: parent
                            anchors.margins: 10
                            focusedImage: root.focusedImageInspection
                            interactionPolicy: appModules.interactionPolicy
                            onRatingRequested: function(rating) {
                                root.requestSetFocusedImageRating(rating)
                            }
                            onDescriptionSaveRequested: function(caption) {
                                root.requestSaveFocusedDescription(caption)
                            }
                            onRatingReasonSaveRequested: function(reasons) {
                                root.requestSaveFocusedRatingReason(reasons)
                            }
                            onContextMenuRequested: function(item, sceneX, sceneY) {
                                root.openImageContextMenu(item, sceneX, sceneY)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 52
                        spacing: 10

                        Button {
                            id: addSelectedBtn
                            Layout.fillWidth: true
                            Layout.preferredHeight: 52
                            text: qsTr("Add to Queue") + " (" + root.selectedCount + ")"
                            enabled: root.backendInteractive && root.selectedCount > 0
                            icon.source: "qrc:/panel_icons/queue-add.svg"
                            icon.width: 16
                            icon.height: 16
                            icon.color: root.colText
                            display: AbstractButton.TextBesideIcon
                            background: Rectangle {
                                radius: root.controlRadius
                                color: root.secondaryButtonFill(
                                    addSelectedBtn.enabled,
                                    addSelectedBtn.hovered,
                                    addSelectedBtn.down)
                                border.width: 1
                                border.color: root.colButtonSecondaryBorder
                            }
                            Material.foreground: root.colText
                            scale: addSelectedBtn.hovered && enabled ? 1.03 : 1.0
                            Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                            onClicked: {
                                exportQueueState.addTargets(selectionState.currentSelectedItems())
                                selectionState.clearSelectedImages()
                            }
                        }

                        Button {
                            id: exportQueueBtn
                            Layout.fillWidth: true
                            Layout.preferredHeight: 52
                            text: qsTr("Export") + " (" + root.exportQueueCount + ")"
                            enabled: root.backendInteractive && (appModules.library.shownCount > 0 || root.exportQueueCount > 0)
                            icon.source: "qrc:/panel_icons/export.svg"
                            icon.width: 16
                            icon.height: 16
                            icon.color: root.colText
                            display: AbstractButton.TextBesideIcon
                            background: Canvas {
                                opacity: exportQueueBtn.enabled ? 1.0 : 0.5
                                property color gradStart: root.colAccentPrimary
                                property color gradEnd: root.colAccentSecondary
                                onGradStartChanged: requestPaint()
                                onGradEndChanged: requestPaint()
                                onWidthChanged: requestPaint()
                                onHeightChanged: requestPaint()
                                onPaint: {
                                    var ctx = getContext("2d")
                                    ctx.clearRect(0, 0, width, height)
                                    var r = 8
                                    ctx.beginPath()
                                    ctx.moveTo(r, 0)
                                    ctx.lineTo(width - r, 0)
                                    ctx.quadraticCurveTo(width, 0, width, r)
                                    ctx.lineTo(width, height - r)
                                    ctx.quadraticCurveTo(width, height, width - r, height)
                                    ctx.lineTo(r, height)
                                    ctx.quadraticCurveTo(0, height, 0, height - r)
                                    ctx.lineTo(0, r)
                                    ctx.quadraticCurveTo(0, 0, r, 0)
                                    ctx.closePath()
                                    var grad = ctx.createLinearGradient(0, height, width, 0)
                                    grad.addColorStop(0.0, Qt.rgba(gradStart.r, gradStart.g, gradStart.b, 1.0))
                                    grad.addColorStop(1.0, Qt.rgba(gradEnd.r, gradEnd.g, gradEnd.b, 1.0))
                                    ctx.fillStyle = grad
                                    ctx.fill()
                                }
                            }
                            Material.foreground: root.colText
                            scale: exportQueueBtn.hovered && enabled ? 1.03 : 1.0
                            Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                            onClicked: {
                                exportQueueState.refreshExportPreview()
                                exportDialog.open()
                            }
                        }
                    }
                }

                Rectangle {
                    id: inspectorResizeHandle
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: inspectorVisible && root.inspectorAdaptiveMaxWidth > 0 ? 5 : 0
                    x: -Math.round(width / 2)
                    color: dragArea.containsMouse || dragArea.drag.active ? root.colAccentPrimary : "transparent"
                    visible: width > 0
                    z: 10
                    Behavior on width { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                    Behavior on color { ColorAnimation { duration: 120 } }

                    MouseArea {
                        id: dragArea
                        anchors.fill: parent
                        anchors.margins: -3          // widen the hit area
                        hoverEnabled: true
                        cursorShape: Qt.SplitHCursor
                        property real startX: 0
                        property real startWidth: 0
                        onPressed: function(mouse) {
                            startX = mapToGlobal(mouse.x, 0).x
                            startWidth = root.inspectorWidth
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed) return
                            var globalX = mapToGlobal(mouse.x, 0).x
                            var delta = startX - globalX   // dragging left ⇒ wider
                            var cappedMax = Math.min(root.inspectorMaxWidth, root.inspectorAdaptiveMaxWidth)
                            var target = startWidth + delta
                            if (cappedMax >= root.inspectorMinWidth) {
                                root.inspectorWidth = Math.max(root.inspectorMinWidth, Math.min(cappedMax, target))
                            } else {
                                root.inspectorWidth = Math.max(0, Math.min(cappedMax, target))
                            }
                        }
                    }
                }
            }
        }

        BackgroundTaskBar {
            Layout.fillWidth: true
            onTaskDetailsRequested: function(task) {
                if (task && task.kind === "imageAnalysis") {
                    advancedContentAnalysisDialog.openTaskDetails(task)
                }
            }
        }

        GlobalSearchDialog {
            id: globalSearchDialog
            backend: appModules
            theme: root
            blurSource: mainContent
            cornerRadius: root.windowCornerRadius
        }

    }

    }

    // ── Minimize / restore content animations ──
    SequentialAnimation {
        id: minimizeAnimation
        ParallelAnimation {
            NumberAnimation { target: mainContent; property: "scale"; from: 1.0; to: 0.96; duration: 130; easing.type: Easing.InCubic }
            NumberAnimation { target: mainContent; property: "opacity"; to: 0.0; duration: 110; easing.type: Easing.InCubic }
        }
        ScriptAction { script: root.showMinimized() }
        ScriptAction { script: { mainContent.scale = 1.0 } }
    }

    // Fade-through: hide content fully → switch visibility (OS animates window) → wait for the
    // OS resize to finish → fade back in. Hiding during the resize avoids the visible Layout
    // re-flow ("twitch") that happens while the window dimensions are interpolating.
    function toggleMaximizeAnimated() {
        if (maximizeTransition.running) {
            return
        }
        maximizeTransition.targetMaximize = !root.windowMaximized
        maximizeTransition.start()
    }

    SequentialAnimation {
        id: maximizeTransition
        property bool targetMaximize: false

        NumberAnimation { target: mainContent; property: "opacity"; to: 0.0; duration: 110; easing.type: Easing.OutCubic }
        ScriptAction {
            script: {
                if (maximizeTransition.targetMaximize) {
                    root.showMaximized()
                } else {
                    root.showNormal()
                }
            }
        }
        // Wait long enough for the Win11 maximize/restore animation to finish.
        PauseAnimation { duration: 240 }
        NumberAnimation { target: mainContent; property: "opacity"; to: 1.0; duration: 180; easing.type: Easing.OutCubic }
    }

    NumberAnimation {
        id: restoreAnimation
        target: mainContent
        property: "opacity"
        from: 0.0
        to: 1.0
        duration: 200
        easing.type: Easing.OutCubic
    }

    Connections {
        target: root
        function onVisibilityChanged() {
            if (root.visibility !== Window.Minimized && root.visibility !== Window.Hidden) {
                if (mainContent.opacity < 1.0 && !restoreAnimation.running) {
                    restoreAnimation.start()
                }
            }
        }
    }

    // ── Frameless-window resize handles (native DWM resize via startSystemResize) ──
    Item {
        id: resizeHandles
        anchors.fill: parent
        visible: root.visibility === Window.Windowed
        z: 1000

        readonly property int edge: 4
        readonly property int corner: 10

        MouseArea {
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: resizeHandles.edge
            cursorShape: Qt.SizeVerCursor
            onPressed: root.startSystemResize(Qt.TopEdge)
        }
        MouseArea {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: resizeHandles.edge
            cursorShape: Qt.SizeVerCursor
            onPressed: root.startSystemResize(Qt.BottomEdge)
        }
        MouseArea {
            anchors { top: parent.top; bottom: parent.bottom; left: parent.left }
            width: resizeHandles.edge
            cursorShape: Qt.SizeHorCursor
            onPressed: root.startSystemResize(Qt.LeftEdge)
        }
        MouseArea {
            anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
            width: resizeHandles.edge
            cursorShape: Qt.SizeHorCursor
            onPressed: root.startSystemResize(Qt.RightEdge)
        }
        MouseArea {
            anchors { top: parent.top; left: parent.left }
            width: resizeHandles.corner; height: resizeHandles.corner
            cursorShape: Qt.SizeFDiagCursor
            onPressed: root.startSystemResize(Qt.TopEdge | Qt.LeftEdge)
        }
        MouseArea {
            anchors { top: parent.top; right: parent.right }
            width: resizeHandles.corner; height: resizeHandles.corner
            cursorShape: Qt.SizeBDiagCursor
            onPressed: root.startSystemResize(Qt.TopEdge | Qt.RightEdge)
        }
        MouseArea {
            anchors { bottom: parent.bottom; left: parent.left }
            width: resizeHandles.corner; height: resizeHandles.corner
            cursorShape: Qt.SizeBDiagCursor
            onPressed: root.startSystemResize(Qt.BottomEdge | Qt.LeftEdge)
        }
        MouseArea {
            anchors { bottom: parent.bottom; right: parent.right }
            width: resizeHandles.corner; height: resizeHandles.corner
            cursorShape: Qt.SizeFDiagCursor
            onPressed: root.startSystemResize(Qt.BottomEdge | Qt.RightEdge)
        }
    }

    WelcomeDialog {
        id: welcomeDialog
        z: 30
        blurSource: mainContent
        cornerRadius: root.windowCornerRadius
        recentProjects: appModules.project.recentProjects
        languageOptions: root.languageOptions
        acceleratorOptions: appModules.project.acceleratorOptions
        currentLanguageIndex: root.languageIndexForCode(languageManager.currentLanguageCode)
        currentAcceleratorBackend: appModules.project.acceleratorBackend
        acceleratorWarning: appModules.project.acceleratorWarning
        serviceMessage: appModules.project.serviceMessage
        headlineFontFamily: root.headlineFontFamily
        primaryAccent: root.colButtonPrimary
        secondaryAccent: root.colAccentSecondary
        textColor: root.colText
        mutedTextColor: root.colTextMuted
        panelColor: root.colBgPanel
        panelBorderColor: root.withAlpha(root.colText, 0.08)
        overlayColor: root.colOverlay
        baseColor: root.colBgCanvas
        onLoadRequested: {
            root.beginProjectLaunch(function() {
                return appModules.project.PromptAndLoadProject()
            })
        }
        onCreateRequested: function(projectName, storageLocation) {
            root.beginProjectLaunch(function() {
                return appModules.project.CreateProjectInFolderNamed(storageLocation, projectName)
            })
        }
        onExitRequested: Qt.quit()
        onLanguageRequested: function(languageCode) {
            languageManager.setLanguage(languageCode)
        }
        onAcceleratorRequested: function(backend) {
            if (!appModules.project.SetAcceleratorBackend(backend)) {
                root.showSnackbar(appModules.project.serviceMessage)
            }
        }
        onAcceleratorWarningAcknowledged: appModules.project.AcknowledgeAcceleratorWarning()
        onRecentProjectRequested: function(projectPath) {
            root.beginProjectLaunch(function() {
                return appModules.project.LoadProject(projectPath)
            })
        }
        onClosed: root.startPendingProjectLaunch()
    }

    Shortcut {
        sequence: StandardKey.SelectAll
        enabled: root.backendInteractive && !root.selectionShortcutBlocked()
        onActivated: root.selectAllCurrentAlbum()
    }

    // ── Accelerator preparation overlay ───────────────────────────────
    Item {
        id: acceleratorPreparationOverlay
        parent: Overlay.overlay
        anchors.fill: parent
        visible: appModules.project.acceleratorPreparing
        z: 70

        ShaderEffectSource {
            id: acceleratorPreparationSnapshot
            width: mainContent.width
            height: mainContent.height
            sourceItem: mainContent
            sourceRect: Qt.rect(0, 0, mainContent.width, mainContent.height)
            textureSize: Qt.size(Math.max(1, mainContent.width), Math.max(1, mainContent.height))
            live: true
            recursive: false
            hideSource: false
            visible: false
        }

        MultiEffect {
            x: mainContent.x
            y: mainContent.y
            width: mainContent.width
            height: mainContent.height
            source: acceleratorPreparationSnapshot
            blurEnabled: true
            blur: 0.6
            blurMax: 64
            saturation: -0.2
            autoPaddingEnabled: false
        }

        Rectangle {
            anchors.fill: parent
            color: root.colOverlay
        }

        MouseArea { anchors.fill: parent; hoverEnabled: true }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 36, 420)
            height: acceleratorPreparationContent.implicitHeight + 36
            radius: 14
            color: root.colBgDeep
            border.width: 0

            ColumnLayout {
                id: acceleratorPreparationContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 20
                spacing: 16

                Label {
                    text: qsTr("Preparing OpenCL")
                    font.family: root.headlineFontFamily
                    font.pixelSize: 21
                    font.weight: 700
                    color: root.colText
                    Layout.alignment: Qt.AlignHCenter
                }

                ImportProgressRing {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 160
                    Layout.preferredHeight: 160
                    ringWidth: 14
                    trackColor: root.colHover
                    fillColor: root.colAccentPrimary
                    indeterminate: true
                    running: appModules.project.acceleratorPreparing
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    text: appModules.project.acceleratorPreparationStatus.length > 0
                          ? appModules.project.acceleratorPreparationStatus
                          : qsTr("Compiling kernels...")
                    color: root.colTextMuted
                    font.pixelSize: 12
                }
            }
        }
    }

    // ── Project loading overlay ────────────────────────────────────────
    Item {
        id: projectLoadingOverlay
        anchors.fill: parent
        visible: root.projectLoadingOverlayVisible
        z: 45

        ShaderEffectSource {
            id: projectLoadingSnapshot
            width: mainContent.width
            height: mainContent.height
            sourceItem: mainContent
            sourceRect: Qt.rect(0, 0, mainContent.width, mainContent.height)
            textureSize: Qt.size(Math.max(1, mainContent.width), Math.max(1, mainContent.height))
            live: true
            recursive: false
            hideSource: false
            visible: false
        }

        MultiEffect {
            x: mainContent.x
            y: mainContent.y
            width: mainContent.width
            height: mainContent.height
            source: projectLoadingSnapshot
            blurEnabled: true
            blur: 0.6
            blurMax: 64
            saturation: -0.2
            autoPaddingEnabled: false
        }

        Rectangle {
            anchors.fill: parent
            color: root.colOverlay
        }

        MouseArea { anchors.fill: parent; hoverEnabled: true }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 36, 420)
            height: projectLoadingContent.implicitHeight + 36
            radius: 14
            color: root.colBgDeep
            border.width: 0

            ColumnLayout {
                id: projectLoadingContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 20
                spacing: 16

                Label {
                    text: qsTr("Loading Project")
                    font.family: root.headlineFontFamily
                    font.pixelSize: 21
                    font.weight: 700
                    color: root.colText
                    Layout.alignment: Qt.AlignHCenter
                }

                ImportProgressRing {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 160
                    Layout.preferredHeight: 160
                    ringWidth: 14
                    trackColor: root.colHover
                    fillColor: root.colAccentPrimary
                    indeterminate: true
                    running: root.projectLoadingOverlayVisible
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    text: appModules.project.projectLoadingMessage.length > 0
                          ? appModules.project.projectLoadingMessage
                          : qsTr("Preparing library...")
                    color: root.colTextMuted
                    font.pixelSize: 12
                }
            }
        }
    }

    // ── Import progress overlay ──────────────────────────────────────────
    Item {
        id: importProgressOverlay
        anchors.fill: parent
        visible: appModules.importExport.importRunning && !appModules.nikonHeRecovery.nikonHeRecoveryActive
        z: 50

        ShaderEffectSource {
            id: importOverlaySnapshot
            width: mainContent.width
            height: mainContent.height
            sourceItem: mainContent
            sourceRect: Qt.rect(0, 0, mainContent.width, mainContent.height)
            textureSize: Qt.size(Math.max(1, mainContent.width), Math.max(1, mainContent.height))
            live: true
            recursive: false
            hideSource: false
            visible: false
        }

        MultiEffect {
            x: mainContent.x
            y: mainContent.y
            width: mainContent.width
            height: mainContent.height
            source: importOverlaySnapshot
            blurEnabled: true
            blur: 0.6
            blurMax: 64
            saturation: -0.2
            autoPaddingEnabled: false
        }

        Rectangle {
            anchors.fill: parent
            color: root.colOverlay
        }

        MouseArea { anchors.fill: parent; hoverEnabled: true }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 36, 420)
            height: importDialogContent.implicitHeight + 36
            radius: 14
            color: root.colBgDeep
            border.width: 0

            ColumnLayout {
                id: importDialogContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 20
                spacing: 16

                Label {
                    text: qsTr("Importing Photos")
                    font.family: root.headlineFontFamily
                    font.pixelSize: 21
                    font.weight: 700
                    color: root.colText
                    Layout.alignment: Qt.AlignHCenter
                }

                ImportProgressRing {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 160
                    Layout.preferredHeight: 160
                    ringWidth: 14
                    trackColor: root.colHover
                    fillColor: root.colAccentPrimary
                    progress: appModules.importExport.importTotal > 0
                              ? appModules.importExport.importCompleted / appModules.importExport.importTotal
                              : 0
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("%1 / %2").arg(appModules.importExport.importCompleted).arg(appModules.importExport.importTotal)
                    font.family: root.dataFontFamily
                    font.pixelSize: 28
                    font.weight: 600
                    color: root.colText
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    text: appModules.importExport.importStatus.length > 0
                          ? appModules.importExport.importStatus
                          : qsTr("Preparing...")
                    color: root.colTextMuted
                    font.pixelSize: 12
                }

                Label {
                    Layout.fillWidth: true
                    visible: appModules.importExport.importFailed > 0
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("%1 file(s) failed").arg(appModules.importExport.importFailed)
                    color: root.colDanger
                    font.family: root.dataFontFamily
                    font.pixelSize: 12
                }

                Button {
                    id: importCancelButton
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Cancel")
                    Material.background: root.colDanger
                    Material.foreground: root.colText
                    onClicked: appModules.importExport.CancelImport()
                }
            }
        }
    }

    Component {
        id: gridComp
        ThumbnailGridView {
            zoomLevel: root.gridZoomLevel
            zoomAdjusting: zoomSlider.pressed
            onZoomLevelChanged: root.gridZoomLevel = zoomLevel
            selectedImagesById: root.selectedImagesById
            exportQueueById: root.exportQueueById
            onImageSelectionChanged: function(elementId, imageId, fileName, isHdr, selected) {
                selectionState.setImageSelected(elementId, imageId, fileName, isHdr, selected)
            }
            onReplaceSelection: function(items) {
                selectionState.replaceSelectedImages(items)
                if (items && items.length > 0) {
                    root.setFocusedImage(items[0])
                } else {
                    root.setFocusedImage(null)
                }
            }
            onImageFocused: function(item) {
                root.setFocusedImage(item)
            }
            onContextMenuRequested: function(item, sceneX, sceneY) {
                root.openImageContextMenu(item, sceneX, sceneY)
            }
        }
    }

    Component {
        id: listComp
        ThumbnailListView {
            selectedImagesById: root.selectedImagesById
            exportQueueById: root.exportQueueById
            onImageSelectionChanged: function(elementId, imageId, fileName, isHdr, selected) {
                selectionState.setImageSelected(elementId, imageId, fileName, isHdr, selected)
            }
            onReplaceSelection: function(items) {
                selectionState.replaceSelectedImages(items)
                if (items && items.length > 0) {
                    root.setFocusedImage(items[0])
                } else {
                    root.setFocusedImage(null)
                }
            }
            onImageFocused: function(item) {
                root.setFocusedImage(item)
            }
            onContextMenuRequested: function(item, sceneX, sceneY) {
                root.openImageContextMenu(item, sceneX, sceneY)
            }
        }
    }
}
