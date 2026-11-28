import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs

// All top-level dialog/menu instances and their wiring, extracted from
// Main.qml. `host` is Main (theme palette, showSnackbar, launch/settings
// helpers, semanticGeneration, pending* state aliases); the controllers and
// state objects are passed in as properties. Dialog ids use the *Obj suffix
// and are re-exported through friendly-name aliases so Main, ShellSignals, and
// shared workspace entry points can reach them via `appDialogs.<name>`.
Item {
    id: root
    property var host: null
    property bool automationMode: false
    property var imageActionsController: null
    property var selectionState: null
    property Item blurSource: null
    // A Save as Preset of the Copy dialog waits for the write of its preset file.
    property bool pendingDialogPresetSave: false

    property alias importDialog: importDialogObj
    property alias importFolderFlow: importFolderFlowObj
    property alias settingsDialog: settingsDialogObj
    property alias adjustmentTransferDialog: adjustmentTransferDialogObj
    property alias adjustmentTransferActions: adjustmentTransferActionsObj
    property alias applyPresetDialog: applyPresetDialogObj
    property alias imageContextMenu: imageContextMenuObj
    property alias nikonHeRecoveryDialog: nikonHeRecoveryDialogObj
    property alias semanticGenerationDialog: semanticGenerationDialogObj
    property alias deleteConfirmDialog: deleteConfirmDialogObj
    property alias editorCloseConfirmDialog: editorCloseConfirmDialogObj
    property alias welcomeDialog: welcomeDialogObj
    property alias welcomePreviewAdapter: welcomePreviewAdapterObj
    property alias globalSearchDialog: globalSearchDialogObj
    property alias backgroundTasksDialog: backgroundTasksDialogObj

    // Every extension of the import categories, read from C++
    // (ImportExportHandler::SupportedImportPatterns). The dialog filter ignores case on
    // Windows and macOS.
    readonly property string supportedImagePatterns: appModules.importExport.supportedImportPatterns

    FileDialog {
        id: importDialogObj
        title: qsTr("Select Images")
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            qsTr("Supported images (%1)").arg(root.supportedImagePatterns),
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

    // LUT browser Import (LUT library plan L6A): the library copies the files into its
    // user folder and reports conflicts or failures through lutLibrary.lastError.
    FileDialog {
        id: lutImportDialogObj
        title: qsTr("Import LUTs")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("CUBE LUTs (*.cube *.CUBE)"), qsTr("All Files (*)")]
        onAccepted: {
            const files = []
            for (let i = 0; i < selectedFiles.length; ++i) {
                files.push(selectedFiles[i].toString())
            }
            if (appModules.lutLibrary)
                appModules.lutLibrary.importFiles(files)
        }
    }

    // Import Folder (#1078): the system folder dialog, then the Import Folder dialog or the
    // remembered choice.
    ImportFolderFlow {
        id: importFolderFlowObj
        folders: appModules.folders
        importExport: appModules.importExport
        libraryBehavior: appModules.libraryBehavior
        theme: root.host
        blurSource: root.blurSource
    }

    // Library file drop (issue #757): files only import at once, as Import Images does; a drop
    // that holds a folder opens the Import Folder dialog with every dropped item as a scan
    // source and the drop target as the destination.
    function importDroppedUrls(urls, folderId) {
        if (!urls || urls.length === 0)
            return
        if (appModules.importExport.DropHoldsFolder(urls))
            importFolderFlowObj.beginDrop(urls, folderId)
        else
            appModules.importExport.StartImportToFolder(urls, folderId)
    }

    SettingDialog {
        id: settingsDialogObj
        objectName: "settingsDialog"
        z: 30
        windowHost: root.host
        blurSource: root.blurSource
        cornerRadius: host.windowCornerRadius
        languageOptions: host.languageOptions
        primaryAccent: host.colButtonPrimary
        secondaryAccent: host.colAccentSecondary
        textColor: host.colText
        mutedTextColor: host.colTextMuted
        panelColor: host.colBgPanel
        canvasColor: host.colBgCanvas
        overlayColor: host.colOverlay
        hoverColor: host.colHover
        dividerColor: host.colDivider
        dangerColor: host.colDanger
        panelBorderColor: host.withAlpha(host.colText, 0.08)
        headlineFontFamily: host.headlineFontFamily
        onMessageRequested: function(message) {
            host.showSnackbar(message)
        }
        onSemanticGenerationBackgroundRequested: {
            semanticGenerationDialogObj.runInBackground()
        }
    }

    AdjustmentTransferDialog {
        id: adjustmentTransferDialogObj
        blurSource: root.blurSource
        cornerRadius: host.windowCornerRadius
        dialogModel: appModules.adjustmentTransfer.dialogModel
        onCopyAccepted: {
            const result = appModules.adjustmentTransfer.CommitCopy()
            if (result && result.message) {
                host.showSnackbar(result.message)
            }
        }
        // The preset file is written later; a failure of the write is reported then.
        onSaveAsPresetRequested: {
            const result = appModules.presets.SaveDialogSelectionAsPreset()
            if (result && result.success === true) {
                root.pendingDialogPresetSave = true
            } else if (result && result.message) {
                host.showSnackbar(String(result.message))
            }
        }
        onPasteAccepted: function(strategy) {
            adjustmentTransferActionsObj.applyPaste(strategy)
            host.pendingAdjustmentPasteTargets = []
        }
        onPasteCanceled: {
            host.pendingAdjustmentPasteTargets = []
        }
    }

    // Apply Preset... of the image menu (edit presets plan 2.3). A started apply reports its
    // library targets through onPasteFinished below, as Paste Adjustments does.
    ApplyPresetDialog {
        id: applyPresetDialogObj
        blurSource: root.blurSource
        cornerRadius: host.windowCornerRadius
        presets: appModules.presets
        editorPreview: appModules.presetPreview
        libraryPreview: appModules.presetDialogPreview
        editorSession: appModules.editorSession
        onMessageRequested: function(message) {
            host.showSnackbar(message)
        }
        onClosed: host.pendingAdjustmentPasteTargets = []
    }

    // Save as Preset of the Copy dialog: the snackbar names the saved preset or the failure.
    // The Versions panel reports its own preset operations.
    Connections {
        target: appModules.presets
        function onOperationFinished(result) {
            if (String(result.operation || "") !== "save" || !root.pendingDialogPresetSave)
                return
            root.pendingDialogPresetSave = false
            host.showSnackbar(result.success === true ? qsTr("Preset saved.")
                                                      : String(result.message || ""))
        }
    }

    // Library targets paste on a worker thread; the background task bar shows
    // the running Paste and this reports its result.
    Connections {
        target: appModules.adjustmentTransfer
        function onPasteFinished(result) {
            if (result && result.message) {
                host.showSnackbar(String(result.message))
            }
        }
    }

    EditorAdjustmentTransferActions {
        id: adjustmentTransferActionsObj
        adjustmentTransfer: appModules.adjustmentTransfer
        adjustmentTransferDialog: adjustmentTransferDialogObj
        interactionPolicy: appModules.interactionPolicy
        pendingTargets: root.imageActionsController.pendingAdjustmentPasteTargets
        onMessageRequested: function(message) {
            host.showSnackbar(message)
        }
    }

    // The editor filmstrip shares this menu; Discard appears only when the
    // menu was opened from the filmstrip on the image currently loaded in the
    // editor, and requires the session's discard eligibility.
    function filmstripDiscardActions() {
        if (root.imageActionsController.menuOrigin !== "editor-filmstrip") {
            return []
        }
        const session = appModules.editorSession
        if (!session || session.hasImage !== true) {
            return []
        }
        if (Number(host.pendingRatingTarget.elementId) !== Number(session.elementId)) {
            return []
        }
        return [{
            id: "discard-edit",
            label: qsTr("Discard"),
            enabled: session.canDiscardCurrentCommit === true
        }]
    }

    // Mounted folders (Mounted Folder Plan, section 2.3 and MF8). The reads name
    // currentFolderMounted, whose MountStateChanged also announces each new mount state, so the
    // bindings follow a scan.
    function mountedPhotoActions() {
        const folders = appModules.folders
        const elementId = Number(host.pendingRatingTarget.elementId)
        if (folders.currentFolderMounted === undefined || elementId <= 0
                || !folders.IsMissingMountedPhoto(elementId)) {
            return []
        }
        return [{ id: "locate-photo", label: qsTr("Locate Photo..."), enabled: true }]
    }

    // A mounted photo leaves the library only through Remove Missing Photos of its mount, and a
    // mounted folder keeps its photos while their files are present. The menu reads the clicked
    // photo only (#1135): a delete of a selection deletes the other photos and keeps the
    // mounted ones.
    function clickedPhotoStaysMounted() {
        const folders = appModules.folders
        if (folders.currentFolderMounted === true) {
            return true
        }
        if (Number(folders.currentFolderId) !== 0) {
            return false
        }
        return folders.IsMountedPhoto(Number(root.imageActionsController.menuClickedItem.elementId))
    }

    FileDialog {
        id: locatePhotoDialogObj
        property int elementId: 0
        title: qsTr("Locate Photo")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("Supported images (%1)").arg(root.supportedImagePatterns),
            qsTr("All Files (*)")
        ]
        onAccepted: appModules.folders.LocateMissingPhoto(elementId, selectedFile)
    }

    ImageContextMenu {
        id: imageContextMenuObj
        ratingEnabled: Number(host.pendingRatingTarget.imageId) > 0
        currentRating: Math.max(0, Math.min(5, Number(host.pendingRatingTarget.rating || 0)))
        collectionRows: appModules.folders.folders ? appModules.folders.folders : []
        currentCollectionId: Number(appModules.folders.currentFolderId)
        addToEnabled: Number(root.imageActionsController.menuClickedItem.elementId) > 0
        actions: root.filmstripDiscardActions().concat(root.mountedPhotoActions()).concat([
            {
                id: "copy-adjustments",
                label: qsTr("Copy Adjustments"),
                enabled: Number(host.pendingAdjustmentSource.elementId) > 0
            },
            {
                id: "paste-adjustments",
                label: qsTr("Paste Adjustments"),
                enabled: appModules.adjustmentTransfer.packageAvailable
                         && Number(root.imageActionsController.menuClickedItem.elementId) > 0
            },
            {
                id: "apply-preset",
                label: qsTr("Apply Preset..."),
                enabled: Number(root.imageActionsController.menuClickedItem.elementId) > 0
            },
            {
                id: "delete",
                label: Number(appModules.folders.currentFolderId) === 0 ? qsTr("Delete") : qsTr("Remove from Album"),
                // Blocked while a running task holds a DeleteImages lock on the clicked
                // image (the focused image; the reason is focusedDeleteReason). The delete of
                // a selection skips each other locked or mounted image.
                enabled: Number(root.imageActionsController.menuClickedItem.elementId) > 0
                          && appModules.interactionPolicy.canDeleteFocused
                          && !root.clickedPhotoStaysMounted()
            }
        ])
        onRatingRequested: function(rating) {
            imageContextMenuObj.close()
            root.imageActionsController.requestSetImageRating(rating)
        }
        onActionRequested: function(actionId) {
            imageContextMenuObj.close()
            if (actionId === "discard-edit") {
                if (appModules.editorSession
                        && appModules.editorSession.canDiscardCurrentCommit === true) {
                    appModules.editorSession.Discard()
                }
                return
            }
            if (actionId === "locate-photo") {
                const elementId = Number(host.pendingRatingTarget.elementId)
                locatePhotoDialogObj.elementId = elementId
                locatePhotoDialogObj.currentFolder =
                    appModules.folders.MissingPhotoLocateFolder(elementId)
                locatePhotoDialogObj.open()
                return
            }
            if (actionId === "copy-adjustments") {
                root.imageActionsController.requestCopyAdjustments()
                return
            }
            if (actionId === "paste-adjustments") {
                root.imageActionsController.preparePasteTargets()
                adjustmentTransferActionsObj.requestPasteAdjustments()
                return
            }
            if (actionId === "apply-preset") {
                root.imageActionsController.requestApplyPreset()
                return
            }
            if (actionId === "delete") {
                root.imageActionsController.requestDeleteConfirmation()
            }
        }
        onExportQueueRequested: {
            imageContextMenuObj.close()
            root.imageActionsController.addTargetsToExportQueue()
        }
        onCollectionRequested: function(folderId) {
            imageContextMenuObj.close()
            root.imageActionsController.runAddTargetsToAlbum(folderId)
        }
    }

    NikonHeRecoveryDialog {
        id: nikonHeRecoveryDialogObj
        parent: Overlay.overlay
        backgroundSource: root.blurSource
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

    BackgroundTasksDialog {
        id: backgroundTasksDialogObj
        controller: appModules.backgroundTasks
        blurSource: root.blurSource
        cornerRadius: root.host ? root.host.windowCornerRadius : 0
    }

    SemanticGenerationDialog {
        id: semanticGenerationDialogObj
        parent: Overlay.overlay
        backgroundSource: root.blurSource
        promptVisible: host.semanticGeneration.promptVisible
        generationRunning: host.semanticGeneration.running
        pendingCount: host.semanticGeneration.pendingCount
        total: host.semanticGeneration.total
        embedded: host.semanticGeneration.embedded
        skipped: host.semanticGeneration.skipped
        failed: host.semanticGeneration.failed
        canceled: host.semanticGeneration.canceled
        remainingSeconds: host.semanticGeneration.remainingSeconds
        onStartRequested: function(rememberChoice) {
            if (rememberChoice) {
                host.semanticGeneration.SetImportPreference("always")
            }
            host.semanticGeneration.StartPendingGeneration(false)
        }
        onSkipRequested: function(rememberChoice) {
            host.semanticGeneration.SkipPendingGeneration(rememberChoice)
        }
        onCancelRequested: host.semanticGeneration.CancelGeneration()
    }

    DeleteConfirmDialog {
        id: deleteConfirmDialogObj
        theme: root.host
        host: root.host
        blurSource: root.blurSource
        onCancelled: root.host.pendingDeleteTargets = []
        onConfirmed: root.imageActionsController.runDeleteTargets()
    }

    // Above Settings and Welcome (z 30): the window's close button stays usable
    // while they are open, and the Save / Discard question must show on top.
    EditorCloseConfirmDialog {
        id: editorCloseConfirmDialogObj
        parent: Overlay.overlay
        z: 40
        theme: root.host
        host: root.host
        blurSource: root.blurSource
        onSaveRequested: root.host.beginEditorCloseSave()
        onDiscardRequested: root.host.beginEditorCloseDiscard()
        onCancelled: root.host.cancelEditorCloseConfirm()
    }

    // Overview data of the previewed project for the welcome surface.
    WelcomeProjectPreviewAdapter {
        id: welcomePreviewAdapterObj
        active: welcomeDialogObj.visible
        thumbnailMaxEdge: host.libraryThumbnailMaxEdge
    }

    WelcomeDialog {
        id: welcomeDialogObj
        objectName: "welcomeDialog"
        z: 30
        windowHost: root.host
        blurSource: root.blurSource
        cornerRadius: host.windowCornerRadius
        adapter: welcomePreviewAdapterObj
        recentProjects: appModules.project.recentProjects
        languageOptions: host.languageOptions
        currentLanguageIndex: host.languageIndexForCode(languageManager.currentLanguageCode)
        acceleratorWarning: appModules.project.acceleratorWarning
        serviceMessage: appModules.project.serviceMessage
        updateService: appModules.updates
        // A row previews that project under the welcome surface; the upper
        // area shows the skeleton until the load completes.
        onPreviewRequested: function(projectPath) {
            appModules.projectLaunch.PreviewProject(projectPath)
        }
        onContinueRequested: host.continueWelcomeProject()
        onOpenRequested: appModules.projectLaunch.BeginPromptOpen()
        onCreateRequested: function(storageLocation, projectName) {
            appModules.projectLaunch.BeginCreateAsUser(storageLocation, projectName)
        }
        onExitRequested: Qt.quit()
        onLanguageRequested: function(languageCode) {
            languageManager.setLanguage(languageCode)
        }
        onAcceleratorWarningAcknowledged: appModules.project.AcknowledgeAcceleratorWarning()
        onClosed: appModules.projectLaunch.StartPendingLaunch()
    }

    SearchPalette {
        id: globalSearchDialogObj
        objectName: "globalSearchDialog"
        searchController: appModules.search
        interactionPolicyController: appModules.interactionPolicy
        blurSource: root.blurSource
        cornerRadius: host.windowCornerRadius
    }

    // True while any modal dialog/menu is open — used by Main's Select-All
    // shortcut admission check (formerly selectionShortcutBlocked).
    function anyDialogOpened() {
        return settingsDialogObj.opened
               || adjustmentTransferDialogObj.opened
               || applyPresetDialogObj.opened
               || nikonHeRecoveryDialogObj.opened
               || semanticGenerationDialogObj.opened
               || backgroundTasksDialogObj.opened
               || deleteConfirmDialogObj.opened
               || importFolderFlowObj.opened
               || editorCloseConfirmDialogObj.opened
               || welcomeDialogObj.opened
    }

    function openEditorCloseConfirmDialog() {
        editorCloseConfirmDialogObj.openForClose()
    }

    function openSettingsDialog(category) {
        settingsDialogObj.requestedCategory = category === undefined ? 0 : category
        settingsDialogObj.open()
    }

    function openUpdateSettings() {
        openSettingsDialog(settingsDialogObj.updatesCategory)
    }

    function openLutSettings() {
        openSettingsDialog(settingsDialogObj.lutCategory)
    }

    function openAiModelSettings() {
        openSettingsDialog(settingsDialogObj.aiModelsCategory)
    }

    // Settings > Language / Theme, scrolled to Library > Color Labels.
    function openColorLabelSettings() {
        settingsDialogObj.revealColorLabelsOnOpen = true
        openSettingsDialog(settingsDialogObj.colorLabelsCategory)
    }

    function openBackgroundTasksDialog() {
        backgroundTasksDialogObj.open()
    }

    function openLutImportDialog() {
        lutImportDialogObj.open()
    }

}
