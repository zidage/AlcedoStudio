import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

Item {
    id: root
    clip: true
    readonly property color cardBg: "transparent"
    readonly property color cardBgSelected: appTheme.selectedTintColor
    readonly property color cardBgHover: appTheme.hoverColor
    readonly property color cardMuted: appTheme.textMutedColor
    readonly property color cardText: appTheme.textColor
    readonly property color cardAccent: appTheme.accentColor
    readonly property color cardDanger: appTheme.dangerColor
    readonly property color cardDangerTint: appTheme.dangerTintColor
    readonly property int dataFontWeight: 500
    readonly property real dataLetterSpacing: -0.2

    property var selectedImagesById: ({})
    property var exportQueueById: ({})

    // ── Zoom control ──────────────────────────────────────────────────
    // zoomLevel maps to column counts: 2, 3, 4, 5, 6, 8, 11, 14
    readonly property var zoomColumns: [2, 3, 4, 5, 6, 8, 11, 14]
    // resolution tiers per zoom level (max edge px)
    readonly property var zoomResolutionEdges: [1024, 1024, 1024, 512, 512, 256, 256, 256]
    readonly property int zoomLevelCount: zoomColumns.length
    property int zoomLevel: 4  // default: 6 columns

    readonly property int columns: zoomColumns[Math.min(zoomLevel, zoomLevelCount - 1)]
    readonly property int desiredMaxEdge: zoomResolutionEdges[Math.min(zoomLevel, zoomLevelCount - 1)]
    readonly property bool compactText: columns >= 8
    readonly property bool hideMetadata: columns >= 8
    readonly property bool hideAllText: columns >= 14
    readonly property int cardInset: columns >= 11 ? 4 : (columns >= 8 ? 6 : 8)
    readonly property int delegateGap: columns >= 11 ? 4 : (columns >= 8 ? 6 : 12)
    readonly property int textAreaHeight: hideAllText ? 0 : (hideMetadata ? 18 : 42)
    readonly property int titleFontSize: columns >= 11 ? 9 : (columns >= 8 ? 10 : 12)
    readonly property int metadataFontSize: columns >= 8 ? 8 : 10

    signal imageSelectionChanged(int elementId, int imageId, string fileName, bool selected)
    signal replaceSelection(var items)
    signal contextMenuRequested(var item, real sceneX, real sceneY)
    signal zoomChanged(int zoomLevel)

    onZoomLevelChanged: {
        zoomChanged(zoomLevel)
        updateCacheHint()
    }
    onWidthChanged: updateCacheHint()
    onHeightChanged: updateCacheHint()

    function maxContentY() {
        return Math.max(0, grid.contentHeight - grid.height)
    }

    function clampContentY() {
        grid.contentY = Math.max(0, Math.min(maxContentY(), grid.contentY))
    }

    function setZoomLevelAt(nextZoomLevel, focusViewportY) {
        const clamped = Math.max(0, Math.min(zoomLevelCount - 1, nextZoomLevel))
        if (clamped === zoomLevel) {
            return
        }

        const oldHeight = Math.max(1, grid.contentHeight)
        const focusY = Math.max(0, Math.min(grid.height, focusViewportY))
        const focusRatio = (grid.contentY + focusY) / oldHeight
        zoomLevel = clamped
        Qt.callLater(function() {
            grid.forceLayout()
            grid.contentY = Math.max(0, Math.min(maxContentY(),
                                                 focusRatio * grid.contentHeight - focusY))
            updateCacheHint()
        })
    }

    function updateCacheHint() {
        if (grid.width <= 0 || grid.cellWidth <= 0 || grid.cellHeight <= 0) {
            return
        }
        const cols = Math.max(1, Math.floor(grid.width / grid.cellWidth))
        const rows = Math.max(1, Math.floor(grid.height / grid.cellHeight))
        albumBackend.SetThumbnailCacheHint(cols * rows)
    }

    function keyForElement(elementId) {
        return String(Number(elementId))
    }

    function isImageSelected(elementId) {
        return Object.prototype.hasOwnProperty.call(
            selectedImagesById, keyForElement(elementId))
    }

    function isImageQueued(elementId) {
        return Object.prototype.hasOwnProperty.call(
            exportQueueById, keyForElement(elementId))
    }

    function hasMultiSelectModifier(modifiers) {
        return (modifiers & Qt.ShiftModifier) || (modifiers & Qt.ControlModifier)
    }

    function selectionItemForIndex(index) {
        if (index < 0 || index >= albumBackend.thumbnails.length) {
            return null
        }

        const row = albumBackend.thumbnails[index]
        if (!row) {
            return null
        }
        const elementId = Number(row.elementId)
        if (elementId <= 0) {
            return null
        }

        return {
            elementId: elementId,
            imageId: Number(row.imageId),
            fileName: row.fileName ? row.fileName : qsTr("(unnamed)")
        }
    }

    GridView {
        id: grid
        anchors.fill: parent
        model: albumBackend.thumbnails
        clip: true
        cacheBuffer: 0
        cellWidth: Math.max(72, Math.floor(width / root.columns))
        cellHeight: Math.max(64, Math.round((cellWidth - root.cardInset * 2) * 2 / 3
                                            + root.textAreaHeight
                                            + root.cardInset * 2
                                            + root.delegateGap))
        interactive: false
        onContentHeightChanged: root.clampContentY()
        onHeightChanged: root.clampContentY()
        onCellHeightChanged: root.clampContentY()

        delegate: Rectangle {
            id: cardDelegate
            required property int index
            required property int elementId
            required property int imageId
        required property string fileName
        required property string cameraModel
        required property int iso
        required property string aperture
        required property string captureDate
        required property int rating
        required property string accent
        required property string thumbUrl
        required property bool thumbLoading
        required property bool thumbMissingSource
        property string liveThumbUrl: thumbUrl
        onThumbUrlChanged: liveThumbUrl = thumbUrl
        property bool liveThumbLoading: thumbLoading
        onThumbLoadingChanged: liveThumbLoading = thumbLoading
        property bool liveThumbMissingSource: thumbMissingSource
        onThumbMissingSourceChanged: liveThumbMissingSource = thumbMissingSource
        property int pinnedElementId: 0
        property int pinnedImageId: 0
        property int pinnedMaxEdge: 0
        readonly property bool thumbnailReady: liveThumbUrl.length > 0
        readonly property bool thumbnailLoadingState: liveThumbLoading
        readonly property bool thumbnailMissingState: !thumbnailReady && !thumbnailLoadingState && liveThumbMissingSource
        readonly property bool thumbnailIdleState: !thumbnailReady && !thumbnailLoadingState && !thumbnailMissingState

        function releasePinnedThumbnail() {
            if (pinnedElementId !== 0 && pinnedImageId !== 0) {
                albumBackend.SetThumbnailVisible(pinnedElementId, pinnedImageId, false,
                                                 pinnedMaxEdge > 0 ? pinnedMaxEdge : root.desiredMaxEdge)
            }
            pinnedElementId = 0
            pinnedImageId = 0
            pinnedMaxEdge = 0
        }

        function bindThumbnailLifetime(force) {
            if (!force && pinnedElementId === elementId && pinnedImageId === imageId
                    && pinnedMaxEdge === root.desiredMaxEdge) {
                return
            }
            releasePinnedThumbnail()
            pinnedElementId = elementId
            pinnedImageId = imageId
            pinnedMaxEdge = root.desiredMaxEdge
            liveThumbUrl = thumbUrl
            liveThumbLoading = thumbLoading
            liveThumbMissingSource = thumbMissingSource
            if (pinnedElementId !== 0 && pinnedImageId !== 0) {
                albumBackend.SetThumbnailVisible(pinnedElementId, pinnedImageId, true, pinnedMaxEdge)
            }
        }

        Component.onCompleted: bindThumbnailLifetime(false)
        onElementIdChanged: bindThumbnailLifetime(false)
        onImageIdChanged: bindThumbnailLifetime(false)
        Component.onDestruction: releasePinnedThumbnail()

        Connections {
            target: root
            function onDesiredMaxEdgeChanged() {
                cardDelegate.bindThumbnailLifetime(true)
            }
        }

            readonly property bool isSelected: root.isImageSelected(elementId)
            readonly property bool isHovered: overlay.hoveredIndex === index

        width: grid.cellWidth - root.delegateGap
        height: grid.cellHeight - root.delegateGap
        radius: root.columns >= 11 ? 6 : appTheme.panelRadius
            color: isSelected ? root.cardBgSelected
                   : (isHovered ? root.cardBgHover : root.cardBg)
            border.width: isSelected ? 2 : 0
            border.color: root.cardAccent
        Behavior on color { ColorAnimation { duration: 120 } }
            Behavior on border.width { NumberAnimation { duration: 150 } }

        Connections {
            target: albumBackend
            ignoreUnknownSignals: true
            function onThumbnailUpdated(updatedElementId, updatedUrl, loading, missingSource) {
                if (updatedElementId === elementId) {
                    liveThumbUrl = updatedUrl
                    liveThumbLoading = loading
                    liveThumbMissingSource = missingSource
                }
            }
        }

            Item {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: root.cardInset
                height: Math.max(48, parent.height - root.textAreaHeight - root.cardInset * 2
                                 - (root.textAreaHeight > 0 ? 4 : 0))
                clip: true

                Rectangle {
                    anchors.fill: parent
                    radius: root.columns >= 11 ? 5 : 10
                    color: appTheme.bgBaseColor
                    border.width: root.columns >= 11 ? 1 : 2
                    border.color: appTheme.dividerColor
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    width: 28
                    height: 28
                    visible: thumbnailLoadingState
                    running: visible
                }
                Image {
                    id: thumbImage
                    anchors.centerIn: parent
                    width: parent.width - 4
                    height: parent.height - 4
                    source: liveThumbUrl
                    visible: false
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                }
                Rectangle {
                    id: thumbMask
                    anchors.fill: thumbImage
                    radius: root.columns >= 11 ? 4 : 8
                    visible: false
                    layer.enabled: true
                }
                MultiEffect {
                    anchors.fill: thumbImage
                    source: thumbImage
                    maskEnabled: true
                    maskSource: thumbMask
                    visible: thumbnailReady
                }
                Rectangle {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 8
                    width: 18
                    height: 18
                    radius: 9
                    visible: thumbnailMissingState
                    color: root.cardDangerTint
                    border.width: 1
                    border.color: root.cardDanger
                }
                Label {
                    anchors.centerIn: parent
                    visible: thumbnailMissingState
                    text: "!"
                    color: root.cardDanger
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: 30
                    font.weight: 700
                }
                HoverHandler {
                    id: thumbHover
                }
                ToolTip.visible: thumbnailMissingState && thumbHover.hovered
                ToolTip.text: qsTr("Source file was moved or deleted")
                ToolTip.delay: 150
        }

        Column {
            id: textColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: root.cardInset
            height: root.textAreaHeight
            visible: root.textAreaHeight > 0
            spacing: root.compactText ? 0 : 2
            Label {
                visible: !root.hideAllText
                text: fileName
                color: root.cardText
                font.family: appTheme.dataFontFamily
                font.pixelSize: root.titleFontSize
                font.weight: root.dataFontWeight
                font.letterSpacing: root.dataLetterSpacing
                elide: Text.ElideRight
                width: parent.width
                height: root.compactText ? root.textAreaHeight : implicitHeight
                verticalAlignment: Text.AlignVCenter
            }
            Label {
                visible: !root.hideMetadata
                text: qsTr("%1 | ISO %2 | f/%3").arg(cameraModel).arg(iso).arg(aperture)
                color: root.cardMuted
                font.family: appTheme.dataFontFamily
                font.pixelSize: root.metadataFontSize
                font.weight: root.dataFontWeight
                font.letterSpacing: root.dataLetterSpacing
                elide: Text.ElideRight
                width: parent.width
            }
            Label {
                visible: !root.hideMetadata
                text: qsTr("%1 | Rating %2/5").arg(captureDate).arg(rating)
                color: root.cardMuted
                font.family: appTheme.dataFontFamily
                font.pixelSize: root.metadataFontSize
                font.weight: root.dataFontWeight
                font.letterSpacing: root.dataLetterSpacing
                elide: Text.ElideRight
                width: parent.width
            }
        }

        }
    }

    // ── Interaction overlay ──
    MouseArea {
        id: overlay
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton

        property int hoveredIndex: -1
        property point dragStart: Qt.point(0, 0)
        property point dragCurrent: Qt.point(0, 0)
        property real dragStartContentY: 0
        property bool isDragging: false
        property var preDragSelection: ({})
        property bool dragAdditive: false

        cursorShape: hoveredIndex >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor

        function gridIndexAt(viewX, viewY) {
            return grid.indexAt(viewX + grid.contentX, viewY + grid.contentY)
        }

        function dragStartContentTop() {
            return dragStart.y + dragStartContentY
        }

        function dragCurrentContentTop() {
            return dragCurrent.y + grid.contentY
        }

        function rubberBandViewportY() {
            return Math.min(dragStartContentTop(), dragCurrentContentTop()) - grid.contentY
        }

        function rubberBandViewportHeight() {
            return Math.abs(dragCurrentContentTop() - dragStartContentTop())
        }

        function applyRubberBandSelection() {
            const bandItems = collectRubberBandItems()
            if (dragAdditive) {
                const merged = Object.values(preDragSelection).concat(bandItems)
                root.replaceSelection(merged)
            } else {
                root.replaceSelection(bandItems)
            }
        }

        function collectRubberBandItems() {
            const colCount = Math.max(1, Math.floor(grid.width / grid.cellWidth))
            const totalCount = grid.count

            const bLeft   = Math.min(dragStart.x, dragCurrent.x)
            const bRight  = Math.max(dragStart.x, dragCurrent.x)
            const bTop    = Math.min(dragStartContentTop(), dragCurrentContentTop())
            const bBottom = Math.max(dragStartContentTop(), dragCurrentContentTop())

            const minCol = Math.max(0, Math.floor(bLeft / grid.cellWidth))
            const maxCol = Math.min(colCount - 1, Math.floor(bRight / grid.cellWidth))
            const minRow = Math.max(0, Math.floor(bTop / grid.cellHeight))
            const maxRow = Math.floor(bBottom / grid.cellHeight)

            const items = []
            for (let row = minRow; row <= maxRow; ++row) {
                for (let col = minCol; col <= maxCol; ++col) {
                    const idx = row * colCount + col
                    if (idx >= 0 && idx < totalCount) {
                        const item = root.selectionItemForIndex(idx)
                        if (item) {
                            items.push(item)
                        }
                    }
                }
            }
            return items
        }

        onPositionChanged: function(mouse) {
            if (pressed && (pressedButtons & Qt.LeftButton)) {
                const dx = mouse.x - dragStart.x
                const dy = mouse.y - dragStart.y
                if (!isDragging && (dx * dx + dy * dy) > 64) {
                    isDragging = true
                    dragAdditive = root.hasMultiSelectModifier(mouse.modifiers)
                    if (dragAdditive) {
                        preDragSelection = Object.assign({}, root.selectedImagesById)
                    }
                }
                if (isDragging) {
                    dragCurrent = Qt.point(mouse.x, mouse.y)
                    applyRubberBandSelection()
                }
            }
            hoveredIndex = gridIndexAt(mouse.x, mouse.y)
        }

        onPressed: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                const idx = gridIndexAt(mouse.x, mouse.y)
                if (idx >= 0) {
                    const item = root.selectionItemForIndex(idx)
                    if (item) {
                        const scenePoint = overlay.mapToItem(null, mouse.x, mouse.y)
                        root.contextMenuRequested(item, scenePoint.x, scenePoint.y)
                    }
                }
                return
            }
            dragStart = Qt.point(mouse.x, mouse.y)
            dragCurrent = Qt.point(mouse.x, mouse.y)
            dragStartContentY = grid.contentY
            isDragging = false
            dragAdditive = false
            preDragSelection = ({})
        }

        onReleased: function(mouse) {
            if (mouse.button !== Qt.LeftButton) {
                isDragging = false
                return
            }
            if (!isDragging) {
                const idx = gridIndexAt(mouse.x, mouse.y)
                if (idx >= 0) {
                    const item = root.selectionItemForIndex(idx)
                    if (item) {
                        if (root.hasMultiSelectModifier(mouse.modifiers)) {
                            const next = !root.isImageSelected(item.elementId)
                            root.imageSelectionChanged(item.elementId, item.imageId, item.fileName, next)
                        } else {
                            root.replaceSelection([item])
                        }
                    }
                } else {
                    root.replaceSelection([])
                }
            }
            isDragging = false
        }

        onDoubleClicked: function(mouse) {
            const idx = gridIndexAt(mouse.x, mouse.y)
            if (idx >= 0) {
                const item = root.selectionItemForIndex(idx)
                if (item) {
                    albumBackend.OpenEditor(item.elementId, item.imageId)
                }
            }
        }

        onExited: hoveredIndex = -1

        onWheel: function(wheel) {
            // Ctrl+Scroll → zoom, otherwise → scroll
            if (wheel.modifiers & Qt.ControlModifier) {
                const delta = wheel.angleDelta.y > 0 ? 1 : -1
                root.setZoomLevelAt(root.zoomLevel + delta, wheel.y)
                wheel.accepted = true
                return
            }

            grid.contentY = Math.max(0, Math.min(
                root.maxContentY(),
                grid.contentY - wheel.angleDelta.y))
            if (isDragging) {
                applyRubberBandSelection()
            }
            hoveredIndex = gridIndexAt(mouseX, mouseY)
            wheel.accepted = true
        }
    }

    // ── Rubber band visual ──
    Rectangle {
        id: rubberBand
        visible: overlay.isDragging
        x: Math.min(overlay.dragStart.x, overlay.dragCurrent.x)
        y: overlay.rubberBandViewportY()
        width: Math.abs(overlay.dragCurrent.x - overlay.dragStart.x)
        height: overlay.rubberBandViewportHeight()
        color: Qt.rgba(appTheme.toneMist.r, appTheme.toneMist.g, appTheme.toneMist.b, 0.08)
        border.width: 1
        border.color: Qt.rgba(appTheme.toneMist.r, appTheme.toneMist.g, appTheme.toneMist.b, 0.50)
        radius: 2
        z: 10
    }
}
