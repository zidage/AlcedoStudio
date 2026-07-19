import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Alcedo.Main 1.0

// Unified editor workspace shell. The viewport is a QQuickRhiItem-backed
// presentation surface; the surrounding shell remains QML-owned.
Item {
    id: root
    objectName: "editorWorkspace"

    property var theme: null
    property var workspaceRouter: appModules.workspaceRouter
    property var editorSession: appModules.editorSession

    readonly property color colPanel: theme ? theme.colGlassPanel : "#1C1C1D"
    readonly property color colStroke: theme ? theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : "#457B9D"
    readonly property color colBg: theme ? theme.colBgCanvas : "#111214"
    readonly property color colDeep: theme ? theme.colBgDeep : "#0C0D0F"
    readonly property color colHover: theme ? theme.colHover : Qt.rgba(1, 1, 1, 0.07)
    // Card surface family — shared with the Library grid (see DESIGN.md). The
    // viewport placeholder resolves here so it matches the editor card family.
    readonly property color colCardSurface: theme ? theme.colCardSurface : "#161719"
    readonly property color colCardBorder: theme ? theme.colCardBorder : Qt.rgba(1, 1, 1, 0.08)
    readonly property int panelRadius: theme ? theme.panelRadius : 12
    readonly property int controlRadius: theme ? theme.controlRadius : 10
    readonly property string headlineFont: theme ? theme.headlineFontFamily : appTheme.headlineFontFamily
    readonly property bool hasImage: editorSession ? editorSession.hasImage : false
    readonly property int focusedElementId: workspaceRouter ? Number(workspaceRouter.elementId) : 0
    readonly property int focusedImageId: workspaceRouter ? Number(workspaceRouter.imageId) : 0

    // Focus order: viewport → side panels → filmstrip handle. Returning to the
    // library is owned by the shared main-window navigation, not by an
    // editor-local control.
    Component.onCompleted: {
        if (!hasImage) {
            emptyStatePrompt.forceActiveFocus()
        } else {
            viewportSlot.forceActiveFocus()
        }
    }

    // Explicit minimum center viewport width. Side panels never swap places
    // under a narrow window; the center column holds this floor instead.
    readonly property int minimumViewportWidth: 360

    ColumnLayout {
        anchors.fill: parent
        spacing: appTheme.spaceMd

        // ── Main editor body ────────────────────────────────────────────
        // Desktop order (non-negotiable): History/Versions left, viewport
        // center, scopes + adjustment stack right.
        RowLayout {
            id: editorDesktopRow
            objectName: "editorDesktopRow"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: appTheme.spaceMd

            // Left: History / Versions rail (+ expandable panel beside rail).
            // objectName is set inside the component (editorHistoryVersionsRail).
            EditorHistoryVersionsRail {
                id: historyVersionsRail
                Layout.fillHeight: true
                theme: root.theme
                editorSession: root.editorSession
                // Rail stays usable without an image so empty-state navigation
                // can still open/collapse the panels; bodies show empty hints.
                controlsEnabled: true
            }

            // Center column: viewport + filmstrip
            ColumnLayout {
                id: centerColumn
                objectName: "editorCenterColumn"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: root.minimumViewportWidth
                spacing: appTheme.spaceMd

                Rectangle {
                    id: viewportSlot
                    objectName: "editorViewportSlot"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: root.panelRadius
                    color: root.colCardSurface
                    border.width: 1
                    border.color: root.colCardBorder
                    clip: true
                    activeFocusOnTab: true
                    focus: true
                    Accessible.role: Accessible.Canvas
                    Accessible.name: qsTr("Editor viewport")
                    Accessible.description: root.hasImage
                                            ? qsTr("Image viewport")
                                            : qsTr("Empty viewport")

                    // No-image empty state — centered localized prompt.
                    Column {
                        id: emptyStatePrompt
                        objectName: "editorEmptyState"
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 48, 420)
                        spacing: appTheme.spaceMd
                        visible: !root.hasImage
                        activeFocusOnTab: true
                        Accessible.role: Accessible.StaticText
                        Accessible.name: qsTr("Select an image to edit")

                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: qsTr("Select an image to edit")
                            font.family: root.headlineFont
                            font.pixelSize: appTheme.fontSizeHeadline
                            font.weight: appTheme.fontWeightHeading
                            color: root.colText
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Label {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("Open an image from the library, or keep this workspace ready for search results.")
                            color: root.colMuted
                            font.pixelSize: appTheme.fontSizeTitle
                        }
                    }

                    // Interaction owns zoom/pan/crop state. The RHI viewport only
                    // presents photograph layers; overlays are a separate QSG item.
                    EditorInteractionController {
                        id: editorInteraction
                        objectName: "editorInteractionController"
                        interactionEnabled: root.hasImage
                    }

                    EditorViewportItem {
                        id: editorViewportItem
                        objectName: "editorViewportItem"
                        anchors.fill: parent
                        visible: root.hasImage
                        // imageIdentity is the durable DB id; imageGeneration is the
                        // monotonic session counter from EditorSessionController so
                        // A→B→A cannot accept a late frame from the first A session.
                        imageIdentity: root.focusedImageId
                        imageGeneration: root.editorSession ? root.editorSession.sessionGeneration : 0
                        Accessible.role: Accessible.Canvas
                        Accessible.name: qsTr("Image viewport")

                        Component.onCompleted: {
                            viewportSlot.ensurePresentationBinding()
                            if (root.editorSession) {
                                root.editorSession.bindInteractionController(editorInteraction)
                            }
                            viewportSlot.syncImageGeometry()
                        }
                        Component.onDestruction: {
                            if (root.editorSession) {
                                root.editorSession.bindInteractionController(null)
                                root.editorSession.unbindPresentationViewport()
                            }
                        }

                        // Pipeline EnsureSize → render reference for crop/zoom math.
                        // Force-apply so equal output sizes still re-sync after a new
                        // image/session generation (Phase 5B).
                        // DirectFrameSink only emits this for render-reference frames
                        // (InteractivePrimary / QualityBase full frames). DetailPatch
                        // ROI sizes must not rewrite reference geometry or the high-res
                        // zoom patch fails SameRoi / viewport coverage.
                        onTargetSizeRequested: function (w, h) {
                            if (w > 0 && h > 0) {
                                editorInteraction.forceRenderReferenceSize(w, h)
                            }
                        }
                    }

                    EditorOverlayItem {
                        id: editorOverlayItem
                        objectName: "editorOverlayItem"
                        anchors.fill: parent
                        visible: root.hasImage
                        interaction: editorInteraction
                        // Overlay must sit above the photograph and receive no
                        // exclusive mouse grab — handlers below own input.
                        z: 2
                    }

                    // Spinner / status remain ordinary QML (not baked into the RHI pass).
                    Label {
                        objectName: "editorViewportStatus"
                        anchors.centerIn: parent
                        visible: root.hasImage && !editorViewportItem.presentationAvailable
                        text: qsTr("Preparing image viewport")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeSection
                        z: 3
                    }

                    // Phase 5D: render-coordinator busy indicator. Visible only
                    // after the viewport is presenting and the coordinator has
                    // in-flight/pending work (interactive drag, settled quality
                    // pass, or detail patch). Bound to EditorSessionController.
                    // renderBusy, which reflects coordinator diagnostics — never
                    // a pipeline task object (D6).
                    BusyIndicator {
                        objectName: "editorRenderBusyIndicator"
                        anchors.centerIn: parent
                        visible: root.hasImage
                                && editorViewportItem.presentationAvailable
                                && root.editorSession
                                && root.editorSession.renderBusy
                        running: visible
                        z: 3
                    }

                    // Crop rotation label (text overlay, not QSG).
                    Rectangle {
                        id: cropAngleBadge
                        objectName: "editorCropAngleBadge"
                        visible: root.hasImage
                                 && editorInteraction.cropOverlayVisible
                                 && editorInteraction.overlayGeometryValid
                        x: Math.min(Math.max(0, editorInteraction.rotateHandleItemPos.x + 10),
                                    Math.max(0, parent.width - width - 4))
                        y: Math.min(Math.max(0, editorInteraction.rotateHandleItemPos.y - height * 0.5),
                                    Math.max(0, parent.height - height - 4))
                        width: cropAngleLabel.implicitWidth + 14
                        height: cropAngleLabel.implicitHeight + 10
                        radius: appTheme.badgeRadius
                        color: Qt.rgba(18 / 255, 18 / 255, 18 / 255, 210 / 255)
                        z: 4

                        Label {
                            id: cropAngleLabel
                            anchors.centerIn: parent
                            text: qsTr("%1°").arg(editorInteraction.rotationLabelDegrees.toFixed(1))
                            color: Qt.rgba(1, 1, 1, 0.96)
                            font.pixelSize: appTheme.fontSizeBody
                            font.family: appTheme.dataFontFamily
                        }
                    }

                    // Zoom readout (status chrome).
                    Label {
                        objectName: "editorZoomReadout"
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 10
                        visible: root.hasImage
                        text: qsTr("%1%").arg(Math.round(editorInteraction.zoom * 100))
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeBody
                        font.weight: appTheme.fontWeightStrong
                        z: 4
                    }

                    // Pointer handlers call the typed interaction surface. They do
                    // not own crop math or view transform state.
                    HoverHandler {
                        id: viewportHover
                        enabled: root.hasImage
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
                        // HoverHandler owns the platform cursor (Item has no cursorShape).
                        cursorShape: editorInteraction.hasCustomCursor
                                     ? editorInteraction.cursorShape
                                     : Qt.ArrowCursor
                        onPointChanged: {
                            if (viewportHover.hovered) {
                                editorInteraction.handleHoverMove(point.position.x, point.position.y)
                            }
                        }
                    }

                    // PointHandler records the true press position (no drag-distance
                    // threshold). DragHandler would only activate after the system
                    // drag distance, which lost crop-corner hit tests and click-zoom.
                    PointHandler {
                        id: viewportPointer
                        enabled: root.hasImage
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.TouchScreen | PointerDevice.Stylus
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        property int _activeButton: Qt.LeftButton
                        property bool _pressed: false
                        onActiveChanged: {
                            if (active) {
                                _pressed = true
                                _activeButton = (point.pressedButtons & Qt.MiddleButton)
                                        ? Qt.MiddleButton : Qt.LeftButton
                                editorInteraction.handlePress(
                                            point.position.x, point.position.y, _activeButton)
                            } else if (_pressed) {
                                editorInteraction.handleRelease(
                                            point.position.x, point.position.y, _activeButton)
                                _pressed = false
                            }
                        }
                        onPointChanged: {
                            if (active) {
                                editorInteraction.handleMove(
                                            point.position.x, point.position.y,
                                            point.pressedButtons)
                            }
                        }
                    }

                    // Phase 5D: a dedicated DragHandler is the reliable move
                    // source for pan (and crop-drag) while zoomed. The double-
                    // tap TapHandler below holds the left-button exclusive grab
                    // from press until release, which starves the PointHandler's
                    // passive grab of drag move events — so a left-button drag
                    // never pans (it falls through to the single-click zoom
                    // toggle on release). Middle button has no competing
                    // TapHandler so it already panned; the DragHandler is a
                    // harmless no-op there (handleMove is idempotent for a
                    // repeated position). The DragHandler takes the grab at the
                    // drag threshold and reports centroid moves to the same
                    // handleMove entry point; target:null so it never moves an
                    // item (the interaction controller owns the view transform).
                    // Enabled with an image in both crop-off (pan) and crop-on
                    // (crop-drag) modes: the controller's handleMove routes to
                    // crop first, so a crop-handle drag still moves the crop and
                    // a plain drag still pans.
                    DragHandler {
                        id: viewportPanDrag
                        enabled: root.hasImage
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.TouchScreen | PointerDevice.Stylus
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        target: null
                        onTranslationChanged: {
                            if (active) {
                                editorInteraction.handleMove(
                                            centroid.position.x, centroid.position.y,
                                            centroid.pressedButtons)
                            }
                        }
                    }

                    WheelHandler {
                        id: viewportWheel
                        enabled: root.hasImage
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onWheel: function (event) {
                            var synthesized = event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0
                            editorInteraction.handleWheel(
                                        event.x, event.y,
                                        event.angleDelta.y,
                                        event.pixelDelta.x, event.pixelDelta.y,
                                        event.modifiers,
                                        synthesized)
                            event.accepted = true
                        }
                    }

                    PinchHandler {
                        id: viewportPinch
                        enabled: root.hasImage
                        target: null
                        property real _lastScale: 1.0
                        onActiveChanged: {
                            _lastScale = 1.0
                        }
                        onScaleChanged: {
                            if (!active) {
                                return
                            }
                            // PinchHandler.scale is cumulative from gesture start.
                            // HandlePinchZoom expects Qt ZoomNativeGesture-style
                            // relative value where scale *= (1 + value).
                            var ratio = scale / Math.max(_lastScale, 1e-6)
                            var delta = ratio - 1.0
                            _lastScale = scale
                            if (Math.abs(delta) > 1e-4) {
                                editorInteraction.handlePinch(
                                            centroid.position.x, centroid.position.y, delta)
                            }
                        }
                    }

                    // Phase 5D: gesturePolicy DragThreshold so a left-button drag
                    // (now owned by viewportPanDrag above) cancels the tap instead
                    // of being held as a would-be double-tap. A clean double-click
                    // (two clicks without a drag) still fires onDoubleTapped and
                    // toggles the zoom; only drags are excluded, which is what
                    // separates pan from double-click-zoom.
                    TapHandler {
                        id: viewportDoubleTap
                        enabled: root.hasImage
                        acceptedButtons: Qt.LeftButton
                        gesturePolicy: TapHandler.DragThreshold
                        onDoubleTapped: function (eventPoint) {
                            editorInteraction.handleDoubleTap(
                                        eventPoint.position.x, eventPoint.position.y)
                        }
                    }

                    // Invisible focus/input owner for keyboard shortcuts (Phase 6G).
                    Item {
                        id: viewportInteractionLayer
                        objectName: "editorViewportInteractionLayer"
                        anchors.fill: parent
                        visible: root.hasImage
                        focus: root.hasImage
                        activeFocusOnTab: true
                        z: 5

                        Keys.onPressed: function (event) {
                            if (event.key === Qt.Key_0 || event.key === Qt.Key_Home) {
                                editorInteraction.resetView()
                                event.accepted = true
                            }
                        }

                        onVisibleChanged: {
                            if (!visible) {
                                editorInteraction.handleLeave()
                            }
                        }
                    }

                    function currentDevicePixelRatio() {
                        var win = viewportSlot.Window.window
                        if (!win) {
                            return 1.0
                        }
                        // QWindow has no devicePixelRatioChanged in Qt 6.9.3.
                        // Prefer the window's current screen; fall back to the property.
                        if (win.screen) {
                            return win.screen.devicePixelRatio
                        }
                        return win.devicePixelRatio
                    }

                    function ensurePresentationBinding() {
                        // Rebind on every open while the same viewport lives so
                        // Finalize→Open image switches keep presentationViewportBound.
                        if (root.editorSession && editorViewportItem) {
                            root.editorSession.bindPresentationViewport(editorViewportItem)
                        }
                    }

                    function syncViewportMetrics() {
                        // setViewportMetrics emits viewStateChanged → single push.
                        editorInteraction.setViewportMetrics(
                                    viewportSlot.width, viewportSlot.height,
                                    currentDevicePixelRatio())
                        if (root.editorSession) {
                            var dpr = currentDevicePixelRatio()
                            root.editorSession.updatePresentationTargetSize(
                                        Math.max(1, Math.round(viewportSlot.width * dpr)),
                                        Math.max(1, Math.round(viewportSlot.height * dpr)))
                        }
                    }

                    function resetAndSyncForImageSession() {
                        ensurePresentationBinding()
                        if (!root.hasImage) {
                            editorInteraction.resetPresentationStateForNewImage()
                            editorInteraction.setImageSize(0, 0)
                            return
                        }
                        // Drop previous image crop/ROI/mode before applying new geometry.
                        editorInteraction.resetPresentationStateForNewImage()
                        syncImageGeometry()
                    }

                    function syncImageGeometry() {
                        if (!root.hasImage) {
                            editorInteraction.setImageSize(0, 0)
                            editorInteraction.setRenderReferenceSize(0, 0)
                            return
                        }
                        if (!appModules || !appModules.images) {
                            return
                        }
                        var size = appModules.images.GetImagePixelSize(
                                    root.focusedElementId, root.focusedImageId)
                        if (size && size.success && size.width > 0 && size.height > 0) {
                            editorInteraction.setImageSize(size.width, size.height)
                            // Until a pipeline frame arrives (TargetSizeRequested), use
                            // source size so crop/zoom math is not zero-sized.
                            if (editorInteraction.renderReferenceWidth <= 0 ||
                                    editorInteraction.renderReferenceHeight <= 0) {
                                editorInteraction.setRenderReferenceSize(size.width, size.height)
                            }
                        }
                    }

                    function pushViewToViewport() {
                        // Full ViewerViewState (zoom/pan, region cache, interactive /
                        // detail flags) — not just three floats — so DirectFrameSink
                        // ROI requests track the controller after zoom and pan.
                        editorInteraction.applyViewStateToViewport(editorViewportItem)
                    }

                    Connections {
                        target: editorInteraction
                        // Single full-state notification; do not also listen to
                        // viewChanged (emitViewAndOverlay fires both).
                        function onViewStateChanged() {
                            viewportSlot.pushViewToViewport()
                        }
                    }

                    // EditorSessionController signals are PascalCase (StateChanged).
                    // Connections function handlers only match camelCase signal names, so
                    // watch NOTIFY-backed properties instead of onStateChanged handlers.
                    property string sessionIdentityKey: {
                        if (!root.editorSession) {
                            return ""
                        }
                        return root.editorSession.sessionGeneration
                                + ":" + root.editorSession.elementId
                                + ":" + root.editorSession.imageId
                                + ":" + (root.editorSession.active ? "1" : "0")
                    }
                    onSessionIdentityKeyChanged: {
                        if (sessionIdentityKey.length > 0) {
                            resetAndSyncForImageSession()
                        }
                    }

                    property bool presentationBound: root.editorSession
                                                    ? root.editorSession.presentationViewportBound
                                                    : false
                    onPresentationBoundChanged: {
                        if (presentationBound) {
                            pushViewToViewport()
                        }
                    }

                    // Qt 6.9.3: QQuickWindow has no devicePixelRatioChanged.
                    // QScreen.devicePixelRatio uses physicalDotsPerInchChanged as NOTIFY.
                    property var trackedScreen: null
                    property real trackedScreenDpr: trackedScreen ? trackedScreen.devicePixelRatio : 1.0
                    onTrackedScreenDprChanged: syncViewportMetrics()

                    function refreshTrackedScreen() {
                        var win = viewportSlot.Window.window
                        trackedScreen = (win && win.screen) ? win.screen : null
                    }

                    Connections {
                        target: viewportSlot.Window.window
                        ignoreUnknownSignals: true
                        function onScreenChanged() {
                            viewportSlot.refreshTrackedScreen()
                            viewportSlot.syncViewportMetrics()
                        }
                    }

                    Component.onCompleted: {
                        refreshTrackedScreen()
                        syncViewportMetrics()
                        ensurePresentationBinding()
                        syncImageGeometry()
                    }
                    onWidthChanged: syncViewportMetrics()
                    onHeightChanged: syncViewportMetrics()
                }

                EditorFilmstrip {
                    id: editorFilmstrip
                    Layout.fillWidth: true
                    Layout.preferredHeight: editorFilmstrip.dockHeight
                    theme: root.theme
                    editorSession: root.editorSession
                    // Phase 1B: empty model; position/count remain defined for handle UI.
                    currentIndex: root.hasImage ? 1 : 0
                    totalCount: root.hasImage ? 1 : 0
                    saveInProgress: false
                }
            }

            // Right: histogram/waveform + adjustment navbar + panel stack.
            EditorAdjustmentStack {
                id: adjustmentStack
                Layout.fillHeight: true
                theme: root.theme
                editorSession: root.editorSession
                controlsEnabled: root.hasImage
            }
        }
    }
}
