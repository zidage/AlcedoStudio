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
    // A snackbar that reports files left to a mounted folder stays longer than the default.
    readonly property int leftToMountsSnackbarMs: 5000

    // The files of a user import that are already in a mounted folder: the scan of that
    // folder imports them, so the import leaves them out (#1016, #1089).
    function leftToMountsText(fileCount) {
        return qsTr("%n photo(s) are already in the mounted folder and will not be imported this time.",
                    "", fileCount)
    }

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
            if (!root.host) {
                return
            }
            const imported = qsTr("Imported %1 image(s).").arg(appModules.importExport.importCompleted)
            const leftToMounts = appModules.importExport.importLeftToMounts
            if (leftToMounts > 0) {
                root.host.showSnackbar(imported + "\n" + root.leftToMountsText(leftToMounts),
                                       root.leftToMountsSnackbarMs)
            } else {
                root.host.showSnackbar(imported)
            }
        }
        // #1089: no import runs, so the import state change above does not report these files.
        function onImportLeftAllToMounts(fileCount) {
            if (root.host) {
                root.host.showSnackbar(root.leftToMountsText(fileCount), root.leftToMountsSnackbarMs)
            }
        }
        function onExportFinished(succeeded, failed, skipped) {
            if (root.host) {
                root.host.showSnackbar(qsTr("Exported %1 image(s).").arg(succeeded))
            }
        }
    }
}