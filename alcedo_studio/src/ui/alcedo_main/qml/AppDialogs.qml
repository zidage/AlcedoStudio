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
    property var exportQueueState: null
    property Item blurSource: null

    property alias importDialog: importDialogObj
    property alias importFolderDialog: importFolderDialogObj
    property alias exportDialog: exportDialogObj
    property alias settingsDialog: settingsDialogObj
    property alias adjustmentTransferDialog: adjustmentTransferDialogObj
    property alias adjustmentTransferActions: adjustmentTransferActionsObj
    property alias imageContextMenu: imageContextMenuObj
    property alias nikonHeRecoveryDialog: nikonHeRecoveryDialogObj
    property alias advancedContentAnalysisDialog: advancedContentAnalysisDialogObj
    property alias semanticGenerationDialog: semanticGenerationDialogObj
    property alias deleteConfirmDialog: deleteConfirmDialogObj
    property alias editorCloseConfirmDialog: editorCloseConfirmDialogObj
    property alias welcomeDialog: welcomeDialogObj
    property alias globalSearchDialog: globalSearchDialogObj
    property alias backgroundTasksDialog: backgroundTasksDialogObj

    FileDialog {
        id: importDialogObj
        title: qsTr("Select Images")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("All Files (*)")]
        onAccepted: {
            const files = []
            for (let i = 0; i < selectedFiles.length; ++i) {
                files.push(selectedFiles[i].toString())
            }
            appModules.importExport.StartImport(files)
        }
    }

    FolderDialog {
        id: importFolderDialogObj
        title: qsTr("Select Folder to Import")
        onAccepted: {
            const folderUrl = selectedFolder.toString()
            const files = appModules.importExport.CollectFolderFiles(folderUrl)
            if (!files || files.length === 0) {
                host.showSnackbar(qsTr("No files found in the selected folder."))
                return
            }
            folderImportConfirmDialogObj.openWith(folderUrl, files)
        }
    }

    FolderImportConfirmDialog {
        id: folderImportConfirmDialogObj
        parent: Overlay.overlay
        theme: host
        host: host
        blurSource: root.blurSource
        onConfirmed: function(filePaths) {
            appModules.importExport.StartImport(filePaths)
        }
    }

    AlbumExportDialog {
        id: exportDialogObj
        blurSource: root.blurSource
        selectedCount: host.selectedCount
        exportQueueCount: host.exportQueueCount
        exportPreviewRows: host.exportPreviewRows
        hdrExportAvailable: root.exportQueueState.hasHdrItems()
        onAddSelectedToQueueRequested: {
            root.exportQueueState.addTargets(root.selectionState.currentSelectedItems())
            root.selectionState.clearSelectedImages()
        }
        onClearQueueRequested: root.exportQueueState.clearQueue()
        onEnsurePreviewRequested: root.exportQueueState.refreshExportPreview()
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
                root.exportQueueState.exportQueueTargets())
        }
    }

    SettingDialog {
        id: settingsDialogObj
        objectName: "settingsDialog"
        z: 30
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
        onCopyAccepted: function(selectedKeys, versionId) {
            const result = appModules.adjustmentTransfer.CopyVersion(
                Number(host.pendingAdjustmentSource.elementId),
                versionId,
                selectedKeys)
            if (result && result.message) {
                host.showSnackbar(result.message)
            }
        }
        onPasteAccepted: function(strategy) {
            adjustmentTransferActionsObj.applyPaste(strategy)
            host.pendingAdjustmentPasteTargets = []
        }
        onPasteDiscarded: {
            appModules.adjustmentTransfer.Discard()
            host.pendingAdjustmentPasteTargets = []
        }
    }

    EditorAdjustmentTransferActions {
        id: adjustmentTransferActionsObj
        adjustmentTransfer: appModules.adjustmentTransfer
        adjustmentTransferDialog: adjustmentTransferDialogObj
        interactionPolicy: appModules.interactionPolicy
        pendingTargets: host.pendingAdjustmentPasteTargets
        onMessageRequested: function(message) {
            host.showSnackbar(message)
        }
    }

    // The editor filmstrip shares this menu; Discard appears only when the
    // menu was opened from the filmstrip on the image currently loaded in the
    // editor, and stays gated by the session's discard eligibility.
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

    ImageContextMenu {
        id: imageContextMenuObj
        ratingEnabled: Number(host.pendingRatingTarget.imageId) > 0
        currentRating: Math.max(0, Math.min(5, Number(host.pendingRatingTarget.rating || 0)))
        actions: root.filmstripDiscardActions().concat([
            {
                id: "copy-adjustments",
                label: qsTr("Copy Adjustments"),
                enabled: Number(host.pendingAdjustmentSource.elementId) > 0
            },
            {
                id: "paste-adjustments",
                label: qsTr("Paste Adjustments"),
                enabled: appModules.adjustmentTransfer.packageAvailable
                         && host.pendingAdjustmentPasteTargets.length > 0
            },
            {
                id: "delete",
                label: Number(appModules.folders.currentFolderId) === 0 ? qsTr("Delete") : qsTr("Remove from Album"),
                // Phase 2: blocked while the selected images are in an active
                // analysis set (DeleteImages lock). The reason is exposed on the
                // policy controller as pendingDeleteReason.
                enabled: host.pendingDeleteTargets.length > 0
                          && appModules.interactionPolicy.canDeletePendingTargets
            }
        ]).concat(root.imageActionsController.albumTargetActions())
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
            if (actionId === "copy-adjustments") {
                root.imageActionsController.requestCopyAdjustments()
                return
            }
            if (actionId === "paste-adjustments") {
                adjustmentTransferActionsObj.requestPasteAdjustments()
                return
            }
            if (actionId === "delete") {
                root.imageActionsController.requestDeleteConfirmation()
                return
            }
            if (String(actionId).indexOf("add-to-album:") === 0) {
                root.imageActionsController.runAddTargetsToAlbum(Number(String(actionId).split(":")[1]))
            }
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

    AdvancedContentAnalysisDialog {
        id: advancedContentAnalysisDialogObj
        objectName: "advancedContentAnalysisDialog"
        parent: Overlay.overlay
        blurSource: root.blurSource
        imageController: appModules.images
        analysisController: appModules.imageAnalysis
        profileController: appModules.aiProviderProfiles
        interactionPolicy: appModules.interactionPolicy
        backendInteractive: host.backendInteractive
        onMessageRequested: function(message) {
            host.showSnackbar(message)
        }
    }

    BackgroundTasksDialog {
        id: backgroundTasksDialogObj
        controller: appModules.backgroundTasks
        blurSource: root.blurSource
        cornerRadius: root.host ? root.host.windowCornerRadius : 0
        onTaskDetailsRequested: function(task) {
            if (task && task.kind === "imageAnalysis")
                advancedContentAnalysisDialogObj.openTaskDetails(task)
        }
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
        statusText: host.semanticGeneration.statusText
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

    EditorCloseConfirmDialog {
        id: editorCloseConfirmDialogObj
        parent: Overlay.overlay
        theme: root.host
        host: root.host
        blurSource: root.blurSource
        onSaveRequested: root.host.beginEditorCloseSave()
        onDiscardRequested: root.host.beginEditorCloseDiscard()
        onCancelled: root.host.cancelEditorCloseConfirm()
    }

    WelcomeDialog {
        id: welcomeDialogObj
        objectName: "welcomeDialog"
        z: 30
        blurSource: root.blurSource
        cornerRadius: host.windowCornerRadius
        recentProjects: appModules.project.recentProjects
        languageOptions: host.languageOptions
        currentLanguageIndex: host.languageIndexForCode(languageManager.currentLanguageCode)
        acceleratorWarning: appModules.project.acceleratorWarning
        serviceMessage: appModules.project.serviceMessage
        headlineFontFamily: host.headlineFontFamily
        primaryAccent: host.colButtonPrimary
        secondaryAccent: host.colAccentSecondary
        textColor: host.colText
        mutedTextColor: host.colTextMuted
        panelColor: host.colBgPanel
        panelBorderColor: host.withAlpha(host.colText, 0.08)
        overlayColor: host.colOverlay
        baseColor: host.colBgCanvas
        onLoadRequested: {
            host.beginProjectLaunch(function() {
                return appModules.project.PromptAndLoadProject()
            })
        }
        onCreateRequested: function(projectName, storageLocation) {
            host.beginProjectLaunch(function() {
                return appModules.project.CreateProjectInFolderNamed(storageLocation, projectName)
            })
        }
        onExitRequested: Qt.quit()
        onLanguageRequested: function(languageCode) {
            languageManager.setLanguage(languageCode)
        }
        onAcceleratorWarningAcknowledged: appModules.project.AcknowledgeAcceleratorWarning()
        onRecentProjectRequested: function(projectPath) {
            host.beginProjectLaunch(function() {
                return appModules.project.LoadProject(projectPath)
            })
        }
        onClosed: host.startPendingProjectLaunch()
    }

    GlobalSearchDialog {
        id: globalSearchDialogObj
        objectName: "globalSearchDialog"
        searchController: appModules.search
        interactionPolicyController: appModules.interactionPolicy
        theme: host
        blurSource: root.blurSource
        cornerRadius: host.windowCornerRadius
    }

    // True while any modal dialog/menu is open — used by Main's Select-All
    // shortcut gate (formerly selectionShortcutBlocked).
    function anyDialogOpened() {
        return exportDialogObj.opened
               || settingsDialogObj.opened
               || adjustmentTransferDialogObj.opened
               || nikonHeRecoveryDialogObj.opened
               || semanticGenerationDialogObj.opened
               || advancedContentAnalysisDialogObj.opened
               || backgroundTasksDialogObj.opened
               || deleteConfirmDialogObj.opened
               || folderImportConfirmDialogObj.opened
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

    function openAdvancedAnalysisDialog() {
        const targets = root.selectionState.currentSelectedItems()
        advancedContentAnalysisDialogObj.openWithTargets(targets)
        if (targets.length <= 0) {
            host.showSnackbar(qsTr("Select at least one image to analyze."))
        }
    }

    function openBackgroundTasksDialog() {
        backgroundTasksDialogObj.open()
    }
}
