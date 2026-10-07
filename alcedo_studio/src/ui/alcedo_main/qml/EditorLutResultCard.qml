import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import Alcedo.Main 1.0

// LUT browser results (LUT library plan L6A, section 6.5): the filtered LUTs
// as a grid of tiles or a compact list, and a footer that names the applied LUT
// and its Color Grade target, with the library actions. Warnings and errors use
// AlertBadge. Search, order, and the layout switch live in the page toolbar
// (EditorLutBrowserPanel). Opening the page and clearing the filters scroll the
// applied LUT into view (revealAppliedEntry).
//
// Data ownership: LutLibraryModel (browser) owns rows, query, and focus;
// LutLibraryController (target) owns the Color Grade target and its current
// association; LutLibraryService (library) owns the entries. Tiles read model
// roles by row and keep no entry copies.
//
// Layout: tiles are laid out in rows of `columns` by a ListView over row
// indices, so every row takes the height of its tallest tile and titles are
// shown in full. Only visible rows instantiate tiles. The list layout is the
// same view with one column and rows without the cube placeholder.
//
// Input: choosing a tile applies it to the target as one settled edit (the
// Editor viewport shows the result); without a target it only moves focus.
// Arrow keys move through the grid the same way (ShortcutRegistry scope
// `editor.lut`); Ctrl+F asks the page to focus its search field.
Item {
    id: root
    objectName: "editorLutResultCard"

    property var browser: null
    property var target: null
    property var library: null
    property var host: null
    // "grid" or "list".
    property string viewMode: "grid"
    readonly property bool listMode: viewMode === "list"

    signal searchRequested()

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colBase: appTheme.bgBaseColor
    readonly property color colCardBorder: appTheme.cardBorderColor
    // Selection is an outline (graph selection tokens), never a filled well.
    readonly property color colSelectionOutline: appTheme.graphSelectionOutlineColor
    readonly property int selectionOutlineWidth: appTheme.graphSelectionOutlineWidth
    readonly property int toolbarChrome: Math.max(
        appTheme.iconOpticalSizeCompact + appTheme.spaceSm,
        appTheme.iconButtonHitSizeCompact - appTheme.spaceSm)
    readonly property int tileGap: listMode ? appTheme.spaceXs / 2 : appTheme.spaceXs

    readonly property int totalCount: browser ? Number(browser.totalCount) : 0
    readonly property int resultCount: browser ? Number(browser.count) : 0
    readonly property bool libraryBusy: !!library && library.busy === true
    readonly property string libraryError: library ? String(library.lastError || "") : ""
    readonly property bool canApply: !!target && target.canApply === true
    readonly property string appliedEntryId: target ? String(target.associationEntryId || "") : ""

    // Grid geometry: as many columns of at least editorLutTileMinWidth as fit.
    // Narrower than one such column, the single column shrinks with the well.
    readonly property int columns: listMode ? 1
                                            : Math.max(1, Math.floor((tileRows.width + tileGap)
                                                                     / (appTheme.editorLutTileMinWidth + tileGap)))
    readonly property real tileWidth: Math.max(0, (tileRows.width - (columns - 1) * tileGap) / columns)
    readonly property int rowCount: Math.ceil(resultCount / columns)

    // Bumped on every model change so tile bindings re-read their roles.
    property int dataRevision: 0
    // Last rejected favorite change (shown in the footer until the next success).
    property string favoriteError: ""

    // Diagnostics / tests and the rail's scroll restore.
    readonly property alias tileRowsView: tileRows
    readonly property real listContentY: tileRows.contentY
    function restoreListContentY(y) {
        tileRows.contentY = Math.max(0, Math.min(Number(y || 0),
                                                 Math.max(0, tileRows.contentHeight - tileRows.height)))
    }

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

    // Scroll the row of the applied LUT into view; no-op when it is not listed.
    function revealAppliedEntry() {
        if (!browser || root.appliedEntryId.length === 0)
            return false
        const row = Number(browser.rowOfEntry(root.appliedEntryId))
        if (row < 0)
            return false
        tileRows.positionViewAtIndex(Math.floor(row / root.columns), ListView.Contain)
        return true
    }

    // Remove every filter and the query, then show the applied LUT.
    function clearFiltersAndReveal() {
        if (!browser)
            return
        browser.clearFilters()
        Qt.callLater(root.revealAppliedEntry)
    }

    function focusResults() {
        tileRows.forceActiveFocus()
    }

    function toggleFavorite(entryId) {
        if (!browser || entryId.length === 0)
            return false
        const ok = browser.toggleFavorite(entryId)
        if (ok)
            root.favoriteError = ""
        return ok
    }

    Connections {
        target: root.browser
        ignoreUnknownSignals: true
        function onDataChanged() { root.dataRevision += 1 }
        function onModelReset() { root.dataRevision += 1 }
        function onRowsRemoved() { root.dataRevision += 1 }
        function onRowsInserted() { root.dataRevision += 1 }
        function onLayoutChanged() { root.dataRevision += 1 }
        function onFavoriteFailed(message) { root.favoriteError = String(message || "") }
    }

    // One chrome recipe for every toolbar SVG action.
    component ToolbarButton: IconActionButton {
        compact: true
        iconColorDefault: appTheme.iconColor
        iconColorMuted: root.colMuted
        fillIdle: "transparent"
        fillHover: appTheme.buttonHoveredFillColor
        fillSelected: appTheme.buttonSelectedFillColor
        Layout.preferredWidth: root.toolbarChrome
        Layout.preferredHeight: root.toolbarChrome
        Layout.minimumWidth: Layout.preferredWidth
        Layout.minimumHeight: Layout.preferredHeight
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: appTheme.spaceSm

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
                        root.searchRequested()
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
                          ? qsTr("Download the official LUT packages in Settings, import .cube files, or copy them into the user LUT folder and refresh.")
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
                        text: qsTr("Open user LUT folder")
                        onClicked: root.library.openUserDirectory()
                    }
                    DialogActionButton {
                        objectName: "editorLutEmptySettingsButton"
                        visible: root.totalCount === 0 && !!root.host
                        text: qsTr("Download official LUTs")
                        onClicked: root.host.openLutSettings()
                    }
                    DialogActionButton {
                        objectName: "editorLutClearSearchButton"
                        visible: root.totalCount > 0
                        text: qsTr("Clear search and filters")
                        onClicked: root.clearFiltersAndReveal()
                    }
                }
            }
        }

        // ── Footer: warnings, the applied LUT and its target, library actions ──
        AlertBadge {
            objectName: "editorLutAssociationMissing"
            text: root.target && root.target.associationMissing === true
                  ? qsTr("The LUT file is missing. The photo renders without it until the file returns.")
                  : ""
        }

        AlertBadge {
            objectName: "editorLutApplyError"
            text: root.target ? String(root.target.lastError || "") : ""
        }

        AlertBadge {
            objectName: "editorLutLibraryError"
            text: root.libraryError.length > 0 ? root.libraryError : root.favoriteError
        }

        RowLayout {
            objectName: "editorLutFooter"
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

            // The applied LUT and the node it applies to; without a target, the reason.
            Label {
                objectName: "editorLutTargetStatus"
                readonly property bool hasAssociation: !!root.target
                                                       && root.target.hasAssociation === true
                readonly property string nodeName: root.target
                                                   ? String(root.target.targetNodeName || "") : ""
                Layout.fillWidth: true
                Layout.leftMargin: appTheme.spaceXs
                text: root.libraryBusy
                      ? qsTr("Refreshing LUT library")
                      : (!root.canApply
                         ? (root.target ? String(root.target.targetMessage || "") : "")
                         : (hasAssociation
                            ? qsTr("%1 applied to %2").arg(String(root.target.associationName))
                                                      .arg(nodeName)
                            : qsTr("Choose a LUT to apply to %1").arg(nodeName)))
                color: hasAssociation && root.canApply && !root.libraryBusy ? root.colText
                                                                             : root.colMuted
                wrapMode: Text.Wrap
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
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
                enabled: !!root.library
                iconSrc: "qrc:/panel_icons/folder-open.svg"
                actionName: qsTr("Open user LUT folder")
                // A rejected dispatch sets the library's lastError (shown above).
                onClicked: root.library.openUserDirectory()
            }

            // Where the user's own LUT files go.
            EditorInfoHint {
                objectName: "editorLutUserFolderHint"
                Layout.alignment: Qt.AlignVCenter
                Layout.rightMargin: appTheme.spaceXs
                visible: !!root.library
                text: qsTr("Put your own .cube files in the user folder of the LUT library:\n%1\nRefresh the library afterwards; they are listed under the User category. Import LUTs copies files there for you.")
                      .arg(root.library ? String(root.library.userDirectoryPath) : "")
            }
        }
    }

    // One LUT: a grid tile (placeholder cube, centered title, print line, status,
    // hover star) or, in the list layout, a compact row (title and print left,
    // star right, no cube). Both keep one data contract and one chrome.
    component EditorLutTile: Item {
        id: tile

        property int row: -1
        readonly property bool listMode: root.listMode
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
        readonly property int textAlignment: listMode ? Text.AlignLeft : Text.AlignHCenter
        // Room the text leaves for the star (always shown in the list layout).
        readonly property int starReserve: appTheme.iconOpticalSizeCompact + appTheme.spaceXs
        // The star's own area sits above the tile's; either one counts as hovering the tile.
        readonly property bool hovered: tileHover.containsMouse || starArea.containsMouse

        objectName: "editorLutTile"
        visible: present
        implicitHeight: tileColumn.implicitHeight
                        + (listMode ? appTheme.spaceSm + appTheme.spaceXs : appTheme.spaceSm * 2)
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
            color: tile.hovered || (tile.focused && tileRows.activeFocus)
                   ? appTheme.buttonHoveredFillColor : "transparent"
            border.width: tile.applied ? root.selectionOutlineWidth : 0
            border.color: root.colSelectionOutline
        }

        ColumnLayout {
            id: tileColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: tile.listMode ? parent.verticalCenter : undefined
            anchors.top: tile.listMode ? undefined : parent.top
            anchors.leftMargin: appTheme.spaceSm
            anchors.rightMargin: tile.listMode ? appTheme.spaceSm + tile.starReserve : appTheme.spaceSm
            anchors.topMargin: appTheme.spaceSm
            spacing: tile.listMode ? 0 : appTheme.spaceXs

            ColorImage {
                objectName: "editorLutTileCube"
                visible: !tile.listMode
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
                horizontalAlignment: tile.textAlignment
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                text: tile.title
                color: tile.selectable ? tile.inkColor : tile.mutedInkColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: tile.listMode ? appTheme.fontSizeBody : appTheme.fontSizeCaption
                font.weight: tile.applied ? appTheme.fontWeightStrong : appTheme.fontWeightRegular
            }

            Label {
                objectName: "editorLutTilePrint"
                Layout.fillWidth: true
                visible: tile.printName.length > 0
                horizontalAlignment: tile.textAlignment
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                text: tile.printName
                color: tile.mutedInkColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }

            AlertBadge {
                Layout.fillWidth: true
                text: tile.statusText
            }
        }

        MouseArea {
            id: tileHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: tile.selectable ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: {
                tileRows.forceActiveFocus()
                root.activateRow(tile.row)
            }
        }

        ToolTip.visible: tile.hovered && tile.detailText.length > 0
        ToolTip.delay: 600
        ToolTip.text: tile.detailText

        // Favorite star: glyph only. Grid tiles show it when starred or hovered;
        // list rows always keep it in their star column.
        ColorImage {
            id: favoriteStar
            objectName: "editorLutFavoriteStar"
            anchors.top: tile.listMode ? undefined : parent.top
            anchors.verticalCenter: tile.listMode ? parent.verticalCenter : undefined
            anchors.right: parent.right
            anchors.topMargin: appTheme.spaceXs
            anchors.rightMargin: tile.listMode ? appTheme.spaceSm : appTheme.spaceXs
            width: appTheme.iconOpticalSizeCompact
            height: appTheme.iconOpticalSizeCompact
            visible: tile.present && (tile.listMode || tile.favorite || tile.hovered)
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
                id: starArea
                anchors.fill: parent
                anchors.margins: -appTheme.spaceXs
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.toggleFavorite(tile.entryId)
            }
        }
    }
}
