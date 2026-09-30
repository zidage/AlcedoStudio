import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import Alcedo.Main 1.0

// LUT browser result card (LUT library plan L6A, section 6.5): search and
// library actions, the target indicator, and the filtered LUTs as tiles.
//
// Data ownership: LutLibraryModel (browser) owns rows, query, and focus;
// LutLibraryController (target) owns the Color Grade target and its current
// association; LutLibraryService (library) owns the entries. Tiles read model
// roles by row and keep no entry copies.
//
// Layout: tiles are laid out in rows of `columns` by a ListView over row
// indices, so every row takes the height of its tallest tile and titles are
// shown in full. Only visible rows instantiate tiles.
//
// Input: choosing a tile applies it to the target as one settled edit (the
// Editor viewport shows the result); without a target it only moves focus.
// Arrow keys move through the grid the same way (ShortcutRegistry scope
// `editor.lut`); the search field keeps native text input.
Rectangle {
    id: root
    objectName: "editorLutResultCard"

    property var browser: null
    property var target: null
    property var library: null
    property var host: null

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colBase: appTheme.bgBaseColor
    readonly property color colCardBorder: appTheme.cardBorderColor
    readonly property color colSelectedFill: appTheme.editorListSelectedFillColor
    readonly property color colSelectedInk: appTheme.editorListSelectedInkColor
    // Selection is an outline (graph selection tokens), never a filled well.
    readonly property color colSelectionOutline: appTheme.graphSelectionOutlineColor
    readonly property int selectionOutlineWidth: appTheme.graphSelectionOutlineWidth
    readonly property int toolbarChrome: Math.max(
        appTheme.iconOpticalSizeCompact + appTheme.spaceSm,
        appTheme.iconButtonHitSizeCompact - appTheme.spaceSm)
    readonly property int tileGap: appTheme.spaceXs

    readonly property int totalCount: browser ? Number(browser.totalCount) : 0
    readonly property int resultCount: browser ? Number(browser.count) : 0
    readonly property bool libraryBusy: !!library && library.busy === true
    readonly property string libraryError: library ? String(library.lastError || "") : ""
    readonly property bool canApply: !!target && target.canApply === true
    readonly property string appliedEntryId: target ? String(target.associationEntryId || "") : ""

    // Grid geometry: as many columns of at least editorLutTileMinWidth as fit.
    readonly property int columns: Math.max(1, Math.floor((tileRows.width + tileGap)
                                                         / (appTheme.editorLutTileMinWidth + tileGap)))
    readonly property real tileWidth: (tileRows.width - (columns - 1) * tileGap) / columns
    readonly property int rowCount: Math.ceil(resultCount / columns)

    // Bumped on every model change so tile bindings re-read their roles.
    property int dataRevision: 0

    // Diagnostics / tests and the rail's scroll restore.
    readonly property alias tileRowsView: tileRows
    readonly property real listContentY: tileRows.contentY
    function restoreListContentY(y) {
        tileRows.contentY = Math.max(0, Math.min(Number(y || 0),
                                                 Math.max(0, tileRows.contentHeight - tileRows.height)))
    }

    radius: appTheme.panelRadius
    color: appTheme.cardSurfaceColor
    border.width: 1
    border.color: appTheme.cardBorderColor

    function roleAt(row, role) {
        const _revision = root.dataRevision
        if (!browser || row < 0 || row >= root.resultCount)
            return undefined
        return browser.data(browser.index(row, 0), role)
    }

    // Choosing a tile: focus it, then apply it to the target when there is one.
    // Choosing the applied tile again removes the LUT from the target.
    function activateEntry(entryId) {
        if (!browser || entryId.length === 0)
            return false
        browser.focusEntry(entryId)
        if (!root.canApply)
            return false
        if (entryId === root.appliedEntryId)
            return target.clearAssociation()
        return target.applyEntry(entryId)
    }

    function activateRow(row) {
        if (row < 0 || row >= root.resultCount)
            return false
        return root.activateEntry(String(browser.entryIdAt(row)))
    }

    // Keyboard browsing: move focus by @p step tiles and apply the new tile.
    function moveFocus(step) {
        if (!browser || root.resultCount === 0)
            return
        const current = Number(browser.focusedRow)
        let row = current < 0 ? 0 : current + step
        if (Math.abs(step) === 1 && current >= 0) {
            if (!browser.focusRelative(step))
                return
            row = Number(browser.focusedRow)
        }
        row = Math.max(0, Math.min(root.resultCount - 1, row))
        // At a grid edge the key does nothing; it must not toggle the current tile off.
        if (row === current)
            return
        root.activateRow(row)
        tileRows.positionViewAtIndex(Math.floor(row / root.columns), ListView.Contain)
    }

    function focusSearch() {
        searchInput.forceActiveFocus()
        searchInput.selectAll()
    }

    Connections {
        target: root.browser
        ignoreUnknownSignals: true
        function onDataChanged() { root.dataRevision += 1 }
        function onModelReset() { root.dataRevision += 1 }
        function onRowsRemoved() { root.dataRevision += 1 }
        function onRowsInserted() { root.dataRevision += 1 }
        function onLayoutChanged() { root.dataRevision += 1 }
    }

    // One chrome recipe for every toolbar SVG action.
    component ToolbarButton: IconActionButton {
        compact: true
        iconColorDefault: appTheme.iconColor
        iconColorMuted: root.colMuted
        fillIdle: root.colBase
        fillHover: appTheme.buttonHoveredFillColor
        fillSelected: appTheme.buttonSelectedFillColor
        Layout.preferredWidth: root.toolbarChrome
        Layout.preferredHeight: root.toolbarChrome
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appTheme.spaceMd
        spacing: appTheme.spaceSm

        // ── Search and library actions (sunken track) ─────────────────────
        Rectangle {
            objectName: "editorLutToolbar"
            Layout.fillWidth: true
            Layout.preferredHeight: root.toolbarChrome + appTheme.spaceXs * 2
            radius: appTheme.controlRadiusSmall
            color: root.colBase
            border.width: 1
            border.color: root.colCardBorder

            RowLayout {
                anchors.fill: parent
                anchors.margins: appTheme.spaceXs / 2
                spacing: appTheme.spaceXs

                ColorImage {
                    Layout.leftMargin: appTheme.spaceXs
                    Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                    Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                    source: "qrc:/panel_icons/search.svg"
                    sourceSize.width: appTheme.iconSourceSizeCompact
                    sourceSize.height: appTheme.iconSourceSizeCompact
                    color: appTheme.iconColor
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    TextInput {
                        id: searchInput
                        objectName: "editorLutSearchInput"
                        anchors.fill: parent
                        verticalAlignment: TextInput.AlignVCenter
                        color: root.colText
                        selectionColor: root.colSelectedFill
                        selectedTextColor: root.colSelectedInk
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightRegular
                        clip: true
                        text: root.browser ? String(root.browser.queryText) : ""
                        Accessible.role: Accessible.EditableText
                        Accessible.name: qsTr("Search LUTs")
                        onTextEdited: {
                            if (root.browser)
                                root.browser.queryText = text
                        }
                        Keys.onReturnPressed: {
                            if (root.browser)
                                root.browser.applyQueryNow()
                            tileRows.forceActiveFocus()
                        }
                        Keys.onEscapePressed: {
                            if (text.length > 0 && root.browser) {
                                root.browser.queryText = ""
                                root.browser.applyQueryNow()
                            } else {
                                tileRows.forceActiveFocus()
                            }
                        }
                    }

                    Text {
                        anchors.fill: searchInput
                        verticalAlignment: Text.AlignVCenter
                        visible: searchInput.text.length === 0 && !searchInput.activeFocus
                        text: qsTr("Search LUTs")
                        color: root.colMuted
                        elide: Text.ElideRight
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightRegular
                    }
                }

                Item {
                    Layout.preferredWidth: root.toolbarChrome
                    Layout.preferredHeight: root.toolbarChrome

                    ToolbarButton {
                        anchors.fill: parent
                        objectName: "editorLutSortButton"
                        iconSrc: "qrc:/panel_icons/sort.svg"
                        actionName: qsTr("Sort LUTs")
                        onClicked: sortPopup.open()
                    }

                    Popup {
                        id: sortPopup
                        objectName: "editorLutSortPopup"
                        y: parent.height + appTheme.spaceXs
                        x: parent.width - width
                        width: appTheme.editorLutBrowserFilterWidth
                        padding: appTheme.spaceXs
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                        background: Rectangle {
                            color: appTheme.cardSurfaceColor
                            border.width: 1
                            border.color: root.colCardBorder
                            radius: appTheme.controlRadiusSmall
                        }

                        ColumnLayout {
                            width: sortPopup.availableWidth
                            spacing: appTheme.spaceXs / 2

                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: appTheme.spaceSm
                                Layout.rightMargin: appTheme.spaceSm
                                Layout.topMargin: appTheme.spaceXs
                                text: root.browser && String(root.browser.queryText).length > 0
                                      ? qsTr("Search results are ordered by relevance.")
                                      : qsTr("Sort by")
                                color: root.colMuted
                                wrapMode: Text.Wrap
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            Repeater {
                                model: [
                                    { label: qsTr("Name"), key: "name" },
                                    { label: qsTr("Modified time"), key: "modified" }
                                ]
                                delegate: Rectangle {
                                    id: sortRow
                                    required property var modelData
                                    readonly property bool current: !!root.browser
                                        && String(root.browser.sortKey) === modelData.key

                                    Layout.fillWidth: true
                                    Layout.preferredHeight: appTheme.lineHeightBody + appTheme.spaceSm
                                    radius: appTheme.badgeRadius
                                    color: sortHover.hovered ? appTheme.buttonHoveredFillColor
                                                             : "transparent"
                                    border.width: current ? root.selectionOutlineWidth : 0
                                    border.color: root.colSelectionOutline

                                    Text {
                                        anchors.left: parent.left
                                        anchors.leftMargin: appTheme.spaceSm
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: sortRow.modelData.label
                                        color: root.colText
                                        font.family: appTheme.uiFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                        font.weight: sortRow.current ? appTheme.fontWeightStrong
                                                                     : appTheme.fontWeightRegular
                                    }

                                    Text {
                                        anchors.right: parent.right
                                        anchors.rightMargin: appTheme.spaceSm
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: sortRow.current
                                        text: root.browser && root.browser.sortAscending ? "▲" : "▼"
                                        color: root.colText
                                        font.family: appTheme.uiFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                    }

                                    HoverHandler {
                                        id: sortHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        onTapped: {
                                            if (!root.browser)
                                                return
                                            if (sortRow.current) {
                                                root.browser.sortAscending = !root.browser.sortAscending
                                            } else {
                                                root.browser.sortKey = sortRow.modelData.key
                                                root.browser.sortAscending = true
                                            }
                                            sortPopup.close()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                    color: root.colCardBorder
                }

                ToolbarButton {
                    objectName: "editorLutImportButton"
                    enabled: !!root.library && !root.libraryBusy && !!root.host
                    iconSrc: "qrc:/panel_icons/import.svg"
                    actionName: qsTr("Import LUTs")
                    onClicked: root.host.openLutImportDialog()
                }

                ToolbarButton {
                    objectName: "editorLutRefreshButton"
                    enabled: !!root.library && !root.libraryBusy
                    iconSrc: "qrc:/panel_icons/retry.svg"
                    actionName: qsTr("Refresh LUT library")
                    onClicked: root.library.refresh()
                }

                ToolbarButton {
                    objectName: "editorLutOpenFolderButton"
                    Layout.rightMargin: appTheme.spaceXs
                    enabled: !!root.library
                    iconSrc: "qrc:/panel_icons/folder-open.svg"
                    actionName: qsTr("Open LUT folder")
                    // A rejected dispatch sets the library's lastError (shown below).
                    onClicked: root.library.openRootDirectory()
                }
            }
        }

        // ── Target indicator ──────────────────────────────────────────────
        ColumnLayout {
            objectName: "editorLutTargetIndicator"
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceXs
            Layout.rightMargin: appTheme.spaceXs
            spacing: appTheme.spaceXs / 2

            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm
                visible: root.canApply

                Label {
                    text: qsTr("Applies to")
                    color: root.colMuted
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }

                Label {
                    objectName: "editorLutTargetNodeName"
                    Layout.fillWidth: true
                    text: root.target ? String(root.target.targetNodeName) : ""
                    color: root.colText
                    wrapMode: Text.Wrap
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightStrong
                }
            }

            Label {
                objectName: "editorLutTargetMessage"
                Layout.fillWidth: true
                visible: !root.canApply
                text: root.target ? String(root.target.targetMessage) : ""
                color: root.colMuted
                wrapMode: Text.Wrap
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm
                visible: !!root.target && root.target.hasImage === true
                         && String(root.target.targetState) !== "noNode"
                         && String(root.target.targetState) !== "notColorGrade"

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        objectName: "editorLutAssociationName"
                        Layout.fillWidth: true
                        text: root.target && root.target.hasAssociation
                              ? String(root.target.associationName)
                              : qsTr("No LUT applied")
                        color: root.target && root.target.hasAssociation ? root.colText : root.colMuted
                        wrapMode: Text.Wrap
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        font.weight: appTheme.fontWeightRegular
                    }

                    Label {
                        objectName: "editorLutAssociationPrint"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.target ? String(root.target.associationPrintName || "") : ""
                        color: root.colMuted
                        wrapMode: Text.Wrap
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    Label {
                        objectName: "editorLutAssociationMissing"
                        Layout.fillWidth: true
                        visible: !!root.target && root.target.associationMissing === true
                        text: qsTr("The LUT file is missing. The photo renders without it until the file returns.")
                        color: appTheme.dangerColor
                        wrapMode: Text.Wrap
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }
                }

            }

            Label {
                objectName: "editorLutApplyError"
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.target ? String(root.target.lastError || "") : ""
                color: appTheme.dangerColor
                wrapMode: Text.Wrap
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }
        }

        // ── Tiles (sunken well) ───────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: appTheme.controlRadiusSmall
            color: root.colBase
            border.width: 1
            border.color: root.colCardBorder
            clip: true

            ListView {
                id: tileRows
                objectName: "editorLutTileRows"
                anchors.fill: parent
                anchors.margins: appTheme.spaceXs
                model: root.rowCount
                spacing: root.tileGap
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.VerticalFlick
                activeFocusOnTab: true
                keyNavigationEnabled: false
                cacheBuffer: Math.max(0, Math.ceil(height))
                Accessible.role: Accessible.List
                Accessible.name: qsTr("LUTs")

                Keys.onPressed: function(event) {
                    const commandId = ShortcutRegistry.commandIdForKey(
                        "editor.lut", event.key, event.modifiers)
                    if (commandId === "lut.selectPrevious") {
                        root.moveFocus(-1)
                    } else if (commandId === "lut.selectNext") {
                        root.moveFocus(1)
                    } else if (commandId === "lut.selectAbove") {
                        root.moveFocus(-root.columns)
                    } else if (commandId === "lut.selectBelow") {
                        root.moveFocus(root.columns)
                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                || event.key === Qt.Key_Space) && root.browser) {
                        root.activateEntry(String(root.browser.focusedEntryId))
                    } else if (event.key === Qt.Key_F && (event.modifiers & Qt.ControlModifier)) {
                        root.focusSearch()
                    } else {
                        return
                    }
                    event.accepted = true
                }

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                    padding: 0
                }

                delegate: Item {
                    id: tileRow
                    required property int index
                    width: tileRows.width
                    height: tileRowLayout.implicitHeight

                    Row {
                        id: tileRowLayout
                        spacing: root.tileGap

                        Repeater {
                            model: root.columns

                            delegate: EditorLutTile {
                                required property int index
                                row: tileRow.index * root.columns + index
                                width: root.tileWidth
                                height: Math.max(implicitHeight, tileRowLayout.rowImplicitHeight)
                            }
                        }

                        // Tallest tile of this row; every tile of the row takes it.
                        readonly property real rowImplicitHeight: {
                            let h = 0
                            for (let i = 0; i < children.length; ++i) {
                                const child = children[i]
                                if (child.visible && child.implicitHeight !== undefined)
                                    h = Math.max(h, child.implicitHeight)
                            }
                            return h
                        }
                    }
                }
            }

            // Loading, empty library, and zero-result states.
            ColumnLayout {
                objectName: "editorLutEmptyState"
                anchors.centerIn: parent
                width: Math.min(parent.width - appTheme.spaceXl * 2, appTheme.editorSidePanelWidth)
                visible: root.resultCount === 0
                spacing: appTheme.spaceSm

                Label {
                    objectName: "editorLutEmptyTitle"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: root.libraryBusy && root.totalCount === 0
                          ? qsTr("Loading LUT library")
                          : (root.totalCount === 0 ? qsTr("No LUTs in the library")
                                                   : qsTr("No LUTs match"))
                    color: root.colText
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeTitle
                    font.weight: appTheme.fontWeightStrong
                }

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    visible: !(root.libraryBusy && root.totalCount === 0)
                    text: root.totalCount === 0
                          ? qsTr("Import .cube files, or copy them into the LUT folder and refresh.")
                          : qsTr("Change the search or the filters to see more LUTs.")
                    color: root.colMuted
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }

                ColumnLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: appTheme.spaceSm
                    visible: !root.libraryBusy

                    DialogActionButton {
                        objectName: "editorLutEmptyImportButton"
                        visible: root.totalCount === 0 && !!root.host
                        text: qsTr("Import LUTs")
                        onClicked: root.host.openLutImportDialog()
                    }
                    DialogActionButton {
                        objectName: "editorLutEmptyOpenFolderButton"
                        visible: root.totalCount === 0 && !!root.library
                        text: qsTr("Open LUT folder")
                        onClicked: root.library.openRootDirectory()
                    }
                    DialogActionButton {
                        objectName: "editorLutClearSearchButton"
                        visible: root.totalCount > 0
                        text: qsTr("Clear search and filters")
                        onClicked: root.browser.clearFilters()
                    }
                }
            }
        }

        // ── Footer: result count and library errors ──────────────────────
        Label {
            objectName: "editorLutLibraryError"
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.libraryError
            color: appTheme.dangerColor
            wrapMode: Text.Wrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }

        Label {
            objectName: "editorLutCountText"
            Layout.fillWidth: true
            text: root.libraryBusy
                  ? qsTr("Refreshing LUT library")
                  : (root.resultCount !== root.totalCount
                     ? qsTr("%1 of %2 LUTs").arg(root.resultCount).arg(root.totalCount)
                     : (root.totalCount === 1 ? qsTr("1 LUT")
                                              : qsTr("%1 LUTs").arg(root.totalCount)))
            color: root.colMuted
            wrapMode: Text.Wrap
            font.family: appTheme.dataFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }
    }

    // One LUT tile: placeholder cube, full title, print line, status, favorite.
    component EditorLutTile: Item {
        id: tile

        property int row: -1
        readonly property bool present: row >= 0 && row < root.resultCount
        readonly property string entryId: present ? String(root.roleAt(row, LutLibraryModel.EntryIdRole)) : ""
        readonly property string title: present ? String(root.roleAt(row, LutLibraryModel.DisplayNameRole) || "") : ""
        readonly property string printName: present ? String(root.roleAt(row, LutLibraryModel.PrintNameRole) || "") : ""
        readonly property string statusText: present ? String(root.roleAt(row, LutLibraryModel.StatusTextRole) || "") : ""
        readonly property string detailText: present ? String(root.roleAt(row, LutLibraryModel.DetailTextRole) || "") : ""
        readonly property bool selectable: present && root.roleAt(row, LutLibraryModel.SelectableRole) === true
        readonly property bool favorite: present && root.roleAt(row, LutLibraryModel.FavoriteRole) === true
        readonly property bool focused: present && root.roleAt(row, LutLibraryModel.FocusedRole) === true
        readonly property bool applied: present && root.roleAt(row, LutLibraryModel.AppliedRole) === true
        readonly property color inkColor: root.colText
        readonly property color mutedInkColor: root.colMuted

        objectName: "editorLutTile"
        visible: present
        implicitHeight: tileColumn.implicitHeight + appTheme.spaceSm * 2
        Accessible.role: Accessible.CheckBox
        Accessible.checkable: true
        Accessible.checked: applied
        Accessible.name: title
        Accessible.description: printName

        // Applied: outline only. Hover and keyboard focus: the quiet hover well.
        Rectangle {
            objectName: "editorLutTileChrome"
            anchors.fill: parent
            radius: appTheme.controlRadiusSmall
            color: tileHover.hovered || (tile.focused && tileRows.activeFocus)
                   ? appTheme.buttonHoveredFillColor : "transparent"
            border.width: tile.applied ? root.selectionOutlineWidth : 0
            border.color: root.colSelectionOutline
        }

        ColumnLayout {
            id: tileColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: appTheme.spaceSm
            spacing: appTheme.spaceXs

            ColorImage {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: appTheme.spaceXs
                Layout.preferredWidth: appTheme.editorLutTileIconSize
                Layout.preferredHeight: appTheme.editorLutTileIconSize
                source: "qrc:/panel_icons/lut-cube.svg"
                sourceSize.width: appTheme.editorLutTileIconSize * 2
                sourceSize.height: appTheme.editorLutTileIconSize * 2
                color: tile.applied ? root.colText : appTheme.iconColor
                opacity: tile.selectable ? 1.0 : 0.45
            }

            Label {
                objectName: "editorLutTileTitle"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                text: tile.title
                color: tile.selectable ? tile.inkColor : tile.mutedInkColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: tile.applied ? appTheme.fontWeightStrong : appTheme.fontWeightRegular
            }

            Label {
                objectName: "editorLutTilePrint"
                Layout.fillWidth: true
                visible: tile.printName.length > 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                text: tile.printName
                color: tile.mutedInkColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }

            Label {
                Layout.fillWidth: true
                visible: tile.statusText.length > 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: tile.statusText
                color: appTheme.dangerColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }
        }

        MouseArea {
            id: tileHover
            readonly property bool hovered: containsMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: tile.selectable ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: {
                tileRows.forceActiveFocus()
                root.activateRow(tile.row)
            }
        }

        ToolTip.visible: tileHover.hovered && tile.detailText.length > 0
        ToolTip.delay: 600
        ToolTip.text: tile.detailText

        // Favorite star: glyph only.
        ColorImage {
            id: favoriteStar
            objectName: "editorLutFavoriteStar"
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: appTheme.spaceXs
            width: appTheme.iconOpticalSizeCompact
            height: appTheme.iconOpticalSizeCompact
            visible: tile.favorite || tileHover.hovered
            source: "qrc:/panel_icons/star.svg"
            sourceSize.width: appTheme.iconSourceSizeCompact
            sourceSize.height: appTheme.iconSourceSizeCompact
            color: tile.favorite ? appTheme.editorListFavoriteActiveColor
                                 : appTheme.editorListFavoriteIdleColor
            Accessible.role: Accessible.CheckBox
            Accessible.checked: tile.favorite
            Accessible.name: tile.favorite ? qsTr("Remove from favorites") : qsTr("Add to favorites")

            // Above the tile's MouseArea, so a star click never applies the tile.
            MouseArea {
                anchors.fill: parent
                anchors.margins: -appTheme.spaceXs
                cursorShape: Qt.PointingHandCursor
                onClicked: root.browser.toggleFavorite(tile.entryId)
            }
        }
    }
}
