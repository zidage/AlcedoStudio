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
    property bool collectionsSidebarUserAdjusted: false
    property bool collectionsWorkspaceObservationReady: false
    // How far the collections sidebar is open: 0 folded, 1 open. One animation drives the slide
    // and the fade of the sidebar and the left edge of the workspace (issue 751). Only
    // setCollectionsSidebarExpanded writes it; it starts open, as collectionsSidebarExpanded.
    property real collectionsSidebarReveal: 1.0
    // The width of the open sidebar and its gap to the workspace.
    readonly property real collectionsSidebarOpenExtent: appTheme.collectionsSidebarWidth
                                                         + appTheme.spaceMd
    // The left edge of the workspace: the shown part of the sidebar and its gap.
    readonly property real collectionsSidebarExtent: collectionsSidebarReveal
                                                     * collectionsSidebarOpenExtent
    // True while a manual toggle folds or opens the sidebar. The shown workspace follows the
    // sidebar edge on every frame and defers the work that only its final size needs. It turns
    // false after the shown workspace has its final size (collectionsSidebarFoldAnimation).
    property bool collectionsSidebarFolding: false
    property bool editorAdjustmentStackExpanded: true
    // Luminance/Hue Range section of the Mask panel (#605): collapsed at the start of the session;
    // the user's choice survives the editor Loader.
    property bool editorMaskRangeSectionExpanded: false
    readonly property string activeWorkspace: appModules.workspaceRouter
                                              ? String(appModules.workspaceRouter.workspace
                                                       || "library")
                                              : "library"
    readonly property bool activeRightSidebarExpanded: activeWorkspace === "editor"
                                                       ? editorAdjustmentStackExpanded
                                                       : libraryInspectorVisible
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
    readonly property color colScopeMarker: appTheme.scopeMarkerColor
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
        root.collectionsSidebarUserAdjusted = true
        root.setCollectionsSidebarExpanded(!root.collectionsSidebarExpanded, true)
    }

    // Opens or folds the collections sidebar. @p animated slides it over the fold duration;
    // otherwise the sidebar and the workspace edge take the new state at once, and a running
    // fold stops at its end.
    function setCollectionsSidebarExpanded(expanded, animated) {
        root.collectionsSidebarExpanded = expanded
        const target = expanded ? 1.0 : 0.0
        if (!animated || appTheme.reduceMotion) {
            collectionsSidebarFoldAnimation.stop()
            root.collectionsSidebarReveal = target
            return
        }
        if (collectionsSidebarFoldAnimation.running
                && collectionsSidebarFoldAnimation.to === target) {
            return
        }
        collectionsSidebarFoldAnimation.stop()
        if (root.collectionsSidebarReveal === target) {
            return
        }
        collectionsSidebarFoldAnimation.from = root.collectionsSidebarReveal
        collectionsSidebarFoldAnimation.to = target
        collectionsSidebarFoldAnimation.duration = expanded ? appTheme.motionFoldOpenMs
                                                            : appTheme.motionFoldCloseMs
        collectionsSidebarFoldAnimation.start()
    }

    // A workspace switch replaces the whole content: the sidebar state of the new workspace
    // applies at once, and the workspace fades in at its final size (WorkspaceHost). The user's
    // own choice stays, and a fold in progress ends at once.
    function applyDefaultCollectionsSidebarForWorkspace(workspace) {
        const expanded = root.collectionsSidebarUserAdjusted
                ? root.collectionsSidebarExpanded
                : workspace === "library"
        root.setCollectionsSidebarExpanded(expanded, false)
    }

    function toggleActiveRightSidebar() {
        if (root.activeWorkspace === "editor") {
            root.editorAdjustmentStackExpanded = !root.editorAdjustmentStackExpanded
            return
        }
        root.libraryInspectorVisible = !root.libraryInspectorVisible
    }

    NumberAnimation {
        id: collectionsSidebarFoldAnimation
        target: root
        property: "collectionsSidebarReveal"
        easing.type: appTheme.motionEasing

        // The last reveal value can reach the nested layouts of the shown workspace in their
        // next polish, after the animation stops (issue 754). The host applies those layouts
        // first, so the workspace has its final size when the flag drops.
        onRunningChanged: {
            if (running) {
                root.collectionsSidebarFolding = true
                return
            }
            workspaceHost.settleShownWorkspaceLayouts()
            root.collectionsSidebarFolding = false
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

    // Button hover and press change only the fill colour; the button keeps its size.
    // Dark secondary action (Add to Export Queue, Import Images): hover lifts the fill,
    // press sinks it below the idle fill.
    function secondaryButtonFill(enabled, hovered, pressed) {
        if (!enabled) {
            return withAlpha(colButtonSecondary, 0.55)
        }
        if (pressed) {
            return Qt.darker(colButtonSecondary, 1.2)
        }
        if (hovered) {
            return Qt.lighter(colButtonSecondary, 1.34)
        }
        return colButtonSecondary
    }

    // Neutral filled action (Load Project, Export, Import From Folder): the light list well
    // with its ink. Hover darkens the well, press darkens it more. A disabled action drops to
    // the sunken base fill with muted ink.
    function filledActionFill(enabled, hovered, pressed) {
        if (!enabled) {
            return appTheme.bgBaseColor
        }
        if (pressed) {
            return Qt.darker(appTheme.editorListSelectedFillColor, 1.24)
        }
        if (hovered) {
            return Qt.darker(appTheme.editorListSelectedFillColor, 1.14)
        }
        return appTheme.editorListSelectedFillColor
    }

    function filledActionInk(enabled) {
        return enabled ? appTheme.editorListSelectedInkColor : appTheme.textMutedColor
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
    readonly property var exportQueueById: appModules.exportQueue.entriesById
    readonly property var semanticGeneration: appModules.semanticGeneration
    readonly property int selectedCount: selectionState.selectedCount
    readonly property int exportQueueCount: appModules.exportQueue.count
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

    // Application close check: when the editor has an open image, caption/X and
    // OS close are intercepted so the user can Save (Finalize true) or Discard
    // (Finalize false) before the process exits. Async save must finish first —
    // quitting during Saving aborts the checkpoint. The close decision and the
    // editor finalize sequence live in C++ (appModules.applicationClose,
    // ApplicationCloseCoordinator); this window keeps the dialog and the update
    // install hand-off.
    property bool allowApplicationClose: false
    property bool updateInstallPending: false

    // A scroll anchor names occurrences of one project's result.
    Connections {
        target: appModules.project
        ignoreUnknownSignals: true

        function onProjectChanged() {
            root.librarySectionScrollAnchor = null
        }
    }

    Connections {
        target: appModules.updates

        function onApplicationCloseRequested() {
            root.updateInstallPending = true
            root.close()
        }
    }

    Connections {
        target: appModules.applicationClose

        function onApplicationCloseFinished() {
            root.finishApplicationClose()
        }

        function onApplicationCloseAborted(message) {
            root.abortEditorCloseSave(message)
        }
    }

    function editorCloseNeedsConfirm() {
        if (root.automationModeEnabled || root.allowApplicationClose) {
            return false
        }
        return appModules.applicationClose.CloseNeedsConfirmation()
    }

    function cancelEditorCloseConfirm() {
        appModules.applicationClose.Cancel()
        if (root.updateInstallPending) {
            root.updateInstallPending = false
            appModules.updates.CancelInstall()
        }
        if (appDialogs.editorCloseConfirmDialog) {
            appDialogs.editorCloseConfirmDialog.busy = false
        }
    }

    function finishApplicationClose() {
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
        if (appDialogs.editorCloseConfirmDialog) {
            appDialogs.editorCloseConfirmDialog.busy = true
        }
        appModules.applicationClose.BeginDiscard()
    }

    function beginEditorCloseSave() {
        if (appDialogs.editorCloseConfirmDialog) {
            appDialogs.editorCloseConfirmDialog.busy = true
        }
        appModules.applicationClose.BeginSave()
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
        if (appModules.applicationClose.waiting) {
            return
        }
        if (appDialogs.editorCloseConfirmDialog
                && appDialogs.editorCloseConfirmDialog.opened) {
            return
        }
        const router = appModules.workspaceRouter
        const inEditor = !!(router && String(router.workspace || "") === "editor")
        if (!inEditor && appModules.applicationClose.EditorPersistBusy()) {
            appModules.applicationClose.BeginPersistWait()
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
    // Scroll anchor of the grouped library view (AlbumSectionView.scrollAnchor), written when
    // that view is torn down (editor round trip, flat mode) and read once by the next one.
    property var librarySectionScrollAnchor: null
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
        imageActionsController.imageContextMenu = appDialogs.imageContextMenu
        imageActionsController.adjustmentTransferDialog = appDialogs.adjustmentTransferDialog
        imageActionsController.deleteConfirmDialog = appDialogs.deleteConfirmDialog
        projectLaunchController.welcomeDialog = appDialogs.welcomeDialog
        projectLaunchController.start()
        root.collectionsWorkspaceObservationReady = true
        Qt.callLater(savedAiKeysReminder.showIfSavedKeys)
    }

    SavedAiKeysReminder {
        id: savedAiKeysReminder
        host: root
        profileController: appModules.aiProviderProfiles
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

    function openBackgroundTasksDialog() {
        appDialogs.openBackgroundTasksDialog()
    }

    // ── Library file drop (issue #757) ──────────────────────────────────────
    // A drop of files and folders from the system file manager imports into one collection
    // with the rules of the Import buttons. The grid drops into the current collection, a
    // Collections row into its own collection.

    // True when the Library may take a file drop now: a project is open, no import runs, no
    // dialog is open, and no agent holds control.
    readonly property bool fileDropImportAllowed: root.backendInteractive
        && !appModules.importExport.importRunning
        && appModules.interactionPolicy.canChangeStateAsUser
        && !appDialogs.anyDialogOpened()

    // Collection name for the drop overlay and the folder dialog title.
    function collectionDisplayName(folderId) {
        if (Number(folderId) === 0) {
            return qsTr("All Photos")
        }
        const rows = appModules.folders ? appModules.folders.folders : []
        for (let i = 0; i < rows.length; ++i) {
            if (Number(rows[i].folderId) === Number(folderId)) {
                return String(rows[i].name)
            }
        }
        return ""
    }

    function importDroppedUrls(urls, folderId) {
        appDialogs.importDroppedUrls(urls, Number(folderId), root.collectionDisplayName(folderId))
    }

    function openLutImportDialog() {
        appDialogs.openLutImportDialog()
    }

    function openUpdateSettings() {
        appDialogs.openUpdateSettings()
    }

    function openLutSettings() {
        appDialogs.openLutSettings()
    }

    function openAiModelSettings() {
        appDialogs.openAiModelSettings()
    }

    function openColorLabelSettings() {
        appDialogs.openColorLabelSettings()
    }

    function refreshFocusedImageInspection() {
        imageActionsController.refreshFocusedImageInspection()
    }


    function revealLibraryAfterProjectLoad() {
        const library = workspaceHost.libraryItem
        if (library && library.playLibraryGridReveal) {
            library.playLibraryGridReveal()
        }
    }

    function updateWelcomeDialogVisibility() {
        projectLaunchController.updateWelcomeDialogVisibility()
    }

    function requestStartupPreview() {
        projectLaunchController.requestStartupPreview()
    }

    function continueWelcomeProject() {
        return projectLaunchController.continueWelcomeProject()
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

    function requestSetCardImageRating(item, rating) {
        imageActionsController.requestSetCardImageRating(item, rating)
    }

    function requestSaveFocusedDescription(caption) {
        imageActionsController.requestSaveFocusedDescription(caption)
    }

    function requestSaveFocusedRatingReason(reasons) {
        imageActionsController.requestSaveFocusedRatingReason(reasons)
    }

    function openLastEditorImage(elementId, imageId) {
        imageActionsController.openLastEditorImage(elementId, imageId)
    }

    function firstLibraryImage() {
        return imageActionsController.firstLibraryImage()
    }

    readonly property alias workspaceLayer: workspaceHost
    // Thumbnail tier of the Library grid; the welcome cover pins share it.
    readonly property int libraryThumbnailMaxEdge: workspaceHost.libraryItem
                                                   && workspaceHost.libraryItem.thumbnailMaxEdge !== undefined
                                                   ? workspaceHost.libraryItem.thumbnailMaxEdge : 0
    readonly property alias selectionState: selectionStateObj
    readonly property var importDialog: appDialogs.importDialog
    readonly property var importFolderDialog: appDialogs.importFolderDialog
    readonly property var globalSearchDialog: appDialogs.globalSearchDialog

    SelectionState {
        id: selectionStateObj
    }

    AppDialogs {
        id: appDialogs
        host: root
        automationMode: root.automationModeEnabled
        imageActionsController: imageActionsController
        selectionState: selectionStateObj
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

            // The shown part of the sidebar and its gap. The panel keeps its full width and
            // slides inside this clip, so its folder list and text never reflow during a fold.
            Item {
                id: collectionsSidebarSlot
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: root.collectionsSidebarExtent
                clip: true
                visible: root.collectionsSidebarReveal > 0

                CollectionsPanel {
                    id: collectionsSidebar
                    objectName: "collectionsSidebar"
                    x: collectionsSidebarSlot.width - root.collectionsSidebarOpenExtent
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    opacity: root.collectionsSidebarReveal
                    enabled: root.collectionsSidebarExpanded
                    folderController: appModules.folders
                    theme: root
                    host: root
                    backendInteractive: root.backendInteractive
                    fileDropEnabled: root.fileDropImportAllowed
                    onImportRequested: root.importDialog.open()
                    onImportFromFolderRequested: root.importFolderDialog.open()
                    onFilesDropped: function(urls, folderId) {
                        root.importDroppedUrls(urls, folderId)
                    }
                    onSearchRequested: root.globalSearchDialog.openFromCollection()
                    onBackgroundTasksRequested: root.openBackgroundTasksDialog()
                }
            }

            // Anchored, not in a layout: the host width follows the sidebar edge in the same
            // event, so a workspace switch sizes the shown workspace once (WorkspaceHost).
            WorkspaceHost {
                id: workspaceHost
                objectName: "workspaceHost"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.leftMargin: root.collectionsSidebarExtent
                theme: root
                host: root
                workspaceRouter: appModules.workspaceRouter
                geometryAnimating: root.collectionsSidebarFolding
                onWorkspaceShowing: function(workspace) {
                    if (root.collectionsWorkspaceObservationReady) {
                        root.applyDefaultCollectionsSidebarForWorkspace(workspace)
                    }
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

    // Workspace-scoped commands: the registry scope string tracks the router's
    // active workspace, so Ctrl+A / Ctrl+S resolve to exactly one owner.
    readonly property string activeShortcutScope: "workspace." + root.activeWorkspace

    RegisteredShortcut {
        objectName: "librarySelectAllShortcut"
        commandId: "library.selectAll"
        activeScope: root.activeShortcutScope
        commandEnabled: root.backendInteractive && !appDialogs.anyDialogOpened()
        onActivated: root.selectAllCurrentAlbum()
    }

    RegisteredShortcut {
        objectName: "librarySaveProjectShortcut"
        commandId: "library.saveProject"
        activeScope: root.activeShortcutScope
        commandEnabled: root.backendInteractive && !appDialogs.anyDialogOpened()
        suppressWhileEditing: false
        onActivated: root.requestSaveProject()
    }

    RegisteredShortcut {
        objectName: "editorUndoShortcut"
        commandId: "editor.undo"
        activeScope: root.activeShortcutScope
        commandEnabled: !appDialogs.anyDialogOpened()
                        && appModules.editorSession
                        && appModules.editorSession.actions
                        && appModules.editorSession.actions.canUndo === true
        onActivated: appModules.editorSession.Undo()
    }

    RegisteredShortcut {
        objectName: "editorRedoShortcut"
        commandId: "editor.redo"
        activeScope: root.activeShortcutScope
        commandEnabled: !appDialogs.anyDialogOpened()
                        && appModules.editorSession
                        && appModules.editorSession.actions
                        && appModules.editorSession.actions.canRedo === true
        onActivated: appModules.editorSession.Redo()
    }

    RegisteredShortcut {
        objectName: "editorSaveImageShortcut"
        commandId: "editor.saveCurrentImage"
        activeScope: root.activeShortcutScope
        commandEnabled: !appDialogs.anyDialogOpened()
                        && appModules.editorSession
                        && appModules.editorSession.hasImage === true
                        && appModules.editorSession.persistInFlight !== true
                        && appModules.editorSession.closeInFlight !== true
        suppressWhileEditing: false
        onActivated: appModules.editorSession.PersistCurrentImage()
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
