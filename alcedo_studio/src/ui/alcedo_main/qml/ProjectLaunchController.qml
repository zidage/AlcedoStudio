import QtQuick
import QtQml

// Owns project open/create launch orchestration: the welcome-dialog visibility
// state machine, the launch + accelerator-preparation timers, and the
// save-project / language-index helpers used by the welcome and File menu.
// State lives here; Main exposes it through aliases so existing bindings and
// the Connections routers keep resolving. appModules is a global context
// property; `welcomeDialog` is assigned by the host on completion.
Item {
    id: root
    property var host: null
    property var welcomeDialog: null
    property bool automationMode: false

    property bool projectLaunchPending: false
    property bool welcomeDismissedForLaunch: false
    property var pendingProjectLaunchAction: null
    property bool restoreWelcomeOnProjectLaunchFailure: false

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
        root.updateWelcomeDialogVisibility()
        if (!root.automationMode) {
            acceleratorPreparationStartTimer.start()
        }
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
        root.dismissWelcomeForProjectLaunch()
        root.updateWelcomeDialogVisibility()
        if (root.welcomeDialog && !root.welcomeDialog.opened && !root.welcomeDialog.visible) {
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
