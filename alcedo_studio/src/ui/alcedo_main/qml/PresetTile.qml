import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Effects
import QtQuick.Layouts

// One preset of the preset browser (edit presets plan 2.1): an image area and the
// name below it. The image area shows the preview of the preset on the open photo
// (plan 2.4): the tile watches its preset on previewSource while it exists, so only
// instantiated tiles render a preview. The area stays neutral until the preview is
// ready. A preset that is not available, and a preset whose preview failed, show the
// warning icon; the tooltip of an unavailable preset gives the reason. Names and
// tooltips are plain text (plan 3.5).
//
// Input: click selects, double click and Enter apply, right click opens the menu of
// the browser. A press that moves more than the system drag distance starts the
// preset drag of the Versions panel; the browser forwards it in scene coordinates.
Item {
    id: tile
    objectName: "presetTile"

    property string presetId: ""
    property string name: ""
    property bool available: true
    // PresetPreviewController: watch, unwatch, previewUrl, previewFailed, revision.
    property var previewSource: null
    property bool selected: false
    property bool renaming: false
    // Plain-text summary of the package; read by the browser when the tooltip opens.
    property string summary: ""

    signal clicked()
    signal activated()
    signal contextRequested(real sceneX, real sceneY)
    signal summaryRequested()
    signal dragStarted(real sceneX, real sceneY)
    signal dragMoved(real sceneX, real sceneY)
    signal dragFinished(real sceneX, real sceneY)
    signal renameCommitted(string text)
    signal renameCanceled()

    readonly property bool hovered: tileMouse.containsMouse
    readonly property real imageAspect: 1.5

    // Preview: the preset this tile watches on the preview controller.
    readonly property string previewPresetId: available && !!previewSource ? presetId : ""
    property string watchedPresetId: ""
    readonly property string previewUrl: watchedPresetId.length > 0 && !!previewSource
                                         ? String(previewSource.previewUrl(watchedPresetId,
                                                                           previewSource.revision) || "")
                                         : ""
    readonly property bool previewFailed: watchedPresetId.length > 0 && !!previewSource
                                          && previewSource.previewFailed(watchedPresetId,
                                                                         previewSource.revision) === true
    readonly property bool previewPainted: previewUrl.length > 0
                                           && previewImage.status === Image.Ready
                                           && previewImage.implicitWidth > 0
                                           && previewImage.implicitHeight > 0

    function syncPreviewWatch() {
        if (previewPresetId === watchedPresetId)
            return
        if (watchedPresetId.length > 0 && previewSource)
            previewSource.unwatch(watchedPresetId)
        watchedPresetId = previewPresetId
        if (watchedPresetId.length > 0)
            previewSource.watch(watchedPresetId)
    }
    onPreviewPresetIdChanged: syncPreviewWatch()
    Component.onCompleted: syncPreviewWatch()
    Component.onDestruction: {
        if (watchedPresetId.length > 0 && previewSource)
            previewSource.unwatch(watchedPresetId)
    }

    implicitHeight: tileColumn.implicitHeight + appTheme.spaceSm * 2
    Accessible.role: Accessible.ListItem
    Accessible.name: tile.name
    Accessible.description: tile.available ? "" : tile.summary

    Rectangle {
        objectName: "presetTileChrome"
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        color: tile.hovered ? appTheme.buttonHoveredFillColor : "transparent"
        border.width: tile.selected ? appTheme.graphSelectionOutlineWidth : 0
        border.color: appTheme.graphSelectionOutlineColor
    }

    ColumnLayout {
        id: tileColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: appTheme.spaceSm
        spacing: appTheme.spaceXs

        Rectangle {
            id: imageArea
            objectName: "presetTileImageArea"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(width / tile.imageAspect)
            radius: appTheme.controlRadiusSmall
            color: appTheme.cardSurfaceColor

            // The photo fits into the 3:2 area, as in the LUT browser.
            Item {
                id: previewPhoto
                objectName: "presetTilePreview"
                readonly property real fitScale: tile.previewPainted
                                                 ? Math.min(imageArea.width / previewImage.implicitWidth,
                                                            imageArea.height / previewImage.implicitHeight)
                                                 : 1
                width: tile.previewPainted ? Math.round(previewImage.implicitWidth * fitScale)
                                           : imageArea.width
                height: tile.previewPainted ? Math.round(previewImage.implicitHeight * fitScale)
                                            : imageArea.height
                x: Math.round((imageArea.width - width) / 2)
                y: Math.round((imageArea.height - height) / 2)
                visible: tile.previewUrl.length > 0

                Rectangle {
                    id: previewMask
                    anchors.fill: parent
                    radius: appTheme.controlRadiusSmall
                    visible: false
                    layer.enabled: true
                }

                Image {
                    id: previewImage
                    objectName: "presetTilePreviewImage"
                    anchors.fill: parent
                    source: tile.previewUrl
                    asynchronous: true
                    // Every shown preview has its own URL; the controller keeps the pixels.
                    cache: false
                    fillMode: Image.PreserveAspectFit
                    opacity: tile.previewPainted ? 1 : 0
                    layer.enabled: tile.previewPainted
                    layer.effect: MultiEffect {
                        maskEnabled: true
                        maskSource: previewMask
                    }
                }
            }

            ColorImage {
                objectName: "presetTileUnavailableIcon"
                anchors.centerIn: parent
                width: appTheme.editorLutTileIconSize
                height: appTheme.editorLutTileIconSize
                visible: !tile.available || (tile.previewFailed && !tile.previewPainted)
                source: "qrc:/panel_icons/info.svg"
                sourceSize.width: appTheme.editorLutTileIconSize * 2
                sourceSize.height: appTheme.editorLutTileIconSize * 2
                color: appTheme.alertBadgeFillColor
            }
        }

        Label {
            objectName: "presetTileName"
            Layout.fillWidth: true
            visible: !tile.renaming
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            textFormat: Text.PlainText
            text: tile.name
            color: tile.available ? appTheme.textColor : appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: tile.selected ? appTheme.fontWeightStrong : appTheme.fontWeightRegular
        }

        // Inline rename: Enter commits, Escape cancels, focus loss commits.
        TextField {
            id: renameField
            objectName: "presetTileRenameField"
            Layout.fillWidth: true
            visible: tile.renaming
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            selectByMouse: true
            leftPadding: appTheme.spaceXs
            rightPadding: appTheme.spaceXs
            topPadding: 0
            bottomPadding: 0
            selectionColor: appTheme.editorListSelectedFillColor
            selectedTextColor: appTheme.editorListSelectedInkColor
            Accessible.name: qsTr("Rename Preset")
            background: Rectangle {
                radius: appTheme.controlRadiusSmall
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: renameField.activeFocus ? appTheme.textColor : appTheme.cardBorderColor
            }

            property bool finished: false
            function finish(commit) {
                if (finished || !tile.renaming)
                    return
                finished = true
                if (commit)
                    tile.renameCommitted(text.trim())
                else
                    tile.renameCanceled()
            }
            onVisibleChanged: {
                if (!visible)
                    return
                finished = false
                text = tile.name
                Qt.callLater(function () {
                    renameField.forceActiveFocus()
                    renameField.selectAll()
                })
            }
            // The field keeps Enter. TextInput passes a handled Return on to its parents,
            // and the browser list applies the selected preset on Enter.
            Keys.onReturnPressed: function (event) {
                finish(true)
                event.accepted = true
            }
            Keys.onEnterPressed: function (event) {
                finish(true)
                event.accepted = true
            }
            Keys.onEscapePressed: function (event) {
                finish(false)
                event.accepted = true
            }
            onEditingFinished: finish(true)
        }
    }

    MouseArea {
        id: tileMouse
        anchors.fill: parent
        enabled: !tile.renaming
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        // A drag of the tile moves the preset; the list must not flick instead.
        preventStealing: true
        property point pressPoint: Qt.point(0, 0)
        property bool dragging: false

        onPressed: function (mouse) {
            pressPoint = Qt.point(mouse.x, mouse.y)
            dragging = false
            if (mouse.button === Qt.RightButton) {
                const scene = mapToItem(null, mouse.x, mouse.y)
                tile.contextRequested(scene.x, scene.y)
            }
        }
        onPositionChanged: function (mouse) {
            if (!(mouse.buttons & Qt.LeftButton) || !tile.available)
                return
            const scene = mapToItem(null, mouse.x, mouse.y)
            if (!dragging) {
                const dx = mouse.x - pressPoint.x
                const dy = mouse.y - pressPoint.y
                if (Math.sqrt(dx * dx + dy * dy) < Qt.styleHints.startDragDistance)
                    return
                dragging = true
                tile.dragStarted(scene.x, scene.y)
            }
            tile.dragMoved(scene.x, scene.y)
        }
        onReleased: function (mouse) {
            if (!dragging)
                return
            dragging = false
            const scene = mapToItem(null, mouse.x, mouse.y)
            tile.dragFinished(scene.x, scene.y)
        }
        onCanceled: dragging = false
        onClicked: function (mouse) {
            if (mouse.button === Qt.LeftButton)
                tile.clicked()
        }
        onDoubleClicked: function (mouse) {
            if (mouse.button === Qt.LeftButton && tile.available)
                tile.activated()
        }
        onContainsMouseChanged: {
            if (containsMouse)
                tile.summaryRequested()
        }
    }

    // Its own tooltip, not the shared attached one: the summary comes from a file, so the
    // content item shows it as plain text.
    ToolTip {
        objectName: "presetTileToolTip"
        visible: tile.hovered && !tileMouse.dragging && tile.summary.length > 0
        delay: 1000
        timeout: 8000
        contentItem: Label {
            text: tile.summary
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }
    }
}
