import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import Alcedo.Main 1.0

// Preset browser (edit presets plan 2.1, step PS2.3): the presets of the preset
// folder as rows of tiles, with a title row for each group (a direct subfolder).
// The Versions panel uses it now; the Apply Preset dialog of PS4 uses the same
// component. PresetListModel owns the rows; tiles read roles by row and keep no
// copies. A selection by click changes selectedPresetId only: the rows are not
// rebuilt and the list does not scroll.
//
// previewSource is the preview controller of the photo of the preview images
// (PresetPreviewController, plan 2.4); each tile watches its preset on it. An empty
// preset folder shows the drop icon only.
Item {
    id: root
    objectName: "presetBrowser"

    property var model: null
    property string selectedPresetId: ""
    property var previewSource: null
    // The preset whose tile shows the inline rename field; empty for none.
    property string renamePresetId: ""
    // A drag that this browser accepts is over it.
    property bool dropHighlight: false

    signal activated(string presetId)
    signal contextRequested(string presetId, real sceneX, real sceneY)
    signal presetDragStarted(string presetId, string name, real sceneX, real sceneY)
    signal presetDragMoved(real sceneX, real sceneY)
    signal presetDragFinished(real sceneX, real sceneY)
    signal renameCommitted(string presetId, string name)
    signal renameCanceled()

    readonly property int count: model ? Number(model.count) : 0
    readonly property int tileGap: appTheme.spaceXs
    readonly property int columns: Math.max(1, Math.floor((rowsView.width + tileGap)
                                                          / (appTheme.editorPresetTileMinWidth + tileGap)))
    readonly property real tileWidth: Math.max(0, (rowsView.width - (columns - 1) * tileGap) / columns)
    // Bumped on each model change so tile bindings read their roles again.
    property int dataRevision: 0
    // Display rows: {kind: "title", text} or {kind: "tiles", first, count}. Built on a
    // model or column change only.
    property var displayRows: []
    // Summary of the hovered tile, read from the model when the pointer enters it.
    property string hoverSummaryId: ""
    property string hoverSummary: ""

    function roleAt(row, role) {
        const _revision = root.dataRevision
        if (!root.model || row < 0 || row >= root.count)
            return undefined
        return root.model.roleAt(row, role)
    }

    function rebuildRows() {
        const rows = []
        // Read the count from the model: modelReset arrives before the count notification.
        const total = root.model ? Number(root.model.count) : 0
        let first = 0
        while (first < total) {
            const group = String(root.model.roleAt(first, PresetListModel.GroupRole) || "")
            let end = first
            while (end < total
                   && String(root.model.roleAt(end, PresetListModel.GroupRole) || "") === group)
                ++end
            if (group.length > 0)
                rows.push({ kind: "title", text: group })
            for (let start = first; start < end; start += root.columns)
                rows.push({ kind: "tiles", first: start, count: Math.min(root.columns, end - start) })
            first = end
        }
        root.displayRows = rows
    }

    // Scroll the tile of @p presetId into view (a new preset that enters rename).
    function revealPreset(presetId) {
        if (!root.model)
            return
        const row = Number(root.model.rowOfPreset(presetId))
        for (let i = 0; i < root.displayRows.length; ++i) {
            const entry = root.displayRows[i]
            if (entry.kind === "tiles" && row >= entry.first && row < entry.first + entry.count) {
                rowsView.positionViewAtIndex(i, ListView.Contain)
                return
            }
        }
    }

    onColumnsChanged: rebuildRows()
    onModelChanged: rebuildRows()

    Connections {
        target: root.model
        ignoreUnknownSignals: true
        function onModelReset() {
            root.dataRevision += 1
            root.hoverSummaryId = ""
            root.rebuildRows()
        }
    }

    Rectangle {
        objectName: "presetBrowserWell"
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        color: appTheme.bgBaseColor
        border.width: 1
        border.color: root.dropHighlight ? appTheme.textColor : appTheme.cardBorderColor
    }

    ListView {
        id: rowsView
        objectName: "presetBrowserRows"
        anchors.fill: parent
        anchors.margins: appTheme.spaceXs
        clip: true
        model: root.displayRows.length
        spacing: root.tileGap
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: true
        keyNavigationEnabled: false
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Presets")

        // Enter applies the selected preset, but not while a tile shows its rename field.
        Keys.onPressed: function (event) {
            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                    && root.selectedPresetId.length > 0 && root.renamePresetId.length === 0) {
                root.activated(root.selectedPresetId)
                event.accepted = true
            }
        }

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
            padding: 0
        }

        delegate: Item {
            id: displayRow
            required property int index
            readonly property var entry: index < root.displayRows.length ? root.displayRows[index] : null
            readonly property bool title: !!entry && entry.kind === "title"
            width: rowsView.width
            height: title ? groupTitle.implicitHeight + appTheme.spaceXs : tileRow.rowHeight

            Label {
                id: groupTitle
                objectName: "presetBrowserGroupTitle"
                visible: displayRow.title
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: appTheme.spaceXs
                textFormat: Text.PlainText
                elide: Text.ElideRight
                text: displayRow.title ? displayRow.entry.text : ""
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
            }

            Row {
                id: tileRow
                visible: !displayRow.title
                spacing: root.tileGap
                readonly property real rowHeight: {
                    let h = 0
                    for (let i = 0; i < children.length; ++i) {
                        const child = children[i]
                        if (child.visible && child.implicitHeight !== undefined)
                            h = Math.max(h, child.implicitHeight)
                    }
                    return h
                }

                Repeater {
                    model: displayRow.title || !displayRow.entry ? 0 : displayRow.entry.count

                    delegate: PresetTile {
                        required property int index
                        readonly property int row: displayRow.entry.first + index
                        width: root.tileWidth
                        height: Math.max(implicitHeight, tileRow.rowHeight)
                        presetId: String(root.roleAt(row, PresetListModel.PresetIdRole) || "")
                        name: String(root.roleAt(row, PresetListModel.NameRole) || "")
                        available: root.roleAt(row, PresetListModel.AvailableRole) === true
                        previewSource: root.previewSource
                        selected: presetId.length > 0 && presetId === root.selectedPresetId
                        renaming: presetId.length > 0 && presetId === root.renamePresetId
                        summary: root.hoverSummaryId === presetId ? root.hoverSummary : ""

                        onSummaryRequested: {
                            root.hoverSummaryId = presetId
                            root.hoverSummary = String(root.roleAt(row, PresetListModel.SummaryRole) || "")
                        }
                        onClicked: {
                            rowsView.forceActiveFocus()
                            root.selectedPresetId = presetId
                        }
                        onActivated: {
                            root.selectedPresetId = presetId
                            root.activated(presetId)
                        }
                        onContextRequested: function (sceneX, sceneY) {
                            root.selectedPresetId = presetId
                            root.contextRequested(presetId, sceneX, sceneY)
                        }
                        onDragStarted: function (sceneX, sceneY) {
                            root.selectedPresetId = presetId
                            root.presetDragStarted(presetId, name, sceneX, sceneY)
                        }
                        onDragMoved: function (sceneX, sceneY) { root.presetDragMoved(sceneX, sceneY) }
                        onDragFinished: function (sceneX, sceneY) { root.presetDragFinished(sceneX, sceneY) }
                        onRenameCommitted: function (text) { root.renameCommitted(presetId, text) }
                        onRenameCanceled: root.renameCanceled()
                    }
                }
            }
        }
    }

    // Empty preset folder: the drop icon only (plan 2.1).
    ColorImage {
        objectName: "presetBrowserEmptyIcon"
        anchors.centerIn: parent
        width: appTheme.editorLutTileIconSize
        height: appTheme.editorLutTileIconSize
        visible: root.count === 0
        source: "qrc:/panel_icons/tray-download.svg"
        sourceSize.width: appTheme.editorLutTileIconSize * 2
        sourceSize.height: appTheme.editorLutTileIconSize * 2
        color: appTheme.textMutedColor
        Accessible.role: Accessible.Graphic
        Accessible.name: qsTr("No presets")
    }
}
