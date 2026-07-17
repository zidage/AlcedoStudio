import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Effects

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
    readonly property string dataFontFamily: theme ? theme.dataFontFamily : appTheme.dataFontFamily
    readonly property string headlineFontFamily: theme ? theme.headlineFontFamily : appTheme.headlineFontFamily
    readonly property int controlRadius: theme ? theme.controlRadius : 10

    function secondaryButtonFill(enabled, hovered, pressed) {
        if (theme && theme.secondaryButtonFill) {
            return theme.secondaryButtonFill(enabled, hovered, pressed)
        }
        return colButtonSecondary
    }

    property bool inspectorVisible: true
    property real inspectorWidth: 300
    readonly property real inspectorMinWidth: 300
    readonly property real inspectorMaxWidth: 600
    readonly property real leftPaneWidth: 276
    readonly property real centerPaneMinWidth: 560
    readonly property real mainFrameHorizontalMargins: 24
    readonly property real contentRowSpacingTotal: 36
    readonly property real inspectorAdaptiveMaxWidth: Math.max(
        0,
        root.width
        - leftPaneWidth
        - centerPaneMinWidth
        - mainFrameHorizontalMargins
        - contentRowSpacingTotal
        - 5)
    property bool gridMode: true
    readonly property int defaultGridZoomLevel: 4
    property int gridZoomLevel: defaultGridZoomLevel

RowLayout {
    anchors.fill: parent
    spacing: 12

    CollectionsPanel {
        objectName: "collectionsPanel"
        Layout.preferredWidth: root.leftPaneWidth
        Layout.minimumWidth: root.leftPaneWidth
        Layout.maximumWidth: root.leftPaneWidth
        Layout.fillHeight: true
        folderController: appModules.folders
        theme: root
        backendInteractive: host.backendInteractive
        selectedCount: host.selectedCount
        onImportRequested: host.importDialog.open()
        onSearchRequested: host.globalSearchDialog.openFromCollection()
        onAdvancedAnalysisRequested: host.openAdvancedAnalysisDialog()
    }

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
                Item {
                    Layout.preferredWidth: 180
                    Layout.preferredHeight: 36

                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onDoubleTapped: root.gridZoomLevel = root.defaultGridZoomLevel
                    }

                    RowLayout {
                        anchors.fill: parent
                        spacing: 8

                        Label {
                            text: "-"
                            color: root.colTextMuted
                            font.family: root.dataFontFamily
                            font.pixelSize: 18
                            font.weight: 600
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.gridZoomLevel = Math.min(7, root.gridZoomLevel + 1)
                            }
                        }

                        Slider {
                            id: zoomSlider
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            from: 0
                            to: 7
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
                                    color: root.colAccentPrimary
                                    radius: 2
                                }
                            }
                            handle: Rectangle {
                                x: zoomSlider.leftPadding + zoomSlider.visualPosition * (zoomSlider.availableWidth - width)
                                y: zoomSlider.topPadding + zoomSlider.availableHeight / 2 - height / 2
                                implicitWidth: 14
                                implicitHeight: 14
                                radius: 7
                                color: root.colAccentPrimary
                                border.width: 1
                                border.color: root.colAccentSecondary
                            }
                        }

                        Label {
                            text: "+"
                            color: root.colText
                            font.family: root.dataFontFamily
                            font.pixelSize: 18
                            font.weight: 600
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.gridZoomLevel = Math.max(0, root.gridZoomLevel - 1)
                            }
                        }
                    }
                }

                Item { Layout.preferredWidth: 10 }
                Item {
                    id: viewModeSwitch
                    Layout.preferredWidth: 132
                    Layout.preferredHeight: 36

                    Rectangle {
                        id: viewModeTrack
                        anchors.fill: parent
                        radius: height / 2
                        color: Qt.rgba(root.colBgBase.r, root.colBgBase.g, root.colBgBase.b, 0.98)
                        border.width: 1
                        border.color: root.colDivider
                    }

                    Rectangle {
                        id: viewModeThumb
                        width: parent.width / 2 - 4
                        height: parent.height - 4
                        y: 2
                        x: root.gridMode ? 2 : parent.width - width - 2
                        radius: height / 2
                        color: root.colAccentPrimary
                        border.width: 1
                        border.color: Qt.rgba(
                            root.colAccentSecondary.r,
                            root.colAccentSecondary.g,
                            root.colAccentSecondary.b,
                            0.52)
                        Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    }

                    Row {
                        anchors.fill: parent
                        spacing: 0

                        Item {
                            width: parent.width / 2
                            height: parent.height

                            Image {
                                id: gridModeIconSource
                                anchors.centerIn: parent
                                width: 20
                                height: 20
                                source: "qrc:/panel_icons/layout-grid.svg"
                                visible: false
                                asynchronous: true
                            }

                            MultiEffect {
                                anchors.fill: gridModeIconSource
                                source: gridModeIconSource
                                colorization: 1.0
                                colorizationColor: "white"
                            }

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.gridMode = true
                            }
                        }

                        Item {
                            width: parent.width / 2
                            height: parent.height

                            Image {
                                id: listModeIconSource
                                anchors.centerIn: parent
                                width: 20
                                height: 20
                                source: "qrc:/panel_icons/list.svg"
                                visible: false
                                asynchronous: true
                            }

                            MultiEffect {
                                anchors.fill: listModeIconSource
                                source: listModeIconSource
                                colorization: 1.0
                                colorizationColor: "white"
                            }

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.gridMode = false
                            }
                        }
                    }
                }

                Item { Layout.preferredWidth: 8 }

                Button {
                    id: inspectorToggleButton
                    objectName: "libraryInspectorToggle"
                    checkable: false
                    flat: true
                    Layout.preferredWidth: 42
                    Layout.preferredHeight: 36
                    display: AbstractButton.IconOnly
                    activeFocusOnTab: true
                    property real iconRotationTarget: root.inspectorVisible ? 180 : 0
                    icon.source: "qrc:/panel_icons/inspector-expand.svg"
                    icon.width: 20
                    icon.height: 20
                    icon.color: root.inspectorVisible
                                ? root.colAccentPrimary
                                : (inspectorToggleButton.hovered ? root.colText : root.colTextMuted)
                    Material.foreground: icon.color
                    ToolTip.visible: hovered
                    ToolTip.text: root.inspectorVisible ? qsTr("Collapse Inspector") : qsTr("Expand Inspector")
                    Accessible.name: ToolTip.text
                    background: Rectangle {
                        radius: root.controlRadius
                        color: "transparent"
                        border.width: 0
                    }
                    onContentItemChanged: {
                        inspectorIconRotate.target = contentItem
                        if (contentItem) {
                            contentItem.transformOrigin = Item.Center
                            contentItem.rotation = iconRotationTarget
                        }
                    }
                    onIconRotationTargetChanged: {
                        if (contentItem) {
                            inspectorIconRotate.stop()
                            inspectorIconRotate.to = iconRotationTarget
                            inspectorIconRotate.start()
                        }
                    }
                    Component.onCompleted: {
                        if (contentItem) {
                            contentItem.transformOrigin = Item.Center
                            contentItem.rotation = iconRotationTarget
                        }
                    }
                    NumberAnimation {
                        id: inspectorIconRotate
                        property: "rotation"
                        duration: 170
                        easing.type: Easing.OutCubic
                    }
                    onClicked: root.inspectorVisible = !root.inspectorVisible
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Loader {
                    anchors.fill: parent
                    active: appModules.library.shownCount > 0
                    sourceComponent: root.gridMode ? gridComp : listComp
                }

                Column {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    visible: appModules.library.shownCount === 0
                    spacing: 8
                    Label {
                        text: appModules.project.serviceReady ? qsTr("No Photos Yet") : qsTr("Open or Create a Project")
                        font.family: root.headlineFontFamily
                        color: root.colText
                        font.pixelSize: 22
                        font.weight: 700
                    }
                    Label {
                        text: appModules.project.serviceReady
                              ? qsTr("Import your images for RAW adjustments.")
                              : qsTr("Use File > Load Project or File > Create Project to choose .alcd files.")
                        color: root.colTextMuted
                        font.pixelSize: 12
                    }
                    Button {
                        id: emptyStateLoadButton
                        visible: !appModules.project.serviceReady
                        text: qsTr("Load Project")
                        Material.background: root.colButtonPrimary
                        Material.foreground: root.colText
                        onClicked: host.beginProjectLaunch(function() {
                            return appModules.project.PromptAndLoadProject()
                        })
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

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: root.panelRadius
                color: root.colBgPanel
                border.width: 0
                clip: true

                InspectorPanel {
                    anchors.fill: parent
                    anchors.margins: 10
                    focusedImage: host.focusedImageInspection
                    interactionPolicy: appModules.interactionPolicy
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
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                spacing: 10

                Button {
                    id: addSelectedBtn
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    text: qsTr("Add to Queue") + " (" + host.selectedCount + ")"
                    enabled: host.backendInteractive && host.selectedCount > 0
                    icon.source: "qrc:/panel_icons/queue-add.svg"
                    icon.width: 16
                    icon.height: 16
                    icon.color: root.colText
                    display: AbstractButton.TextBesideIcon
                    background: Rectangle {
                        radius: root.controlRadius
                        color: root.secondaryButtonFill(
                            addSelectedBtn.enabled,
                            addSelectedBtn.hovered,
                            addSelectedBtn.down)
                        border.width: 1
                        border.color: root.colButtonSecondaryBorder
                    }
                    Material.foreground: root.colText
                    scale: addSelectedBtn.hovered && enabled ? 1.03 : 1.0
                    Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                    onClicked: {
                        host.exportQueueState.addTargets(host.selectionState.currentSelectedItems())
                        host.selectionState.clearSelectedImages()
                    }
                }

                Button {
                    id: exportQueueBtn
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    text: qsTr("Export") + " (" + host.exportQueueCount + ")"
                    enabled: host.backendInteractive && (appModules.library.shownCount > 0 || host.exportQueueCount > 0)
                    icon.source: "qrc:/panel_icons/export.svg"
                    icon.width: 16
                    icon.height: 16
                    icon.color: root.colText
                    display: AbstractButton.TextBesideIcon
                    background: Canvas {
                        opacity: exportQueueBtn.enabled ? 1.0 : 0.5
                        property color gradStart: root.colAccentPrimary
                        property color gradEnd: root.colAccentSecondary
                        onGradStartChanged: requestPaint()
                        onGradEndChanged: requestPaint()
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            var r = 8
                            ctx.beginPath()
                            ctx.moveTo(r, 0)
                            ctx.lineTo(width - r, 0)
                            ctx.quadraticCurveTo(width, 0, width, r)
                            ctx.lineTo(width, height - r)
                            ctx.quadraticCurveTo(width, height, width - r, height)
                            ctx.lineTo(r, height)
                            ctx.quadraticCurveTo(0, height, 0, height - r)
                            ctx.lineTo(0, r)
                            ctx.quadraticCurveTo(0, 0, r, 0)
                            ctx.closePath()
                            var grad = ctx.createLinearGradient(0, height, width, 0)
                            grad.addColorStop(0.0, Qt.rgba(gradStart.r, gradStart.g, gradStart.b, 1.0))
                            grad.addColorStop(1.0, Qt.rgba(gradEnd.r, gradEnd.g, gradEnd.b, 1.0))
                            ctx.fillStyle = grad
                            ctx.fill()
                        }
                    }
                    Material.foreground: root.colText
                    scale: exportQueueBtn.hovered && enabled ? 1.03 : 1.0
                    Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
                    onClicked: {
                        host.exportQueueState.refreshExportPreview()
                        host.exportDialog.open()
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
            color: dragArea.containsMouse || dragArea.drag.active ? root.colAccentPrimary : "transparent"
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
        id: gridComp
        ThumbnailGridView {
            zoomLevel: root.gridZoomLevel
            zoomAdjusting: zoomSlider.pressed
            onZoomLevelChanged: root.gridZoomLevel = zoomLevel
            selectedImagesById: host.selectedImagesById
            exportQueueById: host.exportQueueById
            onImageSelectionChanged: function(elementId, imageId, fileName, isHdr, selected) {
                host.selectionState.setImageSelected(elementId, imageId, fileName, isHdr, selected)
            }
            onReplaceSelection: function(items) {
                host.selectionState.replaceSelectedImages(items)
                if (items && items.length > 0) {
                    host.setFocusedImage(items[0])
                } else {
                    host.setFocusedImage(null)
                }
            }
            onImageFocused: function(item) {
                host.setFocusedImage(item)
            }
            onContextMenuRequested: function(item, sceneX, sceneY) {
                host.openImageContextMenu(item, sceneX, sceneY)
            }
        }
    }

    Component {
        id: listComp
        ThumbnailListView {
            selectedImagesById: host.selectedImagesById
            exportQueueById: host.exportQueueById
            onImageSelectionChanged: function(elementId, imageId, fileName, isHdr, selected) {
                host.selectionState.setImageSelected(elementId, imageId, fileName, isHdr, selected)
            }
            onReplaceSelection: function(items) {
                host.selectionState.replaceSelectedImages(items)
                if (items && items.length > 0) {
                    host.setFocusedImage(items[0])
                } else {
                    host.setFocusedImage(null)
                }
            }
            onImageFocused: function(item) {
                host.setFocusedImage(item)
            }
            onContextMenuRequested: function(item, sceneX, sceneY) {
                host.openImageContextMenu(item, sceneX, sceneY)
            }
        }
    }
}
