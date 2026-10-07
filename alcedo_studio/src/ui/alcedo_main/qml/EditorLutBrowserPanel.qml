import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// LUT browser: the `luts` page of the Editor left rail (LUT library plan L6A,
// sections 1.4 and 6.5). The page sits in the rail's card shell: a toolbar
// (search, favorites, sort, grid/list) spans the top, a row of filter combo
// boxes (category, source, brand, print) sits below it, and the results fill
// the rest. Both bind the application-wide browser model and target
// controller, so every LUT surface shows one library and one target. Opening
// or closing this page never submits an edit and never changes the image, the
// node selection, or the route.
//
// Filters: each combo box writes one LutLibraryModel predicate; its first
// choice (All) removes it, and a combo box at All shows its dimension name.
// A combo box lists only choices with matching LUTs (and the chosen one), and
// a chosen value is not highlighted. Opening the page and clearing the filters
// scroll the applied LUT into view.
// The model owns the choices and which dimensions apply (Brand and Print only
// outside User, Print only when the library declares a print). Favorites is
// the star toggle in the toolbar. This page keeps no filter state of its own.
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
    // "grid" or "list".
    property string viewMode: "grid"

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colBase: appTheme.bgBaseColor
    readonly property color colCardBorder: appTheme.cardBorderColor
    readonly property int toolbarChrome: Math.max(
        appTheme.iconOpticalSizeCompact + appTheme.spaceSm,
        appTheme.iconButtonHitSizeCompact - appTheme.spaceSm)
    // Narrowest filter combo box before the filter row wraps to two columns.
    readonly property int filterMinimumWidth: 104

    readonly property bool anyFilterActive: !!browser
                                            && (String(browser.category) !== "all"
                                                || String(browser.source) !== ""
                                                || String(browser.brand) !== ""
                                                || String(browser.print) !== ""
                                                || browser.favoritesOnly === true)

    // Rail scroll restore (EditorWorkspaceRail captures these per page). An
    // applied LUT in the results takes precedence over the stored position.
    readonly property real listContentY: resultCard.listContentY
    function restoreListContentY(y) {
        resultCard.restoreListContentY(y)
        Qt.callLater(resultCard.revealAppliedEntry)
    }
    Component.onCompleted: Qt.callLater(resultCard.revealAppliedEntry)
    function focusSearch() {
        searchInput.forceActiveFocus()
        searchInput.selectAll()
    }

    // Write one filter predicate; @p value is a choice `value` of that dimension.
    function chooseFilter(dimension, value) {
        if (!browser)
            return
        if (dimension === "category")
            browser.category = value
        else if (dimension === "source")
            browser.source = value
        else if (dimension === "brand")
            browser.brand = value
        else if (dimension === "print")
            browser.print = value
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

    // One filter dimension as a sunken combo box over the model's choices
    // `{value, label, count, selected}`. At All it shows the dimension name,
    // muted. Choices without a matching LUT are left out; All and the chosen
    // choice are always listed.
    component FilterCombo: ComboBox {
        id: combo

        property string title: ""
        property string dimension: ""
        property var choices: []
        readonly property var shownChoices: {
            const shown = []
            for (let i = 0; i < choices.length; ++i) {
                const choice = choices[i]
                if (i === 0 || choice.selected === true || Number(choice.count) > 0)
                    shown.push(choice)
            }
            return shown
        }
        readonly property int selectedIndex: {
            for (let i = 0; i < shownChoices.length; ++i) {
                if (shownChoices[i].selected === true)
                    return i
            }
            return 0
        }
        readonly property bool chosen: selectedIndex > 0

        Layout.fillWidth: true
        Layout.preferredWidth: root.filterMinimumWidth
        Layout.minimumWidth: 0
        Layout.preferredHeight: root.toolbarChrome
        implicitHeight: root.toolbarChrome
        model: shownChoices
        textRole: "label"
        displayText: chosen && shownChoices[selectedIndex] ? String(shownChoices[selectedIndex].label)
                                                           : title
        // Keyboard focus only: a pointer choice leaves no focus border behind.
        focusPolicy: Qt.TabFocus
        activeFocusOnTab: true
        Accessible.role: Accessible.ComboBox
        Accessible.name: title

        // The choices list is rebuilt on every filter change; follow its selection.
        onSelectedIndexChanged: currentIndex = selectedIndex
        onModelChanged: currentIndex = selectedIndex
        Component.onCompleted: currentIndex = selectedIndex
        onActivated: function(index) {
            if (index >= 0 && index < shownChoices.length)
                root.chooseFilter(dimension, String(shownChoices[index].value))
        }

        background: Rectangle {
            implicitHeight: root.toolbarChrome
            radius: appTheme.controlRadiusSmall
            color: root.colBase
            border.width: 1
            border.color: combo.visualFocus || combo.hovered ? root.colMuted : root.colCardBorder
        }

        contentItem: Text {
            leftPadding: appTheme.spaceSm
            rightPadding: appTheme.spaceMd + appTheme.spaceXs
            text: combo.displayText
            color: combo.chosen && combo.enabled ? root.colText : root.colMuted
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightRegular
        }

        indicator: Text {
            x: combo.width - width - appTheme.spaceSm
            y: (combo.height - height) / 2
            text: "▾"
            color: root.colMuted
            opacity: combo.enabled ? 1.0 : 0.45
            font.pixelSize: appTheme.fontSizeCaption
        }

        popup: Popup {
            y: combo.height + 2
            width: Math.max(combo.width, 180)
            implicitHeight: Math.min(contentItem.implicitHeight + 2, 320)
            padding: 1
            margins: appTheme.spaceXs

            background: Rectangle {
                radius: appTheme.controlRadiusSmall
                color: appTheme.cardSurfaceColor
                border.width: 1
                border.color: root.colCardBorder
            }

            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: combo.popup.visible ? combo.delegateModel : null
                currentIndex: combo.highlightedIndex
                boundsBehavior: Flickable.StopAtBounds
                ScrollIndicator.vertical: ScrollIndicator {}
            }
        }

        delegate: ItemDelegate {
            id: choiceRow
            required property int index
            required property var modelData
            readonly property bool current: modelData ? modelData.selected === true : false

            objectName: "editorLutFilterChoice_" + combo.dimension + "_"
                        + (modelData ? String(modelData.value) : "")
            width: ListView.view ? ListView.view.width : combo.width
            leftPadding: appTheme.spaceSm
            rightPadding: appTheme.spaceSm
            topPadding: appTheme.spaceXs
            bottomPadding: appTheme.spaceXs
            implicitHeight: Math.max(appTheme.lineHeightBody + appTheme.spaceSm,
                                     choiceLabel.implicitHeight + topPadding + bottomPadding)
            highlighted: combo.highlightedIndex === index

            background: Rectangle {
                radius: appTheme.badgeRadius
                color: choiceRow.highlighted || choiceRow.hovered ? appTheme.buttonHoveredFillColor
                                                                  : "transparent"
            }

            contentItem: RowLayout {
                spacing: appTheme.spaceSm

                Text {
                    id: choiceLabel
                    Layout.fillWidth: true
                    text: choiceRow.modelData ? String(choiceRow.modelData.label) : ""
                    color: root.colText
                    wrapMode: Text.Wrap
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: choiceRow.current ? appTheme.fontWeightStrong
                                                   : appTheme.fontWeightRegular
                }

                Text {
                    visible: choiceRow.current
                    text: "✓"
                    color: root.colText
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appTheme.spaceSm
        spacing: appTheme.spaceSm

        // ── Toolbar: search, favorites, order, and layout ─────────────────
        RowLayout {
            objectName: "editorLutToolbar"
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

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

            ToolbarButton {
                objectName: "editorLutFavoritesToggle"
                readonly property bool showingFavorites: !!root.browser
                                                         && root.browser.favoritesOnly === true
                selected: showingFavorites
                iconSrc: "qrc:/panel_icons/star.svg"
                iconColorDefault: showingFavorites ? appTheme.editorListFavoriteActiveColor
                                                   : appTheme.iconColor
                iconColorSelected: iconColorDefault
                actionName: showingFavorites ? qsTr("Show all LUTs") : qsTr("Show favorites only")
                onClicked: {
                    if (root.browser)
                        root.browser.favoritesOnly = !showingFavorites
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

        // ── Filters: one combo box per dimension ──────────────────────────
        RowLayout {
            objectName: "editorLutFilterBar"
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

            GridLayout {
                id: filterGrid
                objectName: "editorLutFilterGrid"
                readonly property int visibleCount: (categoryFilter.visible ? 1 : 0)
                                                    + (sourceFilter.visible ? 1 : 0)
                                                    + (brandFilter.visible ? 1 : 0)
                                                    + (printFilter.visible ? 1 : 0)
                readonly property int fittingColumns: Math.max(1, Math.floor(
                    (width + columnSpacing) / (root.filterMinimumWidth + columnSpacing)))
                Layout.fillWidth: true
                columns: Math.max(1, Math.min(visibleCount,
                                              fittingColumns >= visibleCount ? visibleCount : 2))
                columnSpacing: appTheme.spaceXs
                rowSpacing: appTheme.spaceXs

                FilterCombo {
                    id: categoryFilter
                    objectName: "editorLutCategoryFilter"
                    title: qsTr("Category")
                    dimension: "category"
                    choices: root.browser ? root.browser.categoryChoices : []
                }

                FilterCombo {
                    id: sourceFilter
                    objectName: "editorLutSourceFilter"
                    visible: shownChoices.length > 1
                    title: qsTr("Source")
                    dimension: "source"
                    choices: root.browser ? root.browser.sourceChoices : []
                }

                // Brand and Print narrow film simulations; the model lists no choices for User.
                FilterCombo {
                    id: brandFilter
                    objectName: "editorLutBrandFilter"
                    visible: !!root.browser && root.browser.filmFiltersAvailable
                             && shownChoices.length > 1
                    title: qsTr("Brand")
                    dimension: "brand"
                    choices: root.browser ? root.browser.brandChoices : []
                }

                FilterCombo {
                    id: printFilter
                    objectName: "editorLutPrintFilter"
                    visible: !!root.browser && root.browser.filmFiltersAvailable
                             && (root.browser.printFilterAvailable
                                 || String(root.browser.print) !== "")
                             && shownChoices.length > 1
                    title: qsTr("Print")
                    dimension: "print"
                    choices: root.browser ? root.browser.printChoices : []
                }
            }

            ToolbarButton {
                objectName: "editorLutClearFilters"
                Layout.alignment: Qt.AlignTop
                visible: root.anyFilterActive
                iconSrc: "qrc:/panel_icons/close.svg"
                actionName: qsTr("Clear filters")
                focusOnPointerPress: false
                onClicked: resultCard.clearFiltersAndReveal()
            }
        }

        EditorLutResultCard {
            id: resultCard
            Layout.fillWidth: true
            Layout.fillHeight: true
            browser: root.browser
            target: root.target
            library: root.library
            host: root.host
            viewMode: root.viewMode
            onSearchRequested: root.focusSearch()
        }
    }
}
