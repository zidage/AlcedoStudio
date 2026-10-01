import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// LUT browser: the `luts` page of the Editor left rail (LUT library plan L6A,
// sections 1.4 and 6.5). The page sits in the rail's card shell: a toolbar
// (filter toggle, search, sort, grid/list) spans the top; below it the filter
// sidebar folds beside the results. Both bind the application-wide browser
// model and target controller, so every LUT surface shows one library and one
// target. Opening or closing this page never submits an edit and never changes
// the image, the node selection, or the route.
//
// Space: the filter sidebar folds to zero width with the rail's fold motion.
// It stays docked while the results keep one compact tile column beside it;
// the tiles shrink to fit. When the page narrows past that, the sidebar closes
// by itself and reopens once the page is wide enough again. Opened by hand on
// a page that narrow, it floats over the results until it is closed.
Item {
    id: root
    objectName: "editorLutBrowserPanel"

    property var theme: null
    property var editorSession: null
    property var host: null
    readonly property var modules: (typeof appModules !== "undefined" && appModules) ? appModules : null
    property var browser: modules && modules.lutBrowser ? modules.lutBrowser : null
    property var target: modules && modules.lutTarget ? modules.lutTarget : null
    property var library: modules && modules.lutLibrary ? modules.lutLibrary : null

    // View state. The rail owns the lasting copy (this page is unloaded on close).
    // filtersVisible is the user's choice; filtersOpen is what the page shows.
    property bool filtersVisible: true
    // "grid" or "list".
    property string viewMode: "grid"

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colBase: appTheme.bgBaseColor
    readonly property color colCardBorder: appTheme.cardBorderColor
    readonly property int toolbarChrome: Math.max(
        appTheme.iconOpticalSizeCompact + appTheme.spaceSm,
        appTheme.iconButtonHitSizeCompact - appTheme.spaceSm)
    readonly property int filterWidth: appTheme.editorLutBrowserFilterWidth
    // Docked while the results keep one compact tile column beside the sidebar
    // (the column plus the tile well's margins).
    readonly property bool filterDocked: body.width >= filterWidth + appTheme.spaceSm
                                                     + appTheme.editorLutTileCompactWidth
                                                     + appTheme.spaceXs * 2
    // Opened by hand while the page is too narrow to dock; cleared by any resize
    // across the dock width, so narrowing the page always closes the sidebar.
    property bool _openedUndocked: false
    readonly property bool filtersOpen: filtersVisible && (filterDocked || _openedUndocked)
    readonly property bool anyFilterActive: filterCard.anyFilterActive
    onFilterDockedChanged: _openedUndocked = false

    // Fold progress of the filter sidebar (0 folded → 1 open).
    property real filterOpenProgress: filtersOpen ? 1 : 0
    property bool _motionArmed: false
    Behavior on filterOpenProgress {
        enabled: root._motionArmed
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0
                                            : (root.filtersOpen ? appTheme.motionFoldOpenMs
                                                                   : appTheme.motionFoldCloseMs)
            easing.type: appTheme.motionEasing
        }
    }
    Component.onCompleted: _motionArmed = true

    // Rail scroll restore (EditorWorkspaceRail captures these per page).
    readonly property real listContentY: resultCard.listContentY
    function restoreListContentY(y) {
        resultCard.restoreListContentY(y)
    }
    function focusSearch() {
        searchInput.forceActiveFocus()
        searchInput.selectAll()
    }
    function toggleFilters() {
        if (filtersOpen) {
            filtersVisible = false
            _openedUndocked = false
        } else {
            filtersVisible = true
            _openedUndocked = !filterDocked
        }
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
        anchors.margins: appTheme.spaceSm
        spacing: appTheme.spaceSm

        // ── Toolbar: filters, search, order, and layout ───────────────────
        RowLayout {
            objectName: "editorLutToolbar"
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

            ToolbarButton {
                objectName: "editorLutFilterToggle"
                iconSrc: root.filtersOpen ? "qrc:/panel_icons/layout-sidebar.svg"
                                          : "qrc:/panel_icons/layout-sidebar-inactive.svg"
                actionName: root.filtersOpen ? qsTr("Hide filters") : qsTr("Show filters")
                onClicked: root.toggleFilters()
            }

            // Sunken search track.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: root.toolbarChrome
                radius: appTheme.controlRadiusSmall
                color: root.colBase
                border.width: 1
                border.color: searchInput.activeFocus ? appTheme.textMutedColor : root.colCardBorder

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceSm
                    anchors.rightMargin: appTheme.spaceXs / 2
                    spacing: appTheme.spaceXs

                    ColorImage {
                        Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                        Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                        source: "qrc:/panel_icons/search.svg"
                        sourceSize.width: appTheme.iconSourceSizeCompact
                        sourceSize.height: appTheme.iconSourceSizeCompact
                        color: root.colMuted
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
                            selectionColor: appTheme.editorListSelectedFillColor
                            selectedTextColor: appTheme.editorListSelectedInkColor
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
                                resultCard.focusResults()
                            }
                            Keys.onEscapePressed: {
                                if (text.length > 0 && root.browser) {
                                    root.browser.queryText = ""
                                    root.browser.applyQueryNow()
                                } else {
                                    resultCard.focusResults()
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

                    ToolbarButton {
                        objectName: "editorLutSearchClearButton"
                        visible: searchInput.text.length > 0
                        Layout.preferredWidth: root.toolbarChrome - appTheme.spaceXs
                        Layout.preferredHeight: root.toolbarChrome - appTheme.spaceXs
                        iconSrc: "qrc:/panel_icons/close.svg"
                        actionName: qsTr("Clear search")
                        focusOnPointerPress: false
                        onClicked: {
                            if (!root.browser)
                                return
                            root.browser.queryText = ""
                            root.browser.applyQueryNow()
                        }
                    }
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
                    width: root.filterWidth
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
                                border.width: current ? appTheme.graphSelectionOutlineWidth : 0
                                border.color: appTheme.graphSelectionOutlineColor

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

            // Grid / list segments: the monochrome segmented family (sunken track,
            // light well under the current segment), sized to the toolbar row.
            Rectangle {
                id: viewModeTrack
                objectName: "editorLutViewModeNav"
                readonly property int inset: appTheme.spaceXs / 2
                readonly property int segment: root.toolbarChrome - inset * 2
                Layout.preferredWidth: segment * 2 + inset * 2
                Layout.preferredHeight: root.toolbarChrome
                radius: appTheme.controlRadiusSmall
                color: root.colBase
                border.width: 1
                border.color: root.colCardBorder

                Rectangle {
                    objectName: "editorLutViewModeThumb"
                    x: viewModeTrack.inset + (root.viewMode === "list" ? viewModeTrack.segment : 0)
                    y: viewModeTrack.inset
                    width: viewModeTrack.segment
                    height: viewModeTrack.segment
                    radius: Math.max(0, appTheme.controlRadiusSmall - viewModeTrack.inset)
                    color: appTheme.editorListSelectedFillColor
                    Behavior on x {
                        enabled: !appTheme.reduceMotion
                        NumberAnimation {
                            duration: appTheme.motionFoldOpenMs
                            easing.type: appTheme.motionEasing
                        }
                    }
                }

                Row {
                    x: viewModeTrack.inset
                    y: viewModeTrack.inset

                    Repeater {
                        model: [
                            { key: "grid", icon: "qrc:/panel_icons/view-grid.svg",
                              label: qsTr("Grid view"), name: "editorLutGridViewButton" },
                            { key: "list", icon: "qrc:/panel_icons/view-list.svg",
                              label: qsTr("List view"), name: "editorLutListViewButton" }
                        ]
                        delegate: IconActionButton {
                            required property var modelData
                            objectName: modelData.name
                            width: viewModeTrack.segment
                            height: viewModeTrack.segment
                            compact: true
                            selected: root.viewMode === modelData.key
                            showHoverFill: !selected
                            showFocusRing: false
                            iconSrc: modelData.icon
                            actionName: modelData.label
                            iconColorDefault: selected ? appTheme.editorListSelectedInkColor
                                                       : root.colMuted
                            iconColorMuted: root.colMuted
                            iconColorSelected: appTheme.editorListSelectedInkColor
                            fillIdle: "transparent"
                            fillSelected: "transparent"
                            fillHover: appTheme.buttonHoveredFillColor
                            onClicked: root.viewMode = modelData.key
                        }
                    }
                }
            }
        }

        // ── Filter sidebar beside the results ─────────────────────────────
        Item {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Width the docked sidebar takes from the results, fold included.
            readonly property real dockedReveal: root.filterDocked
                                                 ? (root.filterWidth + appTheme.spaceSm)
                                                   * root.filterOpenProgress
                                                 : 0

            EditorLutResultCard {
                id: resultCard
                anchors.fill: parent
                anchors.leftMargin: body.dockedReveal
                browser: root.browser
                target: root.target
                library: root.library
                host: root.host
                viewMode: root.viewMode
                onSearchRequested: root.focusSearch()
            }

            // Clips the full-width sidebar while it folds.
            Item {
                id: filterHost
                objectName: "editorLutFilterHost"
                z: 1
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: root.filterWidth * root.filterOpenProgress
                visible: root.filterOpenProgress > 0.001
                opacity: root.filterOpenProgress
                clip: true

                EditorLutFilterCard {
                    id: filterCard
                    width: root.filterWidth
                    height: parent.height
                    floating: !root.filterDocked
                    browser: root.browser
                }
            }

            // Divider between the docked sidebar and the results.
            Rectangle {
                visible: root.filterDocked && filterHost.visible
                x: filterHost.width + (appTheme.spaceSm * root.filterOpenProgress - width) / 2
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: root.colCardBorder
                opacity: root.filterOpenProgress
            }
        }
    }
}
