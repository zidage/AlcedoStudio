import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Effects

ApplicationWindow {
    id: root
    objectName: "mainWindow"
    width: 1200
    height: 760
    minimumWidth: 960
    minimumHeight: 640
    visible: true
    visibility: Window.Windowed
    title: qsTr("Alcedo Studio")
    flags: Qt.Window | Qt.FramelessWindowHint
    font.family: appTheme.uiFontFamily

    readonly property bool windowMaximized: visibility === Window.Maximized || visibility === Window.FullScreen
    readonly property bool windowRestoring: root.visibility !== Window.Minimized && root.visibility !== Window.Hidden
    readonly property real maximizedInset: 0
    // Snap radius — animating it together with the OS resize causes layout jitter.
    readonly property real windowCornerRadius: windowMaximized ? 0 : 12

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
    // Transparent root surface so DWM does not draw a frame/border around our rounded content.
    color: "transparent"

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
        host: root
    }
    property alias projectLaunchPending: projectLaunchController.projectLaunchPending
    property alias welcomeDismissedForLaunch: projectLaunchController.welcomeDismissedForLaunch
    readonly property alias projectLoadingOverlayVisible: projectLaunchController.projectLoadingOverlayVisible
    readonly property alias projectLaunchBusy: projectLaunchController.projectLaunchBusy

    ProjectLaunchController {
        id: projectLaunchController
        host: root
    }

    // Library workspace UI state survives Loader teardown when routing to the editor.
    // LibraryWorkspace reads these on create and writes them back on destroy.
    property bool libraryInspectorVisible: true
    property real libraryInspectorWidth: 300
    property int libraryGridZoomLevel: 4
    property real libraryGridContentY: 0


    Component.onCompleted: {
        appDialogs.blurSource = mainContent
        imageActionsController.selectionState = selectionStateObj
        imageActionsController.exportQueueState = exportQueueStateObj
        imageActionsController.imageContextMenu = appDialogs.imageContextMenu
        imageActionsController.adjustmentTransferDialog = appDialogs.adjustmentTransferDialog
        imageActionsController.deleteConfirmDialog = appDialogs.deleteConfirmDialog
        projectLaunchController.welcomeDialog = appDialogs.welcomeDialog
        projectLaunchController.start()
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


    function beginProjectLaunch(loadAction) {
        projectLaunchController.beginProjectLaunch(loadAction)
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

    function openImageContextMenu(clickedItem, sceneX, sceneY) {
        imageActionsController.openImageContextMenu(clickedItem, sceneX, sceneY)
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

    readonly property alias exportQueueState: exportQueueStateObj
    readonly property alias selectionState: selectionStateObj
    readonly property var importDialog: appDialogs.importDialog
    readonly property var exportDialog: appDialogs.exportDialog
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
        imageActionsController: imageActionsController
        selectionState: selectionStateObj
        exportQueueState: exportQueueStateObj
    }


    NotificationSnackbar {
        id: notificationSnackbar
        theme: root
        host: root
    }

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
        anchors.margins: 12
        spacing: 12

        TopToolbar {
            id: topToolbar
            theme: root
            host: root
        }

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
            onTaskDetailsRequested: function(task) {
                if (task && task.kind === "imageAnalysis") {
                    appDialogs.advancedContentAnalysisDialog.openTaskDetails(task)
                }
            }
        }
    }

    }

    WindowAnimations {
        id: windowAnimations
        host: root
        contentTarget: mainContent
    }

    ShellSignals {
        id: shellSignals
        host: root
        imageActionsController: imageActionsController
        selectionState: selectionStateObj
        exportQueueState: exportQueueStateObj
        deleteConfirmDialog: appDialogs.deleteConfirmDialog
        exportDialog: appDialogs.exportDialog
        windowAnimations: windowAnimations
    }

    function toggleMaximizeAnimated() {
        windowAnimations.toggleMaximize()
    }

    function minimizeWindow() {
        windowAnimations.minimize()
    }

    WindowResizeHandles {
        host: root
        active: root.visibility === Window.Windowed
    }
    Shortcut {
        sequence: StandardKey.SelectAll
        enabled: root.backendInteractive && !appDialogs.anyDialogOpened()
        onActivated: imageActionsController.selectAllCurrentAlbum()
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
        visible: root.projectLoadingOverlayVisible
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
