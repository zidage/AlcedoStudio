import QtQuick
import QtQml

// Centralized shell signal routing extracted from Main.qml: language reload,
// image-analysis completion, project open/close, folder switch, library
// thumbnail refresh, and import/export session transitions. The import/export
// session bookkeeping (importSessionObserved / exportSessionObserved /
// lastObservedExportCompleted) lives here. `host` is Main; the controllers,
// state objects, dialogs, and window animations are passed in as properties.
Item {
    id: root
    property var host: null
    property var imageActionsController: null
    property var selectionState: null
    property var exportQueueState: null
    property var deleteConfirmDialog: null
    property var windowAnimations: null

    property bool importSessionObserved: false
    property bool exportSessionObserved: false
    property int lastObservedExportCompleted: 0

    Connections {
        target: languageManager
        function onEffectiveLanguageCodeChanged() {
            if (root.imageActionsController) {
                root.imageActionsController.refreshFocusedImageInspection()
            }
        }
    }

    Connections {
        target: appModules.imageAnalysis
        ignoreUnknownSignals: true
        function onStateChanged() {
            if (!appModules.imageAnalysis.running
                    && root.imageActionsController
                    && root.imageActionsController.analysisResultTouchesFocusedImage()) {
                root.imageActionsController.refreshFocusedImageInspection()
            }
        }
    }

    Connections {
        target: appModules.project
        ignoreUnknownSignals: true
        function onProjectChanged() {
            if (root.host) {
                root.host.projectLaunchPending = false
                root.host.welcomeDismissedForLaunch = false
                root.host.updateWelcomeDialogVisibility()
            }
            if (root.selectionState) {
                root.selectionState.clearSelectedImages()
            }
            if (root.exportQueueState) {
                root.exportQueueState.clearQueue()
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

            // Auto-maximize when a project is successfully opened.
            if (appModules.project.serviceReady && root.host && !root.host.windowMaximized
                    && root.windowAnimations && !root.windowAnimations.maximizeRunning) {
                root.windowAnimations.maximize()
            }
        }
        function onServiceStateChanged() {
            if (root.host) {
                root.host.updateWelcomeDialogVisibility()
            }
        }
        function onProjectLoadStateChanged() {
            if (root.host) {
                root.host.projectLaunchPending = false
            }
            if (!appModules.project.projectLoading && root.host) {
                root.host.welcomeDismissedForLaunch = false
            }
            if (root.host) {
                root.host.updateWelcomeDialogVisibility()
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
                root.host.setFocusedImage(null)
            }
            if (root.deleteConfirmDialog) {
                root.deleteConfirmDialog.close()
            }
        }
    }

    Connections {
        target: appModules.library
        ignoreUnknownSignals: true
        function onThumbnailsChanged() {
            if (root.exportQueueState && root.exportQueueState.exportQueueCount > 0) {
                root.exportQueueState.refreshExportPreview()
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
            if (root.host) {
                root.host.showSnackbar(qsTr("Imported %1 image(s).").arg(appModules.importExport.importCompleted))
            }
        }
        function onExportStateChanged() {
            if (appModules.importExport.exportInFlight) {
                root.exportSessionObserved = true
                if (appModules.importExport.exportCompleted > root.lastObservedExportCompleted) {
                    if (root.exportQueueState) {
                        root.exportQueueState.pruneCompleted(appModules.importExport.exportItemStatuses)
                    }
                    root.lastObservedExportCompleted = appModules.importExport.exportCompleted
                }
                return
            }
            if (root.exportQueueState) {
                root.exportQueueState.pruneCompleted(appModules.importExport.exportItemStatuses)
            }
            root.lastObservedExportCompleted = 0
            if (!root.exportSessionObserved) {
                return
            }
            root.exportSessionObserved = false
            if (root.host) {
                root.host.showSnackbar(qsTr("Exported %1 image(s).").arg(appModules.importExport.exportSucceeded))
            }
        }
    }
}