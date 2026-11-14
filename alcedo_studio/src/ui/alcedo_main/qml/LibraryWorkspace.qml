import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Controls.impl
import QtQuick.Layouts
import QtQuick.Effects
import "FileDrop.js" as FileDrop

// Extracted library/album surface. Layout and behavior match the former Main.qml body.
Item {
    id: root
    objectName: "libraryWorkspace"
    focus: true

    // Main shell — selection, dialogs, project helpers.
    property var host: null
    // Optional theme override; defaults to host so color helpers stay shared.
    property var theme: host

    readonly property color colBgDeep: theme ? theme.colBgDeep : "#0C0D0F"
    readonly property color colBgBase: theme ? theme.colBgBase : "#141516"
    readonly property color colBgPanel: theme ? theme.colBgPanel : "#1C1C1D"
    readonly property color colBgCanvas: theme ? theme.colBgCanvas : "#111214"
    readonly property int panelRadius: theme ? theme.panelRadius : 12
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colTextMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccentPrimary: theme ? theme.colAccentPrimary : "#457B9D"
    readonly property color colAccentSecondary: theme ? theme.colAccentSecondary : "#6D93B7"
    readonly property color colDivider: theme ? theme.colDivider : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colHover: theme ? theme.colHover : Qt.rgba(1, 1, 1, 0.07)
    readonly property color colGlassPanel: theme ? theme.colGlassPanel : "#1C1C1D"
    readonly property color colGlassStroke: theme ? theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colButtonPrimary: theme ? theme.colButtonPrimary : "#457B9D"
    readonly property color colButtonSecondary: theme ? theme.colButtonSecondary : "#3A3F44"
    readonly property color colButtonSecondaryBorder: theme ? theme.colButtonSecondaryBorder : Qt.rgba(1, 1, 1, 0.12)
    readonly property color colButtonHighlight: theme ? theme.colButtonHighlight : "#E9C46A"
    readonly property color colDanger: theme ? theme.colDanger : appTheme.dangerColor
    readonly property string dataFontFamily: theme ? theme.dataFontFamily : appTheme.dataFontFamily
    readonly property string headlineFontFamily: theme ? theme.headlineFontFamily : appTheme.headlineFontFamily
    readonly property int controlRadius: theme ? theme.controlRadius : 10

    function secondaryButtonFill(enabled, hovered, pressed) {
        if (theme && theme.secondaryButtonFill) {
            return theme.secondaryButtonFill(enabled, hovered, pressed)
        }
        return colButtonSecondary
    }

    // Neutral filled action (Load Project, Export): the Main shell owns its colours.
    function filledActionFill(enabled, hovered, pressed) {
        if (theme && theme.filledActionFill) {
            return theme.filledActionFill(enabled, hovered, pressed)
        }
        return enabled ? appTheme.editorListSelectedFillColor : appTheme.bgBaseColor
    }

    function filledActionInk(enabled) {
        if (theme && theme.filledActionInk) {
            return theme.filledActionInk(enabled)
        }
        return enabled ? appTheme.editorListSelectedInkColor : appTheme.textMutedColor
    }

    // Inspector / browser view state is owned by the Main shell (host) so values
    // survive Loader teardown when entering the editor workspace. Snapshot on
    // create; write through on change (avoid live bindings that fight Slider).
    property bool inspectorVisible: true
    property real inspectorWidth: 300
    readonly property real inspectorMinWidth: 300
    readonly property real inspectorMaxWidth: 600
    readonly property real centerPaneMinWidth: 560
    // Collections now live on the Main shell. This width is the right-hand
    // stack only (Top toolbar + workspace). One 12px gap plus the 5px handle.
    readonly property real contentRowSpacingTotal: 12
    readonly property real inspectorAdaptiveMaxWidth: Math.max(
        0,
        root.width
        - centerPaneMinWidth
        - contentRowSpacingTotal
        - 5)
    readonly property int defaultGridZoomLevel: 4
    // Thumbnail tier of the grid view; 0 when the grid view is not loaded.
    readonly property int thumbnailMaxEdge: contentViewLoader.item
                                            && contentViewLoader.item.desiredMaxEdge !== undefined
                                            ? contentViewLoader.item.desiredMaxEdge : 0
    property int gridZoomLevel: defaultGridZoomLevel
    // True while the workspace width follows the collections sidebar fold (WorkspaceHost).
    property bool geometryAnimating: false
    property bool _viewStateReady: false
    property real libraryGridRevealOpacity: 1
    property real libraryGridRevealShift: 0

    // Library loading (DESIGN.md "Library loading"). A refresh that replaces the shown result
    // (a collection switch, a filter, the search) shows the staggered grid loader after
    // libraryLoadingRevealDelayMs, so a fast read draws no frame of it. In-place refreshes
    // (a rating write, an import into the shown collection) keep the grid as it is.
    readonly property bool libraryQueryReplacing: appModules.library.queryReplacing
                                                  && appModules.library.queryError.length === 0
    readonly property int libraryLoadingRevealDelayMs: 150
    property bool libraryLoadingShown: false
    // Opacity of the previous result: it dims while a replacing read runs over it.
    property real libraryStaleViewOpacity: libraryLoadingShown && contentViewLoader.active
                                           ? 0.3 : 1
    Behavior on libraryStaleViewOpacity {
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : appTheme.motionFoldOpenMs
            easing.type: appTheme.motionEasing
        }
    }
    readonly property var libraryZoomColumns: [2, 3, 4, 5, 6, 8, 11, 14]

    onLibraryQueryReplacingChanged: {
        if (libraryQueryReplacing) {
            libraryLoadingRevealTimer.restart()
        } else {
            libraryLoadingRevealTimer.stop()
            libraryLoadingShown = false
        }
    }

    Timer {
        id: libraryLoadingRevealTimer
        interval: root.libraryLoadingRevealDelayMs
        onTriggered: root.libraryLoadingShown = root.libraryQueryReplacing
    }

    // Card geometry of the classic photo grid at the current zoom level and @p viewWidth; the
    // same formula as ThumbnailGridView.metricsForZoomLevel, so the placeholder tiles stand
    // where the photo cards appear.
    function libraryLoadingGridMetrics(viewWidth) {
        const level = Math.max(0, Math.min(libraryZoomColumns.length - 1, gridZoomLevel))
        const visualCols = libraryZoomColumns[level]
        const inset = visualCols >= 11 ? 4 : (visualCols >= 8 ? 6 : 8)
        const gap = visualCols >= 11 ? 4 : (visualCols >= 8 ? 6 : 12)
        const textHeight = visualCols >= 14 ? 0 : (visualCols >= 8 ? 18 : 50)
        const cellW = Math.max(72, Math.floor(viewWidth / visualCols))
        const cellH = Math.max(64, Math.round((cellW - inset * 2) * 2 / 3
                                              + textHeight + inset * 2 + gap))
        const cols = Math.max(1, Math.floor(viewWidth / cellW))
        return {
            cols: cols,
            width: cols * cellW - gap,
            gap: gap,
            aspect: (cellW - gap) / Math.max(1, cellH - gap),
            radius: visualCols >= 11 ? 6 : appTheme.panelRadius
        }
    }

    function prepareLibraryGridReveal() {
        libraryGridRevealAnim.stop()
        if (appTheme.reduceMotion) {
            libraryGridRevealOpacity = 1
            libraryGridRevealShift = 0
            return
        }
        libraryGridRevealOpacity = 0
        libraryGridRevealShift = appTheme.spaceMd
    }

    function playLibraryGridReveal() {
        if (appTheme.reduceMotion) {
            libraryGridRevealAnim.stop()
            libraryGridRevealOpacity = 1
            libraryGridRevealShift = 0
            return
        }
        if (libraryGridRevealOpacity >= 1 && libraryGridRevealShift === 0) {
            return
        }
        libraryGridRevealAnim.restart()
    }

    function applyHostViewState() {
        if (!host) {
            return
        }
        _viewStateReady = false
        inspectorVisible = host.libraryInspectorVisible
        inspectorWidth = host.libraryInspectorWidth
        gridZoomLevel = host.libraryGridZoomLevel
        _viewStateReady = true
    }

    function persistViewState() {
        if (!host) {
            return
        }
        host.libraryInspectorVisible = inspectorVisible
        host.libraryInspectorWidth = inspectorWidth
        host.libraryGridZoomLevel = gridZoomLevel
        const view = contentViewLoader.item
        if (view && view.contentY !== undefined) {
            if (host.contentY !== undefined) {
                host.contentY = view.contentY
            } else {
                host.libraryGridContentY = view.contentY
            }
        }
    }

    function hostContentY() {
        if (!host) {
            return 0
        }
        return host.contentY !== undefined
                ? Number(host.contentY)
                : Number(host.libraryGridContentY || 0)
    }

    function restoreScrollPosition() {
        const view = contentViewLoader.item
        if (!view || !host) {
            return
        }
        if (view.restoreScrollAnchor) {
            // The grouped view reads the anchor once; it carries its group field, so groups of
            // another field start at the top.
            view.restoreScrollAnchor(host.librarySectionScrollAnchor)
            host.librarySectionScrollAnchor = null
        } else if (view.restoreContentY) {
            view.restoreContentY(root.hostContentY())
        } else {
            return
        }
        Qt.callLater(root.revealRequestedImage)
        Qt.callLater(root.revealPendingFile)
    }

    // The last edited photo of a newly opened project, requested while this workspace is
    // shown: the library finds its position on the query worker (selecting the root folder
    // when the current folder lacks it), loads its page, and the view that shows that result
    // scrolls to it (focusPositionReady).
    function revealPendingFile() {
        if (appModules.library.pendingRevealFileId > 0) {
            appModules.library.RequestPendingReveal()
        }
    }

    function revealRequestedImage() {
        const view = contentViewLoader.item
        if (!view || !host) {
            return
        }
        const elementId = Number(host.libraryScrollTargetElementId || 0)
        if (elementId <= 0) {
            return
        }

        const requestedIndex = Number(host.libraryScrollTargetIndex)
        let revealed = false
        if (requestedIndex >= 0 && view.scrollToIndexAtTop) {
            revealed = view.scrollToIndexAtTop(requestedIndex)
        }
        if (!revealed && view.scrollToElementAtTop) {
            revealed = view.scrollToElementAtTop(elementId)
        }
        if (!revealed) {
            return
        }

        if (host.clearLibraryScrollTarget) {
            host.clearLibraryScrollTarget(elementId)
        }
        root.persistViewState()
    }

    function focusLibraryItem(item, index) {
        if (!item || !host) {
            return
        }
        host.setFocusedImage(item)
        if (host.requestFilmstripScrollToElement) {
            host.requestFilmstripScrollToElement(item.elementId, index)
        }
    }

    onInspectorVisibleChanged: {
        if (_viewStateReady && host) {
            host.libraryInspectorVisible = inspectorVisible
        }
    }
    onInspectorWidthChanged: {
        if (_viewStateReady && host) {
            host.libraryInspectorWidth = inspectorWidth
        }
    }
    onGridZoomLevelChanged: {
        if (_viewStateReady && host) {
            host.libraryGridZoomLevel = gridZoomLevel
        }
    }

    // Keep local mirrors in sync when the top-toolbar toggle flips host state.
    Connections {
        target: host
        ignoreUnknownSignals: true
        function onLibraryInspectorVisibleChanged() {
            if (root.inspectorVisible !== host.libraryInspectorVisible) {
                root.inspectorVisible = host.libraryInspectorVisible
            }
        }
        function onLibraryScrollRequestIdChanged() {
            Qt.callLater(root.revealRequestedImage)
        }
    }

    Connections {
        target: appModules.library
        function onPendingRevealChanged() {
            Qt.callLater(root.revealPendingFile)
        }
    }

    Connections {
        target: appModules.library.thumbnailModel
        ignoreUnknownSignals: true
        function onCountChanged() {
            Qt.callLater(root.revealRequestedImage)
        }
        function onTotalCountChanged() {
            Qt.callLater(root.revealRequestedImage)
        }
    }

    Component.onCompleted: {
        root.applyHostViewState()
        Qt.callLater(root.revealPendingFile)
        if (root.libraryQueryReplacing) {
            libraryLoadingRevealTimer.restart()
        }
    }
    Component.onDestruction: root.persistViewState()

RowLayout {
    anchors.fill: parent
    spacing: 12

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: root.centerPaneMinWidth
        spacing: 10

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: root.panelRadius
            color: root.colGlassPanel
            border.width: 1
            border.color: root.colGlassStroke
            clip: true

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            anchors.topMargin: 10
            anchors.bottomMargin: 0
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                Label { text: qsTr("Browser"); color: root.colTextMuted; font.pixelSize: 13; font.weight: 600 }
                Item { Layout.fillWidth: true }

                // ── Zoom slider ──
                // Left end: the most photos (the smallest thumbnails, zoom level 7); right end:
                // the largest photos (zoom level 0). The slider runs in the same direction.
                Item {
                    Layout.preferredWidth: 236
                    Layout.preferredHeight: appTheme.iconButtonHitSizeCompact

                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onDoubleTapped: root.gridZoomLevel = root.defaultGridZoomLevel
                    }

                    RowLayout {
                        anchors.fill: parent
                        spacing: 8

                        IconActionButton {
                            objectName: "libraryZoomSmallerButton"
                            compact: true
                            fillIdle: "transparent"
                            iconSrc: "qrc:/panel_icons/thumbnail-zoom-smallest.svg"
                            actionName: qsTr("Show more photos")
                            enabled: root.gridZoomLevel < 7
                            onClicked: root.gridZoomLevel = Math.min(7, root.gridZoomLevel + 1)
                        }

                        Slider {
                            id: zoomSlider
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            from: 7
                            to: 0
                            stepSize: 1
                            value: root.gridZoomLevel
                            onValueChanged: root.gridZoomLevel = Math.round(value)
                            background: Rectangle {
                                x: zoomSlider.leftPadding
                                y: zoomSlider.topPadding + zoomSlider.availableHeight / 2 - height / 2
                                implicitWidth: zoomSlider.availableWidth
                                implicitHeight: 4
                                width: zoomSlider.availableWidth
                                height: implicitHeight
                                radius: 2
                                color: Qt.rgba(root.colBgBase.r, root.colBgBase.g, root.colBgBase.b, 0.98)
                                Rectangle {
                                    width: zoomSlider.visualPosition * parent.width
                                    height: parent.height
                                    color: appTheme.textColor
                                    radius: 2
                                }
                            }
                            handle: Rectangle {
                                x: zoomSlider.leftPadding + zoomSlider.visualPosition * (zoomSlider.availableWidth - width)
                                y: zoomSlider.topPadding + zoomSlider.availableHeight / 2 - height / 2
                                implicitWidth: 14
                                implicitHeight: 14
                                radius: 7
                                color: appTheme.textColor
                            }
                        }

                        IconActionButton {
                            objectName: "libraryZoomLargerButton"
                            compact: true
                            fillIdle: "transparent"
                            iconSrc: "qrc:/panel_icons/thumbnail-zoom-largest.svg"
                            actionName: qsTr("Show larger photos")
                            enabled: root.gridZoomLevel > 0
                            onClicked: root.gridZoomLevel = Math.max(0, root.gridZoomLevel - 1)
                        }
                    }
                }

                Item { Layout.preferredWidth: 10 }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Query failure: the last accepted view stays visible and the row offers
                // Retry. A running read shows the loading grid instead (libraryLoadingShown).
                RowLayout {
                    id: libraryQueryStateRow
                    objectName: "libraryQueryStateRow"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    z: 2
                    visible: appModules.library.queryError.length > 0
                    spacing: appTheme.spaceSm

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Library query failed: %1").arg(appModules.library.queryError)
                        color: appTheme.dangerColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }

                    DialogActionButton {
                        id: libraryQueryRetryButton
                        objectName: "libraryQueryRetryButton"
                        visible: appModules.library.queryError.length > 0
                        buttonWidth: 96
                        buttonHeight: appTheme.iconButtonHitSizeCompact
                        text: qsTr("Retry")
                        Accessible.name: qsTr("Retry the library query")
                        onClicked: appModules.library.RetryLibraryQuery()
                    }
                }

                Loader {
                    id: contentViewLoader
                    objectName: "libraryContentViewLoader"
                    anchors.fill: parent
                    anchors.topMargin: libraryQueryStateRow.visible
                                       ? libraryQueryStateRow.implicitHeight + appTheme.spaceXs : 0
                    active: appModules.library.grouped
                            ? appModules.library.sectionModel.groupCount > 0
                            : appModules.library.shownCount > 0
                    sourceComponent: appModules.library.grouped ? sectionComp : gridComp
                    opacity: root.libraryGridRevealOpacity * root.libraryStaleViewOpacity
                    transform: Translate { y: root.libraryGridRevealShift }
                    onLoaded: {
                        if (host && host.projectLoadingOverlayVisible) {
                            root.prepareLibraryGridReveal()
                        } else {
                            root.playLibraryGridReveal()
                        }
                        Qt.callLater(root.restoreScrollPosition)
                    }
                }

                // Drop of local files and folders onto the grid: import into the current
                // collection, the same as the Import buttons (issue #757).
                DropArea {
                    id: gridFileDropArea
                    objectName: "libraryGridFileDropArea"
                    anchors.fill: parent
                    keys: ["text/uri-list"]

                    readonly property int targetFolderId: Number(appModules.folders.currentFolderId)
                    property var urls: []

                    // The action is always Copy: import reads the files in place, and a Move
                    // answer lets the file manager delete them.
                    onEntered: function(drag) {
                        urls = host && host.fileDropImportAllowed ? FileDrop.localFileUrls(drag) : []
                        if (urls.length > 0) {
                            drag.accept(Qt.CopyAction)
                        } else {
                            drag.accepted = false
                        }
                    }
                    onExited: urls = []
                    onDropped: function(drop) {
                        const dropped = urls.length > 0 ? urls : FileDrop.localFileUrls(drop)
                        urls = []
                        if (!host || !host.fileDropImportAllowed || dropped.length === 0) {
                            drop.accepted = false
                            return
                        }
                        drop.accept(Qt.CopyAction)
                        host.importDroppedUrls(dropped, targetFolderId)
                    }
                }

                // Drop overlay: a hairline frame on a dimmed grid with the target collection.
                Rectangle {
                    objectName: "libraryGridFileDropOverlay"
                    anchors.fill: parent
                    z: 5
                    visible: gridFileDropArea.containsDrag && gridFileDropArea.urls.length > 0
                    radius: appTheme.controlRadiusSmall
                    color: Qt.rgba(root.colBgDeep.r, root.colBgDeep.g, root.colBgDeep.b, 0.78)
                    border.width: 1
                    border.color: appTheme.textMutedColor

                    ColumnLayout {
                        anchors.centerIn: parent
                        width: Math.min(parent.width - appTheme.spaceXl * 2, 420)
                        spacing: appTheme.spaceSm

                        IconImage {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            source: "qrc:/panel_icons/import.svg"
                            sourceSize: Qt.size(28, 28)
                            color: root.colText
                        }

                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideMiddle
                            text: qsTr("Import to %1")
                                  .arg(host ? host.collectionDisplayName(gridFileDropArea.targetFolderId) : "")
                            color: root.colText
                            font.family: root.headlineFontFamily
                            font.pixelSize: appTheme.fontSizeSection
                            font.weight: appTheme.fontWeightHeading
                        }
                    }
                }

                // Loading, no result shown (a collection switch): placeholder cards at the
                // grid's column count and card proportion, in place of the empty state.
                Item {
                    id: libraryLoadingGrid
                    objectName: "libraryLoadingGrid"
                    anchors.fill: parent
                    anchors.topMargin: contentViewLoader.anchors.topMargin
                    z: 1
                    clip: true
                    visible: root.libraryLoadingShown && !contentViewLoader.active

                    readonly property var metrics: root.libraryLoadingGridMetrics(width)

                    StaggeredGridLoader {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: libraryLoadingGrid.metrics.width
                        running: libraryLoadingGrid.visible
                        columns: libraryLoadingGrid.metrics.cols
                        rows: 0
                        spacing: libraryLoadingGrid.metrics.gap
                        tileAspect: libraryLoadingGrid.metrics.aspect
                        tileRadius: libraryLoadingGrid.metrics.radius
                        cycleMs: 1600
                        waveStrength: 0.1
                        accessibleName: qsTr("Updating")
                    }
                }

                // Loading over the previous result (a filter or search change): the dimmed
                // grid stays and a compact staggered grid stands at its centre.
                Rectangle {
                    id: libraryLoadingMark
                    objectName: "libraryLoadingMark"
                    anchors.centerIn: parent
                    z: 3
                    width: 72
                    height: 72
                    radius: appTheme.panelRadius
                    color: root.colBgPanel
                    border.width: 1
                    border.color: root.colGlassStroke
                    visible: opacity > 0
                    opacity: root.libraryLoadingShown && contentViewLoader.active ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: appTheme.reduceMotion ? 0 : appTheme.motionFadeMs
                            easing.type: appTheme.motionEasing
                        }
                    }

                    StaggeredGridMark {
                        anchors.centerIn: parent
                        width: 36
                        height: 36
                        running: root.libraryLoadingShown
                        accessibleName: qsTr("Updating")
                    }
                }

                ParallelAnimation {
                    id: libraryGridRevealAnim
                    NumberAnimation {
                        target: root
                        property: "libraryGridRevealOpacity"
                        to: 1
                        duration: appTheme.reduceMotion ? 0 : appTheme.motionFoldOpenMs
                        easing.type: Easing.OutCubic
                    }
                    NumberAnimation {
                        target: root
                        property: "libraryGridRevealShift"
                        to: 0
                        duration: appTheme.reduceMotion ? 0 : appTheme.motionFoldOpenMs
                        easing.type: Easing.OutCubic
                    }
                }

                Column {
                    readonly property bool hasLibraryFilter:
                        appModules.search.activeSearchQuery.length > 0
                        || appModules.stats.statsFilterDate.length > 0
                        || appModules.stats.statsFilterImportDate.length > 0
                        || appModules.stats.statsFilterCamera.length > 0
                        || appModules.stats.statsFilterLens.length > 0
                        || appModules.stats.statsFilterLabel.length > 0
                        || appModules.stats.statsFilterRating.length > 0
                    readonly property bool noMatchingPhotos: appModules.project.serviceReady
                                                             && hasLibraryFilter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: contentViewLoader.anchors.topMargin
                    // A replacing read decides whether the scope is empty; until it publishes,
                    // the loading grid (after its delay) stands here.
                    visible: appModules.library.shownCount === 0
                             && appModules.library.queryError.length === 0
                             && !root.libraryQueryReplacing
                    spacing: 8
                    Label {
                        text: parent.noMatchingPhotos ? qsTr("No Matching Photos")
                              : appModules.project.serviceReady ? qsTr("No Photos Yet") : qsTr("Open or Create a Project")
                        font.family: root.headlineFontFamily
                        color: root.colText
                        font.pixelSize: 22
                        font.weight: 700
                    }
                    Label {
                        text: parent.noMatchingPhotos
                              ? qsTr("Change the filters or the search to show photos.")
                              : appModules.project.serviceReady
                              ? qsTr("Import your images for RAW adjustments.")
                              : qsTr("Use File > Load Project or File > Create Project to choose .alcd files.")
                        color: root.colTextMuted
                        font.pixelSize: 12
                    }
                    Button {
                        id: emptyStateLoadButton
                        visible: !appModules.project.serviceReady
                        enabled: appModules.interactionPolicy.canChangeStateAsUser
                        text: qsTr("Load Project")
                        hoverEnabled: true
                        leftPadding: appTheme.spaceLg
                        rightPadding: appTheme.spaceLg
                        background: Rectangle {
                            implicitWidth: 100
                            implicitHeight: 40
                            radius: root.controlRadius
                            color: root.filledActionFill(emptyStateLoadButton.enabled,
                                                         emptyStateLoadButton.hovered,
                                                         emptyStateLoadButton.down)
                            Behavior on color {
                                enabled: !appTheme.reduceMotion
                                ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
                            }
                        }
                        contentItem: Label {
                            text: emptyStateLoadButton.text
                            color: root.filledActionInk(emptyStateLoadButton.enabled)
                            font: emptyStateLoadButton.font
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: appModules.projectLaunch.BeginPromptOpen()
                    }
                }
            }
        }
        } // close album card Rectangle

    } // close center block wrapper

    // ── inspector panel + overlay resize handle ──
    Item {
        id: inspectorContainer
        Layout.fillHeight: true
        Layout.minimumWidth: 0
        Layout.maximumWidth: root.inspectorAdaptiveMaxWidth
        Layout.preferredWidth: root.inspectorVisible
                               ? Math.min(root.inspectorWidth, root.inspectorAdaptiveMaxWidth)
                               : 0
        Behavior on Layout.preferredWidth { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

        // One panel: the Inspector pages, then the marks card and the button row at the bottom.
        // The marks card and the buttons have the inset of the Album section cards (the 10 px
        // page margin plus spaceMd), so every card of the panel has one width.
        Rectangle {
            anchors.fill: parent
            radius: root.panelRadius
            color: root.colBgPanel
            border.width: 0
            clip: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                InspectorPanel {
                    id: inspectorPanel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.topMargin: 10
                    Layout.leftMargin: 10
                    Layout.rightMargin: 10
                    focusedImage: host.focusedImageInspection
                    interactionPolicy: appModules.interactionPolicy
                    selectionState: host.selectionState
                    selectedCount: host.selectedCount
                    onRatingRequested: function(rating) {
                        host.requestSetFocusedImageRating(rating)
                    }
                    onDescriptionSaveRequested: function(caption) {
                        host.requestSaveFocusedDescription(caption)
                    }
                    onRatingReasonSaveRequested: function(reasons) {
                        host.requestSaveFocusedRatingReason(reasons)
                    }
                    onContextMenuRequested: function(item, sceneX, sceneY) {
                        host.openImageContextMenu(item, sceneX, sceneY)
                    }
                    onColorLabelSettingsRequested: host.openColorLabelSettings()
                }

                ColumnLayout {
                    objectName: "libraryInspectorFooter"
                    Layout.fillWidth: true
                    Layout.topMargin: appTheme.spaceSm
                    Layout.leftMargin: 10 + appTheme.spaceMd
                    Layout.rightMargin: 10 + appTheme.spaceMd
                    Layout.bottomMargin: 10 + appTheme.spaceMd
                    spacing: appTheme.spaceSm

                    // Rating, color labels, and Delete mark of the selection (#772).
                    LibraryMarksControl {
                        Layout.fillWidth: true
                        visible: !inspectorPanel.exportPageActive
                        selectionState: host.selectionState
                        selectedCount: host.selectedCount
                        backendInteractive: host.backendInteractive
                        theme: root
                        onMessageRequested: function(message) { host.showSnackbar(message) }
                        onRatingApplied: host.refreshFocusedImageInspection()
                    }

                    LibraryInspectorActions {
                        Layout.fillWidth: true
                        Layout.preferredHeight: buttonHeight
                        inspectorPanel: inspectorPanel
                        selectionState: host.selectionState
                        selectedCount: host.selectedCount
                        backendInteractive: host.backendInteractive
                        exportQueueCount: host.exportQueueCount
                        theme: root
                    }
                }
            }
        }

        Rectangle {
            id: inspectorResizeHandle
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: root.inspectorVisible && root.inspectorAdaptiveMaxWidth > 0 ? 5 : 0
            x: -Math.round(width / 2)
            color: dragArea.containsMouse || dragArea.drag.active ? appTheme.textMutedColor : "transparent"
            visible: width > 0
            z: 10
            Behavior on width { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            Behavior on color { ColorAnimation { duration: 120 } }

            MouseArea {
                id: dragArea
                anchors.fill: parent
                anchors.margins: -3          // widen the hit area
                hoverEnabled: true
                cursorShape: Qt.SplitHCursor
                property real startX: 0
                property real startWidth: 0
                onPressed: function(mouse) {
                    startX = mapToGlobal(mouse.x, 0).x
                    startWidth = root.inspectorWidth
                }
                onPositionChanged: function(mouse) {
                    if (!pressed) return
                    var globalX = mapToGlobal(mouse.x, 0).x
                    var delta = startX - globalX   // dragging left ⇒ wider
                    var cappedMax = Math.min(root.inspectorMaxWidth, root.inspectorAdaptiveMaxWidth)
                    var target = startWidth + delta
                    if (cappedMax >= root.inspectorMinWidth) {
                        root.inspectorWidth = Math.max(root.inspectorMinWidth, Math.min(cappedMax, target))
                    } else {
                        root.inspectorWidth = Math.max(0, Math.min(cappedMax, target))
                    }
                }
            }
        }
    }
}


    Component {
        id: sectionComp
        AlbumSectionView {
            objectName: "libraryAlbumSectionView"
            zoomLevel: root.gridZoomLevel
            // Leaving the library (or the grouped view) keeps the photo at the top of the view.
            Component.onDestruction: {
                if (host) {
                    host.librarySectionScrollAnchor = scrollAnchor()
                }
            }
            exportQueueById: host.exportQueueById
            onZoomChanged: function(level) { root.gridZoomLevel = level }
            onImageSelectionChanged: function(elementId, imageId, fileName, isHdr, selected) {
                host.selectionState.setImageSelected(elementId, imageId, fileName, isHdr, selected)
            }
            onReplaceSelection: function(items) {
                host.selectionState.replaceSelectedImages(items)
                if (items && items.length > 0) {
                    const index = appModules.library.thumbnailModel.rowByElementId(
                        Number(items[0].elementId))
                    root.focusLibraryItem(items[0], index)
                } else {
                    host.setFocusedImage(null)
                }
            }
            onImageFocused: function(item, index) {
                root.focusLibraryItem(item, index)
            }
            onContextMenuRequested: function(item, sceneX, sceneY) {
                host.openImageContextMenu(item, sceneX, sceneY)
            }
            onRatingRequested: function(item, rating) {
                host.requestSetCardImageRating(item, rating)
            }
        }
    }

    Component {
        id: gridComp
        ThumbnailGridView {
            objectName: "libraryThumbnailGridView"
            zoomLevel: root.gridZoomLevel
            // Ctrl+wheel: the grid asks for the level, so the zoomLevel binding stays.
            onZoomLevelRequested: function(level) { root.gridZoomLevel = level }
            zoomAdjusting: zoomSlider.pressed
            geometryAnimating: root.geometryAnimating
            onZoomLevelChanged: root.gridZoomLevel = zoomLevel
            onContentYChanged: {
                if (host) {
                    if (host.contentY !== undefined) {
                        host.contentY = contentY
                    } else {
                        host.libraryGridContentY = contentY
                    }
                }
            }
            exportQueueById: host.exportQueueById
            onImageSelectionChanged: function(elementId, imageId, fileName, isHdr, selected) {
                host.selectionState.setImageSelected(elementId, imageId, fileName, isHdr, selected)
            }
            onReplaceSelection: function(items) {
                host.selectionState.replaceSelectedImages(items)
                if (items && items.length > 0) {
                    const index = appModules.library.thumbnailModel.rowByElementId(
                        Number(items[0].elementId))
                    root.focusLibraryItem(items[0], index)
                } else {
                    host.setFocusedImage(null)
                }
            }
            onImageFocused: function(item, index) {
                root.focusLibraryItem(item, index)
            }
            onContextMenuRequested: function(item, sceneX, sceneY) {
                host.openImageContextMenu(item, sceneX, sceneY)
            }
            onRatingRequested: function(item, rating) {
                host.requestSetCardImageRating(item, rating)
            }
        }
    }
}
