import QtQuick
import QtQml

// Owns project open/create launch orchestration: the welcome-dialog visibility
// state machine, the launch + accelerator-preparation timers, and the
// save-project / language-index helpers used by the welcome and File menu.
// State lives here; Main exposes it through aliases so existing bindings and
// the Connections routers keep resolving. appModules is a global context
// property; `welcomeDialog` is assigned by the host on completion.
// Launch stays in the already-open window: the loading overlay is raised first,
// then welcome is dismissed, so the empty library never flashes in between.
Item {
    id: root
    property var host: null
    property var welcomeDialog: null
    property bool automationMode: false

    property bool projectLaunchPending: false
    property bool welcomeDismissedForLaunch: false
    property var pendingProjectLaunchAction: null
    property bool restoreWelcomeOnProjectLaunchFailure: false
    property bool welcomeOpenScheduled: false
    property int welcomeReadyAttempts: 0

    readonly property bool projectLoadingOverlayVisible: root.projectLaunchPending || appModules.project.projectLoading
    readonly property bool projectLaunchBusy: root.projectLoadingOverlayVisible || root.pendingProjectLaunchAction !== null

    onWelcomeDismissedForLaunchChanged: root.updateWelcomeDialogVisibility()

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

    // Called from the host's Component.onCompleted once welcomeDialog is wired.
    function start() {
        if (!root.automationMode) {
            acceleratorPreparationStartTimer.start()
        }
        // Open welcome only after the shell has a real size so MultiEffect
        // snapshots the loaded main UI, same as SettingDialog / other modals.
        root.scheduleWelcomeOpen()
    }

    function scheduleWelcomeOpen() {
        if (root.welcomeOpenScheduled) {
            return
        }
        root.welcomeOpenScheduled = true
        Qt.callLater(root.openWelcomeAfterShellReady)
    }

    function openWelcomeAfterShellReady() {
        root.welcomeOpenScheduled = false
        const shell = root.host
        const workspace = shell ? shell.workspaceLayer : null
        const sizeReady = !!shell && Number(shell.width) > 0 && Number(shell.height) > 0
        const libraryReady = !workspace || workspace.libraryItem
        if (sizeReady && libraryReady) {
            root.welcomeReadyAttempts = 0
            root.updateWelcomeDialogVisibility()
            return
        }
        if (root.welcomeReadyAttempts > 30) {
            root.welcomeReadyAttempts = 0
            root.updateWelcomeDialogVisibility()
            return
        }
        root.welcomeReadyAttempts += 1
        root.scheduleWelcomeOpen()
    }

    function showSnackbar(messageText) {
        if (root.host) root.host.showSnackbar(messageText)
    }

    function requestSaveProject() {
        const ok = appModules.project.SaveProject()
        if (ok) {
            root.showSnackbar(appModules.project.serviceMessage)
        }
    }

    function languageIndexForCode(code) {
        const options = root.host ? root.host.languageOptions : []
        for (let i = 0; i < options.length; ++i) {
            if (options[i].code === code) {
                return i
            }
        }
        return 0
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
        // Show the loading overlay in the same window before closing welcome so
        // the empty library never flashes between the two surfaces.
        root.startPendingProjectLaunch()
        root.dismissWelcomeForProjectLaunch()
        root.updateWelcomeDialogVisibility()
    }

    function startPendingProjectLaunch() {
        if (!root.pendingProjectLaunchAction || projectLaunchTimer.running) {
            return
        }
        root.projectLaunchPending = true
        projectLaunchTimer.restart()
    }

    function updateWelcomeDialogVisibility() {
        const shouldShowWelcome = !root.welcomeDismissedForLaunch
                                  && !root.automationMode
                                  && !appModules.project.serviceReady
                                  && !appModules.project.projectLoading
        if (!root.welcomeDialog) {
            return
        }
        if (shouldShowWelcome) {
            if (!root.welcomeDialog.opened) {
                root.welcomeDialog.open()
            }
        } else if (root.welcomeDialog.opened) {
            root.welcomeDialog.close()
        }
    }
}
