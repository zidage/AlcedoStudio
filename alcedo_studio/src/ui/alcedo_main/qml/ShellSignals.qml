import QtQuick
import QtQml

// Centralized shell signal routing extracted from Main.qml: language reload,
// project open/close, folder switch, and
// import/export session transitions. The import session bookkeeping
// (importSessionObserved) lives here. The export queue (appModules.exportQueue)
// clears itself on a project change and removes the finished entries itself. `host` is Main; the controllers, state objects, and
// dialogs are passed in as properties.
Item {
    id: root
    property var host: null
    property var imageActionsController: null
    property var selectionState: null
    property var deleteConfirmDialog: null

    property bool importSessionObserved: false

    Connections {
        target: languageManager
        function onEffectiveLanguageCodeChanged() {
            if (root.imageActionsController) {
                root.imageActionsController.refreshFocusedImageInspection()
            }
        }
    }

    Connections {
        target: appModules.project
        ignoreUnknownSignals: true
        function onProjectChanged() {
            if (root.host) {
                root.host.updateWelcomeDialogVisibility()
            }
            if (root.selectionState) {
                root.selectionState.clearSelectedImages()
            }
            if (root.host) {
                root.host.pendingDeleteTargets = []
                root.host.pendingRatingTarget = ({})
                root.host.setFocusedImage(null)
            }
            if (root.deleteConfirmDialog) {
                root.deleteConfirmDialog.close()
            }
            if (root.host) {
                root.host.showSnackbar(appModules.project.serviceMessage)
            }
        }
        // The welcome visibility rule reads projectEntered and the launch state
        // (ProjectLaunchController), not serviceReady or projectLoading.
        function onProjectEnteredChanged() {
            if (root.host) {
                root.host.updateWelcomeDialogVisibility()
            }
        }
        // acceleratorPreparing NOTIFY AcceleratorPreparationStateChanged. A handler for that
        // signal itself does not run (upper-case name).
        function onAcceleratorPreparingChanged() {
            if (root.host && !appModules.project.acceleratorPreparing) {
                root.host.requestStartupPreview()
            }
        }
    }

    Connections {
        target: appModules.folders
        ignoreUnknownSignals: true
        function onFolderSelectionChanged() {
            if (root.selectionState) {
                root.selectionState.clearSelectedImages()
            }
            if (root.host) {
                root.host.pendingDeleteTargets = []
                root.host.pendingRatingTarget = ({})
            }
            if (root.deleteConfirmDialog) {
                root.deleteConfirmDialog.close()
            }
            const inEditor = appModules.workspaceRouter
                             && String(appModules.workspaceRouter.workspace || "") === "editor"
            if (inEditor && root.imageActionsController
                    && root.imageActionsController.requestEditorLibraryListSync) {
                root.imageActionsController.requestEditorLibraryListSync()
                return
            }
            if (root.host) {
                root.host.setFocusedImage(null)
            }
        }
    }

    // Stats filter and search changes reach the editor through the accepted
    // library result (ImageActionsController, LibraryModule.queryUpdating).
    // StatsFilterChanged and SearchStateChanged start with an upper-case
    // letter, so a QML handler for them does not run.

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
            if (root.host) {
                root.host.showSnackbar(appModules.importExport.importStatus)
            }
        }
        function onExportFinished(succeeded, failed, skipped) {
            if (root.host) {
                root.host.showSnackbar(qsTr("Exported %1 image(s).").arg(succeeded))
            }
        }
    }
}