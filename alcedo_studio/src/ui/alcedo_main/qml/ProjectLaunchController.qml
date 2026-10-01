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
// The welcome surface stays open until the user enters a project
// (appModules.project.projectEntered). At startup it previews the most recent
// project: the project loads under the welcome surface without the loading ring.
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
    // The startup preview runs once, after the welcome surface opens and the
    // accelerator preparation has finished.
    property bool startupPreviewRequested: false

    // A preview load runs under the welcome surface, so only enter-mode work shows the ring.
    readonly property bool projectLoadingOverlayVisible: root.projectLaunchPending
                                                         || (appModules.project.projectLoading
                                                             && appModules.project.projectLoadEntryMode === "enter")
    readonly property bool projectLaunchBusy: root.projectLoadingOverlayVisible || root.pendingProjectLaunchAction !== null

    onWelcomeDismissedForLaunchChanged: root.updateWelcomeDialogVisibility()

    Timer {
        id: acceleratorPreparationStartTimer
        interval: 16
        repeat: false
        onTriggered: {
            appModules.project.StartAcceleratorPreparation()
            root.requestStartupPreview()
        }
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
            root.requestStartupPreview()
            return
        }
        if (root.welcomeReadyAttempts > 30) {
            root.welcomeReadyAttempts = 0
            root.updateWelcomeDialogVisibility()
            root.requestStartupPreview()
            return
        }
        root.welcomeReadyAttempts += 1
        root.scheduleWelcomeOpen()
    }

    // Previews the first recent project once the welcome surface is open. Waits
    // for the accelerator preparation; ShellSignals calls again when it ends.
    function requestStartupPreview() {
        if (root.startupPreviewRequested || root.automationMode) {
            return
        }
        // `visible` is true from open(); `opened` waits for the enter transition.
        if (!root.welcomeDialog || !root.welcomeDialog.visible) {
            return
        }
        if (appModules.project.acceleratorPreparing) {
            return
        }
        root.startupPreviewRequested = true
        const recent = appModules.project.recentProjects
        if (!recent || recent.length === 0 || appModules.project.serviceReady
                || appModules.project.projectLoading || root.projectLaunchBusy) {
            return
        }
        appModules.project.PreviewProject(String(recent[0].path || ""))
    }

    // Enters the previewed project. When the preview load still runs, the load
    // changes to enter mode: the welcome surface closes and the ring shows until
    // the Library is visible.
    function continueWelcomeProject() {
        const loadRunning = appModules.project.projectLoading
        if (!appModules.project.EnterLoadedProject()) {
            return false
        }
        if (loadRunning) {
            root.dismissWelcomeForProjectLaunch()
            root.updateWelcomeDialogVisibility()
            return true
        }
        root.updateWelcomeDialogVisibility()
        if (root.host && root.host.revealLibraryAfterProjectLoad) {
            root.host.revealLibraryAfterProjectLoad()
        }
        return true
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
        root.restoreWelcomeOnProjectLaunchFailure = !appModules.project.projectEntered
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
        // welcomeDismissedForLaunch covers the enter-mode load that runs while no
        // project is entered yet; the ring shows in place of the welcome surface.
        const shouldShowWelcome = !root.welcomeDismissedForLaunch
                                  && !root.automationMode
                                  && !appModules.project.projectEntered
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
