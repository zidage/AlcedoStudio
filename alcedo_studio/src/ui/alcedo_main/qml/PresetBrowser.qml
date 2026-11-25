import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import Alcedo.Main 1.0

// Preset browser (edit presets plan 2.1, step PS2.3): a toolbar with the search field
// and the grid / list switch, then the presets of the preset folder as rows of tiles
// (grid) or one row each (list), with a title row for each group (a direct subfolder).
// The Versions panel and the Apply Preset dialog use it. PresetListModel owns the
// rows; tiles read roles by row and keep no copies. A selection by click changes
// selectedPresetId only: the rows are not rebuilt and the list does not scroll.
//
// Search: each word of the query must occur in the group or the name of a preset,
// case ignored. The query belongs to this browser; the owner keeps the layout
// (viewMode) and gets viewModeRequested from the switch.
//
// previewSource is the preview controller of the photo of the preview images
// (PresetPreviewController, plan 2.4); each tile watches its preset on it. An empty
// preset folder shows the drop icon only; a query without a match shows the search
// icon only.
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
    // "grid" or "list"; the owner keeps it.
    property string viewMode: "grid"
    readonly property bool listMode: viewMode === "list"
    property string queryText: ""

    signal activated(string presetId)
    signal contextRequested(string presetId, real sceneX, real sceneY)
    signal presetDragStarted(string presetId, string name, real sceneX, real sceneY)
    signal presetDragMoved(real sceneX, real sceneY)
    signal presetDragFinished(real sceneX, real sceneY)
    signal renameCommitted(string presetId, string name)
    signal renameCanceled()
    signal viewModeRequested(string viewMode)

    readonly property int count: model ? Number(model.count) : 0
    // Presets that the query lets through.
    property int shownCount: 0
    readonly property int toolbarChrome: Math.max(
        appTheme.iconOpticalSizeCompact + appTheme.spaceSm,
        appTheme.iconButtonHitSizeCompact - appTheme.spaceSm)
    readonly property real toolbarHeight: toolbar.implicitHeight + appTheme.spaceXs
    // The area of the presets under the toolbar; an owner puts a drop guide in it.
    readonly property Item well: wellSlot
    readonly property int tileGap: listMode ? appTheme.spaceXs / 2 : appTheme.spaceXs
    readonly property int columns: listMode
                                   ? 1
                                   : Math.max(1, Math.floor((rowsView.width + tileGap)
                                                            / (appTheme.editorPresetTileMinWidth + tileGap)))
    readonly property real tileWidth: Math.max(0, (rowsView.width - (columns - 1) * tileGap) / columns)
    // Bumped on each model change so tile bindings read their roles again.
    property int dataRevision: 0
    // Display rows: {kind: "title", text} or {kind: "tiles", rows: [model row, ...]}.
    // Built on a model, column, or query change only.
    property var displayRows: []

    function roleAt(row, role) {
        const _revision = root.dataRevision
        if (!root.model || row < 0 || row >= root.count)
            return undefined
        return root.model.roleAt(row, role)
    }

    function queryWords() {
        const query = root.queryText.trim().toLocaleLowerCase()
        return query.length > 0 ? query.split(/\s+/) : []
    }

    function rebuildRows() {
        const rows = []
        const words = root.queryWords()
        // Read the count from the model: modelReset arrives before the count notification.
        const total = root.model ? Number(root.model.count) : 0
        let shown = 0
        let selectedShown = false
        let first = 0
        while (first < total) {
            const group = String(root.model.roleAt(first, PresetListModel.GroupRole) || "")
            const groupText = group.toLocaleLowerCase()
            let end = first
            const matches = []
            while (end < total
                   && String(root.model.roleAt(end, PresetListModel.GroupRole) || "") === group) {
                const text = groupText + " "
                             + String(root.model.roleAt(end, PresetListModel.NameRole) || "").toLocaleLowerCase()
                if (words.every(function (word) { return text.indexOf(word) >= 0 })) {
                    matches.push(end)
                    if (String(root.model.roleAt(end, PresetListModel.PresetIdRole) || "")
                            === root.selectedPresetId)
                        selectedShown = true
                }
                ++end
            }
            if (matches.length > 0 && group.length > 0)
                rows.push({ kind: "title", text: group })
            for (let start = 0; start < matches.length; start += root.columns)
                rows.push({ kind: "tiles", rows: matches.slice(start, start + root.columns) })
            shown += matches.length
            first = end
        }
        root.displayRows = rows
        root.shownCount = shown
        // Enter applies the selected preset; a hidden preset is not selected.
        if (!selectedShown && root.renamePresetId.length === 0)
            root.selectedPresetId = ""
    }

    // Scroll the tile of @p presetId into view (a new preset that enters rename). A
    // query that hides it is cleared first.
    function revealPreset(presetId) {
        if (!root.model)
            return
        const row = Number(root.model.rowOfPreset(presetId))
        if (row < 0)
            return
        for (let pass = 0; pass < 2; ++pass) {
            for (let i = 0; i < root.displayRows.length; ++i) {
                const entry = root.displayRows[i]
                if (entry.kind === "tiles" && entry.rows.indexOf(row) >= 0) {
                    rowsView.positionViewAtIndex(i, ListView.Contain)
                    return
                }
            }
            if (root.queryText.length === 0)
                return
            root.queryText = ""
        }
    }

    function focusSearch() {
        searchField.focusInput()
    }

    // From the search field to the results: the first shown preset is selected when
    // no shown preset is.
    function focusResults() {
        rowsView.forceActiveFocus()
        if (root.selectedPresetId.length > 0)
            return
        for (let i = 0; i < root.displayRows.length; ++i) {
            const entry = root.displayRows[i]
            if (entry.kind === "tiles" && entry.rows.length > 0) {
                root.selectedPresetId = String(root.roleAt(entry.rows[0], PresetListModel.PresetIdRole) || "")
                return
            }
        }
    }

    onColumnsChanged: rebuildRows()
    onModelChanged: rebuildRows()
    onQueryTextChanged: rebuildRows()

    Connections {
        target: root.model
        ignoreUnknownSignals: true
        function onModelReset() {
            root.dataRevision += 1
            root.rebuildRows()
        }
    }

    // ── Toolbar: search and layout ─────────────────────────────────────────
    RowLayout {
        id: toolbar
        objectName: "presetBrowserToolbar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: appTheme.spaceXs

        BrowserSearchField {
            id: searchField
            Layout.fillWidth: true
            Layout.preferredHeight: root.toolbarChrome
            chromeHeight: root.toolbarChrome
            inputObjectName: "presetBrowserSearchInput"
            clearButtonObjectName: "presetBrowserSearchClearButton"
            placeholderText: qsTr("Search Presets")
            text: root.queryText
            onTextEdited: function (text) { root.queryText = text }
            onSubmitted: root.focusResults()
            onCleared: root.queryText = ""
            onLeaveRequested: root.focusResults()
        }

        BrowserViewModeToggle {
            objectName: "presetBrowserViewModeNav"
            namePrefix: "presetBrowser"
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: root.toolbarChrome
            chromeHeight: root.toolbarChrome
            viewMode: root.viewMode
            onViewModeRequested: function (mode) { root.viewModeRequested(mode) }
        }
    }

    Item {
        id: wellSlot
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: toolbar.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: appTheme.spaceXs

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

            // Enter applies the selected preset, but not while a tile shows its rename
            // field. Ctrl+F moves to the search field.
            Keys.onPressed: function (event) {
                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                        && root.selectedPresetId.length > 0 && root.renamePresetId.length === 0) {
                    root.activated(root.selectedPresetId)
                    event.accepted = true
                } else if (event.key === Qt.Key_F && (event.modifiers & Qt.ControlModifier)) {
                    root.focusSearch()
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
                        model: displayRow.title || !displayRow.entry ? 0 : displayRow.entry.rows.length

                        delegate: PresetTile {
                            required property int index
                            readonly property int row: displayRow.entry.rows[index]
                            width: root.tileWidth
                            height: Math.max(implicitHeight, tileRow.rowHeight)
                            listMode: root.listMode
                            presetId: String(root.roleAt(row, PresetListModel.PresetIdRole) || "")
                            name: String(root.roleAt(row, PresetListModel.NameRole) || "")
                            available: root.roleAt(row, PresetListModel.AvailableRole) === true
                            previewSource: root.previewSource
                            selected: presetId.length > 0 && presetId === root.selectedPresetId
                            renaming: presetId.length > 0 && presetId === root.renamePresetId
                            reason: available ? ""
                                              : String(root.roleAt(row, PresetListModel.ReasonRole) || "")

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

        // Empty preset folder: the drop icon only (plan 2.1). A query without a match:
        // the search icon only.
        ColorImage {
            objectName: "presetBrowserEmptyIcon"
            anchors.centerIn: parent
            width: appTheme.editorLutTileIconSize
            height: appTheme.editorLutTileIconSize
            visible: root.shownCount === 0
            source: root.count === 0 ? "qrc:/panel_icons/tray-download.svg"
                                     : "qrc:/panel_icons/search.svg"
            sourceSize.width: appTheme.editorLutTileIconSize * 2
            sourceSize.height: appTheme.editorLutTileIconSize * 2
            color: appTheme.textMutedColor
            Accessible.role: Accessible.Graphic
            Accessible.name: root.count === 0 ? qsTr("No presets") : qsTr("No presets match")
        }
    }
}
