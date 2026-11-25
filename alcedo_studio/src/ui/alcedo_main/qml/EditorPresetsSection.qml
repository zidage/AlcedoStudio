import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Alcedo.Main 1.0
import "FileDrop.js" as FileDrop

// The preset part of the Versions panel (edit presets plan 2.1 and 2.2): the header
// with Add and the menu, the error badge, the shared PresetBrowser, the tile menu,
// and the drop of .alcpreset files from the system (Add). PresetController owns the
// commands; the Versions panel owns the drag between this part and the Versions list.
ColumnLayout {
    id: root
    objectName: "editorPresetsSection"

    property var presets: null
    // PresetPreviewController: the tile previews on the open photo.
    property var presetPreview: null
    // An apply to the open photo is possible (a photo is open and Versions can change).
    property bool canApply: false
    // A Version drag is over this part and a drop here saves a preset.
    property bool versionDropHighlight: false
    // A Version drag runs (the drop guide shows its band), and the dragged name.
    property bool versionDragActive: false
    property string versionDragName: ""
    property string errorText: ""
    readonly property alias browser: presetBrowser
    readonly property real headerHeight: header.implicitHeight

    signal applyRequested(string presetId)
    signal applyFileRequested(var url)
    signal presetDragStarted(string presetId, string name, real sceneX, real sceneY)
    signal presetDragMoved(real sceneX, real sceneY)
    signal presetDragFinished(real sceneX, real sceneY)

    spacing: appTheme.spaceSm

    function presetFileUrls(dragEvent) {
        return FileDrop.localFileUrls(dragEvent).filter(function (url) {
            return url.toLowerCase().endsWith(".alcpreset")
        })
    }

    // A save by drag from this panel waits for its file; the new tile then enters rename.
    // A save from the Copy dialog reports through its own snackbar.
    property bool awaitingSave: false

    function report(result) {
        root.errorText = result && result.success !== true
                         ? String(result.message || qsTr("The preset operation failed")) : ""
    }

    function reportSave(result) {
        root.report(result)
        root.awaitingSave = !!result && result.success === true
    }

    function addFiles(urls) {
        if (root.presets)
            root.report(root.presets.AddFiles(urls))
    }

    function commitRename(presetId, name) {
        presetBrowser.renamePresetId = ""
        if (!root.presets || name.length === 0)
            return
        const current = String(presetBrowser.model.roleAt(presetBrowser.model.rowOfPreset(presetId),
                                                          PresetListModel.NameRole) || "")
        if (name === current)
            return
        root.report(root.presets.Rename(presetId, name))
    }

    component HeaderButton: IconActionButton {
        compact: true
        iconColorDefault: appTheme.textColor
        iconColorMuted: appTheme.textMutedColor
        fillIdle: appTheme.cardSurfaceColor
        fillHover: appTheme.buttonHoveredFillColor
        fillPressed: appTheme.buttonPressedFillColor
        fillSelected: appTheme.buttonSelectedFillColor
        focusRingColor: appTheme.textColor
    }

    // A save, add, or rename of the service ends: a new preset enters rename after a
    // save by drag, a renamed preset stays selected, and a failure shows its reason.
    Connections {
        target: root.presets
        ignoreUnknownSignals: true
        function onOperationFinished(result) {
            const operation = String(result.operation || "")
            if (operation === "scan" || operation === "useRoot")
                return
            if (operation === "save") {
                if (!root.awaitingSave)
                    return
                root.awaitingSave = false
            }
            root.report(result)
            const ids = result.presetIds || []
            if (result.success !== true || ids.length === 0)
                return
            presetBrowser.selectedPresetId = String(ids[0])
            if (operation === "save") {
                presetBrowser.renamePresetId = String(ids[0])
                Qt.callLater(presetBrowser.revealPreset, String(ids[0]))
            }
        }
    }

    RowLayout {
        id: header
        objectName: "editorPresetsHeader"
        Layout.fillWidth: true
        spacing: appTheme.spaceSm

        Label {
            Layout.fillWidth: true
            text: qsTr("Presets")
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightHeading
        }

        HeaderButton {
            objectName: "editorPresetsAddButton"
            enabled: !!root.presets
            iconSrc: "qrc:/panel_icons/plus.svg"
            actionName: qsTr("Add Preset...")
            onClicked: addDialog.open()
        }

        HeaderButton {
            id: menuButton
            objectName: "editorPresetsMenuButton"
            enabled: !!root.presets
            iconSrc: "qrc:/panel_icons/dots.svg"
            actionName: qsTr("Preset actions")
            onClicked: {
                const point = menuButton.mapToItem(null, 0, menuButton.height)
                headerMenu.parent = Overlay.overlay
                headerMenu.openAt(point.x, point.y)
            }
        }
    }

    AlertBadge {
        objectName: "editorPresetsError"
        Layout.fillWidth: true
        text: root.errorText
    }

    // The slot takes the height under the header, so the header stays at the top. The
    // splitter can shrink the part to its header; the browser shows when the slot has
    // room for it. The slot decides, not the browser: a layout gives no height to a
    // hidden item, so a browser hidden at height 0 never shows again.
    Item {
        id: browserSlot
        objectName: "editorPresetsBrowserSlot"
        Layout.fillWidth: true
        Layout.fillHeight: true

        PresetBrowser {
            id: presetBrowser
            anchors.fill: parent
            visible: browserSlot.height >= presetBrowser.toolbarHeight + appTheme.spaceXl
            model: root.presets ? root.presets.presets : null
            previewSource: root.presetPreview
            viewMode: root.presets ? String(root.presets.browserViewMode) : "grid"
            onViewModeRequested: function (mode) {
                if (root.presets)
                    root.presets.browserViewMode = mode
            }
            dropHighlight: root.versionDropHighlight || presetFileDrop.urls.length > 0
            onActivated: function (presetId) {
                if (root.canApply)
                    root.applyRequested(presetId)
            }
            onContextRequested: function (presetId, sceneX, sceneY) {
                tileMenu.presetId = presetId
                tileMenu.available = presetBrowser.roleAt(presetBrowser.model.rowOfPreset(presetId),
                                                          PresetListModel.AvailableRole) === true
                tileMenu.parent = Overlay.overlay
                tileMenu.openAt(sceneX, sceneY)
            }
            onPresetDragStarted: function (presetId, name, sceneX, sceneY) {
                root.presetDragStarted(presetId, name, sceneX, sceneY)
            }
            onPresetDragMoved: function (sceneX, sceneY) { root.presetDragMoved(sceneX, sceneY) }
            onPresetDragFinished: function (sceneX, sceneY) { root.presetDragFinished(sceneX, sceneY) }
            onRenameCommitted: function (presetId, name) { root.commitRename(presetId, name) }
            onRenameCanceled: presetBrowser.renamePresetId = ""

            // Drop of .alcpreset files from the system: Add (plan 2.2). The action is Copy, so
            // the file manager keeps the source files.
            DropArea {
                id: presetFileDrop
                objectName: "editorPresetsFileDropArea"
                anchors.fill: parent
                keys: ["text/uri-list"]
                property var urls: []
                onEntered: function (drag) {
                    urls = root.presets ? root.presetFileUrls(drag) : []
                    if (urls.length > 0)
                        drag.accept(Qt.CopyAction)
                    else
                        drag.accepted = false
                }
                onExited: urls = []
                onDropped: function (drop) {
                    const dropped = urls.length > 0 ? urls : root.presetFileUrls(drop)
                    urls = []
                    if (dropped.length === 0) {
                        drop.accepted = false
                        return
                    }
                    drop.accept(Qt.CopyAction)
                    root.addFiles(dropped)
                }
            }

            // A Version drag: the band says that a drop here saves a preset.
            DropGuideOverlay {
                objectName: "editorPresetsDropGuide"
                parent: presetBrowser.well
                anchors.fill: parent
                shown: root.versionDragActive
                expanded: root.versionDropHighlight
                bandText: qsTr("Drag here to save as a preset")
                expandedText: qsTr("Release to save as a preset")
                detailText: root.versionDragName
            }
        }
    }

    AppContextMenu {
        id: headerMenu
        objectName: "editorPresetsHeaderMenu"
        AppMenuItem {
            objectName: "editorPresetsApplyFileItem"
            text: qsTr("Apply Preset File...")
            enabled: root.canApply
            onTriggered: applyFileDialog.open()
        }
        AppMenuItem {
            objectName: "editorPresetsRefreshItem"
            text: qsTr("Refresh")
            onTriggered: root.presets.Refresh()
        }
        AppMenuItem {
            objectName: "editorPresetsShowFolderItem"
            text: qsTr("Show Folder")
            onTriggered: root.report(root.presets.ShowFolder())
        }
    }

    AppContextMenu {
        id: tileMenu
        objectName: "editorPresetTileMenu"
        property string presetId: ""
        property bool available: false
        // Rename starts when the menu has closed. On close the menu gives the focus back
        // to the item that had it before, and its window gives the activation back to the
        // main window; a rename field that already had the focus would lose it and commit.
        property string renameAfterClose: ""
        onClosed: {
            const presetId = renameAfterClose
            renameAfterClose = ""
            if (presetId.length > 0)
                presetBrowser.renamePresetId = presetId
        }
        AppMenuItem {
            objectName: "editorPresetTileApplyItem"
            text: qsTr("Apply")
            enabled: tileMenu.available && root.canApply
            onTriggered: root.applyRequested(tileMenu.presetId)
        }
        AppMenuItem {
            objectName: "editorPresetTileRenameItem"
            text: qsTr("Rename")
            onTriggered: tileMenu.renameAfterClose = tileMenu.presetId
        }
        AppMenuItem {
            objectName: "editorPresetTileShowItem"
            text: qsTr("Show in Folder")
            onTriggered: root.report(root.presets.ShowInFolder(tileMenu.presetId))
        }
        AppMenuItem {
            objectName: "editorPresetTileDeleteItem"
            text: qsTr("Delete")
            onTriggered: root.report(root.presets.Delete(tileMenu.presetId))
        }
    }

    FileDialog {
        id: addDialog
        title: qsTr("Add Preset")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Alcedo presets (*.alcpreset)"), qsTr("All Files (*)")]
        onAccepted: {
            const files = []
            for (let i = 0; i < selectedFiles.length; ++i)
                files.push(selectedFiles[i].toString())
            root.addFiles(files)
        }
    }

    // One-time apply (R8): the file makes a Version and does not enter the folder.
    FileDialog {
        id: applyFileDialog
        title: qsTr("Apply Preset File")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Alcedo presets (*.alcpreset)"), qsTr("All Files (*)")]
        onAccepted: root.applyFileRequested(selectedFile.toString())
    }
}
