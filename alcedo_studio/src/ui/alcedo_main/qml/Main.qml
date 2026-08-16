import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Effects

ApplicationWindow {
    id: root
    objectName: "mainWindow"
    // The test host supplies this context property before loading QML. The
    // typeof guard keeps source-loaded QML fixtures compatible with the
    // production context, where the property is explicitly set to false.
    readonly property bool automationModeEnabled: typeof automationMode === "boolean"
                                                  && automationMode
    readonly property bool startMaximizedRequested: typeof startMaximized === "boolean"
                                                    && startMaximized
    readonly property bool nativeFrameManagedEnabled: typeof nativeFrameManaged === "boolean"
                                                       && nativeFrameManaged
    // macOS keeps the system traffic lights and hides the title-bar surface.
    // Qt.platform.os is still "osx" on Qt 6.9; accept "macos" if that changes.
    readonly property bool nativeTrafficLightsEnabled: Qt.platform.os === "osx"
                                                       || Qt.platform.os === "macos"
    property bool nativeFrameReady: !root.nativeFrameManagedEnabled
    property bool collectionsSidebarExpanded: true
    property bool collectionsWorkspaceObservationReady: false
    property real collectionsSidebarGap: collectionsSidebarExpanded
                                                 ? appTheme.spaceMd : 0
    readonly property string activeWorkspace: appModules.workspaceRouter
                                              ? String(appModules.workspaceRouter.workspace
                                                       || "library")
                                              : "library"
    width: 1200
    height: 760
    minimumWidth: 960
    minimumHeight: 640
    visible: root.nativeFrameReady
    // Production (startMaximized context property) opens as the real app.
    // Tests and the automation host omit the property and stay windowed.
    visibility: !root.nativeFrameReady
                ? Window.Hidden
                : root.startMaximizedRequested ? Window.Maximized : Window.Windowed
    title: qsTr("Alcedo Studio")
    flags: root.nativeFrameManagedEnabled
           ? Qt.Window | Qt.WindowTitleHint | Qt.WindowSystemMenuHint
             | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint
             | Qt.WindowCloseButtonHint
           : root.nativeTrafficLightsEnabled
             ? Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
               | Qt.WindowTitleHint | Qt.WindowSystemMenuHint
               | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint
               | Qt.WindowCloseButtonHint
             : Qt.Window | Qt.FramelessWindowHint
    // Keep custom chrome edge-to-edge. The toolbar reserves its leading region
    // so interactive content does not sit under the macOS traffic lights.
    topPadding: 0
    leftPadding: 0
    rightPadding: 0
    bottomPadding: 0
    font.family: appTheme.uiFontFamily

    readonly property bool windowMaximized: visibility === Window.Maximized || visibility === Window.FullScreen
    readonly property real maximizedInset: 0
    // Snap radius — animating it together with the OS resize causes layout jitter.
    // Native macOS windows already clip to the system corner; do not double-round.
    readonly property real windowCornerRadius: (windowMaximized || root.nativeTrafficLightsEnabled) ? 0 : 12

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
    // Semantic card surface family — the same base/border the Library grid
    // (ThumbnailGridView) uses for its card wells. Editor cards resolve here so
    // History/Versions, adjustment shells, the filmstrip dock, and the viewport
    // placeholder share one documented surface (see DESIGN.md).
    readonly property color colCardSurface: appTheme.cardSurfaceColor
    readonly property color colCardBorder: appTheme.cardBorderColor
    // Phase 4D: opaque disabled surface (replaces opacity: 0.55 on panel shells).
    readonly property color colDisabledSurface: appTheme.disabledSurfaceColor
    readonly property color colScopeGrid: appTheme.scopeGridColor
    readonly property color colScopePlotBorder: appTheme.scopePlotBorderColor
    readonly property color colScopeHistogramRed: appTheme.scopeHistogramRedColor
    readonly property color colScopeHistogramGreen: appTheme.scopeHistogramGreenColor
    readonly property color colScopeHistogramBlue: appTheme.scopeHistogramBlueColor
    readonly property string dataFontFamily: appTheme.dataFontFamily
    readonly property string uiFontFamily: appTheme.uiFontFamily
    readonly property string headlineFontFamily: appTheme.headlineFontFamily
    readonly property int controlRadius: appTheme.controlRadius
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

    function toggleCollectionsSidebar() {
        root.collectionsSidebarExpanded = !root.collectionsSidebarExpanded
    }

    onActiveWorkspaceChanged: {
        if (root.collectionsWorkspaceObservationReady) {
            root.collectionsSidebarExpanded = false
        }
    }

    Behavior on collectionsSidebarGap {
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0
                      : (root.collectionsSidebarExpanded
                         ? appTheme.motionFoldOpenMs
                         : appTheme.motionFoldCloseMs)
            easing.type: Easing.OutCubic
        }
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
    // Transparent on the DWM / frameless path so the platform does not stroke a
    // second frame around the QML-rounded canvas. Native macOS windows clip
    // themselves, so fill the canvas color out to those system corners.
    color: root.nativeTrafficLightsEnabled ? root.colBgCanvas : "transparent"

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
    property alias pendingDeleteTargets: imageActionsController.pendingDeleteTargets
    property alias pendingRatingTarget: imageActionsController.pendingRatingTarget
    property alias pendingAdjustmentSource: imageActionsController.pendingAdjustmentSource
    property alias pendingAdjustmentPasteTargets: imageActionsController.pendingAdjustmentPasteTargets
    property alias focusedImageTarget: imageActionsController.focusedImageTarget
    property alias focusedImageInspection: imageActionsController.focusedImageInspection

    ImageActionsController {
        id: imageActionsController
        objectName: "imageActionsController"
        host: root
    }
    property alias projectLaunchPending: projectLaunchController.projectLaunchPending
    property alias welcomeDismissedForLaunch: projectLaunchController.welcomeDismissedForLaunch
    readonly property alias projectLoadingOverlayVisible: projectLaunchController.projectLoadingOverlayVisible
    readonly property alias projectLaunchBusy: projectLaunchController.projectLaunchBusy

    ProjectLaunchController {
        id: projectLaunchController
        host: root
        automationMode: root.automationModeEnabled
    }

    // Shared workspace scroll state survives Loader teardown when routing between
    // Library and Editor. The aliases keep the existing diagnostic/property names
    // while exposing one global vertical and horizontal position.
    property real globalContentY: 0
    property real globalContentX: 0
    property alias contentY: root.globalContentY
    property alias contentX: root.globalContentX

    // Application close gate: when the editor has an open image, caption/X and
    // OS close are intercepted so the user can Save (Finalize true) or Discard
    // (Finalize false) before the process exits. Async save must finish first —
    // quitting during Saving aborts the checkpoint.
    property bool allowApplicationClose: false
    property bool waitingEditorCloseSave: false
    property bool updateInstallPending: false
    property string editorCloseSessionState: appModules.editorSession
                                             ? appModules.editorSession.sessionState : ""

    Connections {
        target: appModules.updates

        function onApplicationCloseRequested() {
            root.updateInstallPending = true
            root.close()
        }
    }

    function editorCloseNeedsConfirm() {
        if (root.automationModeEnabled || root.allowApplicationClose) {
            return false
        }
        if (root.waitingEditorCloseSave) {
            return true
        }
        const router = appModules.workspaceRouter
        if (!router || String(router.workspace || "") !== "editor") {
            return false
        }
        const session = appModules.editorSession
        return !!(session && session.hasImage === true)
    }

    function cancelEditorCloseConfirm() {
        root.waitingEditorCloseSave = false
        if (root.updateInstallPending) {
            root.updateInstallPending = false
            appModules.updates.CancelInstall()
        }
        if (appDialogs.editorCloseConfirmDialog) {
            appDialogs.editorCloseConfirmDialog.busy = false
        }
    }

    function finishApplicationClose() {
        root.waitingEditorCloseSave = false
        if (root.updateInstallPending) {
            root.updateInstallPending = false
            if (!appModules.updates.CommitInstall()) {
                root.abortEditorCloseSave(appModules.updates.errorText)
                return
            }
        }
        root.allowApplicationClose = true
        if (appDialogs.editorCloseConfirmDialog
                && appDialogs.editorCloseConfirmDialog.opened) {
            appDialogs.editorCloseConfirmDialog.close()
        }
        root.close()
    }

    function abortEditorCloseSave(messageText) {
        root.waitingEditorCloseSave = false
        if (root.updateInstallPending) {
            root.updateInstallPending = false
            appModules.updates.CancelInstall()
        }
        if (appDialogs.editorCloseConfirmDialog) {
            appDialogs.editorCloseConfirmDialog.busy = false
            if (appDialogs.editorCloseConfirmDialog.opened) {
                appDialogs.editorCloseConfirmDialog.close()
            }
        }
        const detail = String(messageText || "")
        root.showSnackbar(detail.length > 0
                          ? detail
                          : qsTr("Could not save edits. Resolve the save error, then quit again."))
    }

    function beginEditorCloseDiscard() {
        const session = appModules.editorSession
        if (session) {
            if (session.hasPendingRecovery === true
                    && session.actions
                    && session.actions.canDiscardAndContinue === true) {
                session.DiscardAndContinue()
            } else {
                session.Finalize(false)
            }
        }
        root.finishApplicationClose()
    }

    function beginEditorCloseSave() {
        // Application exit explicitly seals the editor session. Ordinary
        // workspace routing keeps it alive for immediate re-entry.
        root.waitingEditorCloseSave = true
        if (appDialogs.editorCloseConfirmDialog) {
            appDialogs.editorCloseConfirmDialog.busy = true
        }
        if (appModules.editorSession) {
            appModules.editorSession.Finalize(true)
        }
        if (appModules.workspaceRouter) {
            appModules.workspaceRouter.openLibrary()
        }
        Qt.callLater(root.pollEditorCloseSave)
    }

    function pollEditorCloseSave() {
        if (!root.waitingEditorCloseSave) {
            return
        }
        const session = appModules.editorSession
        if (!session) {
            root.finishApplicationClose()
            return
        }
        const state = String(session.sessionState || "")
        // Same in-flight seal gate as EditorFilmstrip.
        if (state === "Saving" || state === "Switching") {
            return
        }
        if (session.hasPendingRecovery === true
                || state === "RetainedImageFailure"
                || state === "Failed") {
            root.abortEditorCloseSave(session.lastError)
            return
        }
        // Close completed (or sync no-op). Do not quit while still Interactive
        // after a rejected seal — that would drop unsaved work.
        if (state === "NoImage" || state === "ShuttingDown") {
            root.finishApplicationClose()
            return
        }
        root.abortEditorCloseSave(session.lastError)
    }

    onEditorCloseSessionStateChanged: {
        if (root.waitingEditorCloseSave) {
            root.pollEditorCloseSave()
        }
    }

    onClosing: function(close) {
        if (!root.editorCloseNeedsConfirm()) {
            if (root.updateInstallPending) {
                root.updateInstallPending = false
                if (!appModules.updates.CommitInstall()) {
                    close.accepted = false
                    root.showSnackbar(appModules.updates.errorText)
                }
            }
            return
        }
        close.accepted = false
        if (root.waitingEditorCloseSave) {
            return
        }
        if (appDialogs.editorCloseConfirmDialog
                && appDialogs.editorCloseConfirmDialog.opened) {
            return
        }
        appDialogs.openEditorCloseConfirmDialog()
    }

    // Library workspace UI state survives Loader teardown when routing to the editor.
    // LibraryWorkspace reads these on create and writes them back on destroy.
    property bool libraryInspectorVisible: true
    property real libraryInspectorWidth: 300
    property int libraryGridZoomLevel: 4
    property alias libraryGridContentY: root.globalContentY
    property alias editorFilmstripContentX: root.globalContentX

    // Cross-workspace reveal requests are consumed by the next live view. The
    // monotonically increasing IDs also handle selecting the same image twice.
    property int libraryScrollTargetElementId: 0
    property int libraryScrollTargetIndex: -1
    property int libraryScrollRequestId: 0
    property int filmstripScrollTargetElementId: 0
    property int filmstripScrollTargetIndex: -1
    property int filmstripScrollRequestId: 0


    Component.onCompleted: {
        appDialogs.blurSource = mainContent
        imageActionsController.selectionState = selectionStateObj
        imageActionsController.exportQueueState = exportQueueStateObj
        imageActionsController.imageContextMenu = appDialogs.imageContextMenu
        imageActionsController.adjustmentTransferDialog = appDialogs.adjustmentTransferDialog
        imageActionsController.deleteConfirmDialog = appDialogs.deleteConfirmDialog
        projectLaunchController.welcomeDialog = appDialogs.welcomeDialog
        projectLaunchController.start()
        root.collectionsWorkspaceObservationReady = true
    }


    function showSnackbar(messageText) {
        notificationSnackbar.show(messageText)
    }

    function requestSaveProject() {
        projectLaunchController.requestSaveProject()
    }

    function languageIndexForCode(code) {
        return projectLaunchController.languageIndexForCode(code)
    }

    function openSettingsDialog(category) {
        appDialogs.openSettingsDialog(category)
    }

    function openAdvancedAnalysisDialog() {
        appDialogs.openAdvancedAnalysisDialog()
    }

    function openBackgroundTasksDialog() {
        appDialogs.openBackgroundTasksDialog()
    }

    function openUpdateSettings() {
        appDialogs.openSettingsDialog(6)
    }


    function beginProjectLaunch(loadAction) {
        projectLaunchController.beginProjectLaunch(loadAction)
    }

    function revealLibraryAfterProjectLoad() {
        const library = workspaceHost.libraryItem
        if (library && library.playLibraryGridReveal) {
            library.playLibraryGridReveal()
        }
    }

    function startPendingProjectLaunch() {
        projectLaunchController.startPendingProjectLaunch()
    }

    function updateWelcomeDialogVisibility() {
        projectLaunchController.updateWelcomeDialogVisibility()
    }

    function setFocusedImage(item) {
        imageActionsController.setFocusedImage(item)
    }

    function selectAllCurrentAlbum() {
        imageActionsController.selectAllCurrentAlbum()
    }

    function requestLibraryScrollToElement(elementId, index) {
        const target = Number(elementId || 0)
        if (target <= 0) {
            return
        }
        libraryScrollTargetElementId = target
        libraryScrollTargetIndex = index === undefined ? -1 : Number(index)
        libraryScrollRequestId += 1
    }

    function clearLibraryScrollTarget(elementId) {
        if (Number(elementId || 0) !== Number(libraryScrollTargetElementId)) {
            return
        }
        libraryScrollTargetElementId = 0
        libraryScrollTargetIndex = -1
    }

    function requestFilmstripScrollToElement(elementId, index) {
        const target = Number(elementId || 0)
        if (target <= 0) {
            return
        }
        filmstripScrollTargetElementId = target
        filmstripScrollTargetIndex = index === undefined ? -1 : Number(index)
        filmstripScrollRequestId += 1
    }

    function clearFilmstripScrollTarget(elementId) {
        if (Number(elementId || 0) !== Number(filmstripScrollTargetElementId)) {
            return
        }
        filmstripScrollTargetElementId = 0
        filmstripScrollTargetIndex = -1
    }

    function openImageContextMenu(clickedItem, sceneX, sceneY) {
        imageActionsController.openImageContextMenu(clickedItem, sceneX, sceneY)
    }

    function openEditorFilmstripContextMenu(clickedItem, sceneX, sceneY) {
        imageActionsController.openImageContextMenu(clickedItem, sceneX, sceneY,
                                                    "editor-filmstrip")
    }

    function requestSetFocusedImageRating(rating) {
        imageActionsController.requestSetFocusedImageRating(rating)
    }

    function requestSaveFocusedDescription(caption) {
        imageActionsController.requestSaveFocusedDescription(caption)
    }

    function requestSaveFocusedRatingReason(reasons) {
        imageActionsController.requestSaveFocusedRatingReason(reasons)
    }

    function editorImageStillExists(elementId) {
        return imageActionsController.editorImageStillExists(elementId)
    }

    readonly property alias workspaceLayer: workspaceHost
    readonly property alias exportQueueState: exportQueueStateObj
    readonly property alias selectionState: selectionStateObj
    readonly property var importDialog: appDialogs.importDialog
    readonly property var importFolderDialog: appDialogs.importFolderDialog
    readonly property var globalSearchDialog: appDialogs.globalSearchDialog

    ExportQueueState {
        id: exportQueueStateObj
    }

    SelectionState {
        id: selectionStateObj
    }

    AppDialogs {
        id: appDialogs
        host: root
        automationMode: root.automationModeEnabled
        imageActionsController: imageActionsController
        selectionState: selectionStateObj
        exportQueueState: exportQueueStateObj
    }


    NotificationSnackbar {
        id: notificationSnackbar
        theme: root
        host: root
    }

    // Shared blur target for modal dialogs (AppDialogs + editor-local overlays).
    readonly property alias dialogBlurSource: mainContent

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
        anchors.margins: appTheme.spaceMd
        spacing: appTheme.spaceSm

        TopToolbar {
            id: topToolbar
            theme: root
            host: root
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            CollectionsPanel {
                id: collectionsSidebar
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: root.collectionsSidebarExpanded ? appTheme.collectionsSidebarWidth : 0
                opacity: root.collectionsSidebarExpanded ? 1.0 : 0.0
                enabled: root.collectionsSidebarExpanded
                folderController: appModules.folders
                theme: root
                host: root
                backendInteractive: root.backendInteractive
                selectedCount: root.selectedCount
                onImportRequested: root.importDialog.open()
                onImportFromFolderRequested: root.importFolderDialog.open()
                onSearchRequested: root.globalSearchDialog.openFromCollection()
                onAdvancedAnalysisRequested: root.openAdvancedAnalysisDialog()
                onBackgroundTasksRequested: root.openBackgroundTasksDialog()

                Behavior on width {
                    NumberAnimation {
                        duration: appTheme.reduceMotion ? 0
                                  : (root.collectionsSidebarExpanded
                                     ? appTheme.motionFoldOpenMs
                                     : appTheme.motionFoldCloseMs)
                        easing.type: Easing.OutCubic
                    }
                }
                Behavior on opacity {
                    NumberAnimation {
                        duration: appTheme.reduceMotion ? 0 : appTheme.motionFadeMs
                        easing.type: Easing.OutCubic
                    }
                }
            }

            ColumnLayout {
                anchors.left: collectionsSidebar.right
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.leftMargin: root.collectionsSidebarGap
                spacing: appTheme.spaceMd

                WorkspaceHost {
                    id: workspaceHost
                    objectName: "workspaceHost"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    theme: root
                    host: root
                    workspaceRouter: appModules.workspaceRouter
                }

                BackgroundTaskBar {
                    Layout.fillWidth: true
                }
            }
        }
    }

    }

    WindowAnimations {
        id: windowAnimations
        host: root
    }

    ShellSignals {
        id: shellSignals
        host: root
        imageActionsController: imageActionsController
        selectionState: selectionStateObj
        exportQueueState: exportQueueStateObj
        deleteConfirmDialog: appDialogs.deleteConfirmDialog
    }

    function toggleMaximizeAnimated() {
        windowAnimations.toggleMaximize()
    }

    function minimizeWindow() {
        windowAnimations.minimize()
    }

    WindowResizeHandles {
        host: root
        active: root.visibility === Window.Windowed && !root.nativeTrafficLightsEnabled
    }
    Shortcut {
        sequence: StandardKey.SelectAll
        enabled: root.backendInteractive && !appDialogs.anyDialogOpened()
        onActivated: root.selectAllCurrentAlbum()
    }

    // ── Accelerator preparation overlay ───────────────────────────────
    AcceleratorPreparationOverlay {
        visible: appModules.project.acceleratorPreparing
        z: 70
        theme: root
        blurSource: mainContent
    }

    // ── Project loading overlay ────────────────────────────────────────
    ProjectLoadingOverlay {
        wanted: root.projectLoadingOverlayVisible
        z: 45
        theme: root
        host: root
        blurSource: mainContent
    }

    // ── Import progress overlay ──────────────────────────────────────────
    ImportProgressOverlay {
        visible: appModules.importExport.importRunning && !appModules.nikonHeRecovery.nikonHeRecoveryActive
        z: 50
        theme: root
        blurSource: mainContent
    }

}
