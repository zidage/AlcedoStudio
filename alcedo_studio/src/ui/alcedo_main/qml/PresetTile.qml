import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One preset of the preset browser (edit presets plan 2.1): an image area and the
// name below it, or in the list layout a small image area left of the name. The
// image area shows the preview of the preset on the open photo (plan 2.4): the tile
// watches its preset on previewSource while it exists, so only instantiated tiles
// render a preview. The area stays neutral until the preview is
// ready. A preset that is not available, and a preset whose preview failed, show the
// warning icon. Only an unavailable preset has a tooltip, with the reason; an available
// preset shows no item summary (issue #1124). Names and tooltips are plain text (plan 3.5).
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
    // List layout: a compact row with a small image area left of the name.
    property bool listMode: false
    // Why the preset is not available; empty for an available preset.
    property string reason: ""

    signal clicked()
    signal activated()
    signal contextRequested(real sceneX, real sceneY)
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

    implicitHeight: tileColumn.implicitHeight
                    + (listMode ? appTheme.spaceSm + appTheme.spaceXs : appTheme.spaceSm * 2)
    Accessible.role: Accessible.ListItem
    Accessible.name: tile.name
    Accessible.description: tile.reason

    Rectangle {
        objectName: "presetTileChrome"
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        color: tile.hovered ? appTheme.buttonHoveredFillColor : "transparent"
        border.width: tile.selected ? appTheme.graphSelectionOutlineWidth : 0
        border.color: appTheme.graphSelectionOutlineColor
    }

    // Grid: the image area above the name. List: a small image area left of it.
    GridLayout {
        id: tileColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: tile.listMode ? undefined : parent.top
        anchors.verticalCenter: tile.listMode ? parent.verticalCenter : undefined
        anchors.margins: appTheme.spaceSm
        anchors.topMargin: tile.listMode ? 0 : appTheme.spaceSm
        columns: tile.listMode ? 2 : 1
        columnSpacing: appTheme.spaceSm
        rowSpacing: appTheme.spaceXs

        // The photo fits into the 3:2 area, as in the LUT browser. A preset that is not
        // available, and a failed preview, show the warning icon.
        BrowserPreviewThumb {
            id: imageArea
            objectName: "presetTileImageArea"
            namePrefix: "presetTile"
            readonly property real slotWidth: tile.listMode ? appTheme.editorBrowserListThumbWidth
                                                            : width
            Layout.fillWidth: !tile.listMode
            Layout.preferredWidth: tile.listMode ? appTheme.editorBrowserListThumbWidth : -1
            Layout.preferredHeight: Math.round(slotWidth / tile.imageAspect)
            Layout.alignment: Qt.AlignVCenter
            color: appTheme.cardSurfaceColor
            previewUrl: tile.previewUrl
            placeholderSource: !tile.available || tile.previewFailed ? "qrc:/panel_icons/info.svg" : ""
            placeholderColor: appTheme.alertBadgeFillColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 0

            Label {
                objectName: "presetTileName"
                Layout.fillWidth: true
                visible: !tile.renaming
                horizontalAlignment: tile.listMode ? Text.AlignLeft : Text.AlignHCenter
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                textFormat: Text.PlainText
                text: tile.name
                color: tile.available ? appTheme.textColor : appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: tile.listMode ? appTheme.fontSizeBody : appTheme.fontSizeCaption
                font.weight: tile.selected ? appTheme.fontWeightStrong : appTheme.fontWeightRegular
            }

            // Inline rename: Enter commits, Escape cancels, focus loss in the active window
            // commits.
            TextField {
                id: renameField
                objectName: "presetTileRenameField"
                Layout.fillWidth: true
                visible: tile.renaming
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: tile.listMode ? appTheme.fontSizeBody : appTheme.fontSizeCaption
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
                function takeFocus() {
                    renameField.forceActiveFocus()
                    renameField.selectAll()
                }
                onVisibleChanged: {
                    if (!visible)
                        return
                    finished = false
                    text = tile.name
                    Qt.callLater(renameField.takeFocus)
                }
                // A window that is activated again (after a menu or another application)
                // gives the focus back to an open rename.
                readonly property bool windowActive: Window.active
                onWindowActiveChanged: {
                    if (windowActive && visible && !finished && !activeFocus)
                        takeFocus()
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
                // A focus loss commits when the focus has settled on another item of the
                // active window. A focus loss because the window went inactive keeps the
                // rename open, and so does a focus that comes back at once.
                onEditingFinished: Qt.callLater(function () {
                    if (!renameField.activeFocus && renameField.windowActive)
                        renameField.finish(true)
                })
            }
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
    }

    // Its own tooltip, not the shared attached one: the reason names a file, so the content
    // item shows it as plain text.
    ToolTip {
        objectName: "presetTileToolTip"
        visible: tile.hovered && !tileMouse.dragging && tile.reason.length > 0
        delay: 1000
        timeout: 8000
        contentItem: Label {
            text: tile.reason
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }
    }
}
