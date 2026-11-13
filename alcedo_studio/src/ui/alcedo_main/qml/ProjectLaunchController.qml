import QtQuick
import QtQml

// Presentation of the project launch: the welcome-dialog visibility rule, the
// accelerator-preparation start timer, the startup preview, and the save-project /
// language-index helpers used by the welcome and File menu. The launch sequence and its
// rollback live in C++ (appModules.projectLaunch, ProjectLaunchCoordinator); this item reads
// its state. appModules is a global context property; `welcomeDialog` is assigned by the host
// on completion.
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

    property bool welcomeOpenScheduled: false
    property int welcomeReadyAttempts: 0
    // The startup preview runs once, after the welcome surface opens and the
    // accelerator preparation has finished.
    property bool startupPreviewRequested: false

    readonly property bool projectLoadingOverlayVisible: appModules.projectLaunch.loadingOverlayVisible
    readonly property bool projectLaunchBusy: appModules.projectLaunch.launchBusy

    Connections {
        target: appModules.projectLaunch

        function onLaunchStateChanged() {
            root.updateWelcomeDialogVisibility()
        }
    }

    Timer {
        id: acceleratorPreparationStartTimer
        interval: 16
        repeat: false
        onTriggered: {
            appModules.project.StartAcceleratorPreparation()
            root.requestStartupPreview()
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
        if (!appModules.projectLaunch.ContinueWelcomeProject()) {
            return false
        }
        root.updateWelcomeDialogVisibility()
        if (loadRunning) {
            return true
        }
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

    function updateWelcomeDialogVisibility() {
        // welcomeDismissedForLaunch covers the enter-mode load that runs while no
        // project is entered yet; the ring shows in place of the welcome surface.
        const shouldShowWelcome = !appModules.projectLaunch.welcomeDismissedForLaunch
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
