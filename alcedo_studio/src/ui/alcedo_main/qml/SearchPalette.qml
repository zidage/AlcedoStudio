import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import QtQuick.Effects

// Global search palette (global_search_redesign_plan.md, Section 2). A launcher, not a result
// browser: recent searches, one command, Inspector filter rows, and up to three photo previews.
// Enter and "View all" apply the search to the Library; the palette shows no result list and
// no result count. The settings drawer below the input row holds the field scope and the
// natural-language switch. Routes, preview rows, and request matching are owned by
// SearchSession (C++); the palette keeps only the query text, the highlight index, and the
// typing debounce. The drawer open state is owned by SearchController (QSettings
// `search/settingsPanelOpen`).
Dialog {
    id: searchPalette
    font.family: appTheme.uiFontFamily

    property var searchController: null
    property var interactionPolicyController: null
    property Item blurSource: null
    property real cornerRadius: 0

    // Admission rule of InteractionPolicyController: while natural-language search is on, the
    // field check boxes are disabled. Enabled when the controller is absent (isolated tests).
    readonly property bool searchFieldFiltersEnabled: interactionPolicyController
        ? interactionPolicyController.canChangeSearchFieldFilters
        : true
    readonly property var searchSession: searchController ? searchController.session : null
    readonly property var results: searchSession ? searchSession.results : []
    readonly property string lastQuery: searchSession ? searchSession.query : ""
    readonly property var activeSearchRequestId: searchSession ? searchSession.activeRequestId : 0
    readonly property bool searchLoading: searchSession ? searchSession.loading : false
    readonly property bool settingsPanelOpen: searchController
                                              ? searchController.searchSettingsPanelOpen : false
    // Field check boxes of the settings drawer: SearchController property and setter of each.
    readonly property var searchFieldOptions: [
        { objectName: "searchFieldFilenameCheckBox", text: qsTr("File name"),
          property: "searchFieldFilenameEnabled", setter: "SetSearchFieldFilenameEnabled" },
        { objectName: "searchFieldExifCheckBox", text: qsTr("EXIF info"),
          property: "searchFieldExifEnabled", setter: "SetSearchFieldExifEnabled" },
        { objectName: "searchFieldAiDescriptionCheckBox", text: qsTr("AI description"),
          property: "searchFieldAiDescriptionEnabled",
          setter: "SetSearchFieldAiDescriptionEnabled" },
        { objectName: "searchFieldAiTagsCheckBox", text: qsTr("AI tags"),
          property: "searchFieldAiTagsEnabled", setter: "SetSearchFieldAiTagsEnabled" }
    ]

    property string queryText: ""
    readonly property string trimmedQuery: queryText.trim()
    // Route of the typed query (SearchSession::RouteFor): "empty", "fuzzy", or "semantic".
    readonly property string queryRoute: {
        if (!searchSession || !searchController) {
            return "empty"
        }
        // RouteFor reads the natural-language switch; the read below makes the binding
        // re-evaluate when the switch changes.
        const naturalLanguageEnabled = searchController.naturalLanguageSearchEnabled
        void naturalLanguageEnabled
        return searchSession.RouteFor(trimmedQuery)
    }
    readonly property bool semanticRoute: queryRoute === "semantic"
    // Reason text of a natural-language search that Enter could not apply to the Library.
    property string semanticApplyError: ""

    readonly property var recentRows: searchController ? searchController.recentSearches : []
    // Filter rows of the empty query and of the typed query (SearchController).
    property var emptyFilterRows: []
    property var matchingFilterRows: []
    readonly property var filterRows: trimmedQuery.length === 0 ? emptyFilterRows
                                                                : matchingFilterRows
    // Rows that Up, Down, hover, and Enter move through, in display order.
    readonly property var navRows: {
        const rows = []
        if (trimmedQuery.length === 0) {
            for (let i = 0; i < recentRows.length; ++i) {
                rows.push({ kind: "recent", index: i })
            }
        } else {
            rows.push({ kind: "command", index: 0 })
        }
        for (let i = 0; i < filterRows.length; ++i) {
            rows.push({ kind: "filter", index: i })
        }
        return rows
    }
    readonly property int recentNavOffset: 0
    readonly property int filterNavOffset: trimmedQuery.length === 0 ? recentRows.length : 1
    property int highlightIndex: -1

    readonly property bool photosLoading: queryRoute === "fuzzy"
                                          && (previewTimer.running || searchLoading)
    readonly property var previewRows: photosLoading ? [] : results.slice(0, 3)
    readonly property string searchErrorText: searchSession ? searchSession.searchErrorText : ""
    // Text of the Photos section when it shows no previews (Section 2.7).
    readonly property string photosStatusText: {
        if (trimmedQuery.length === 0) {
            return ""
        }
        if (semanticRoute) {
            return semanticApplyError.length > 0 ? semanticApplyError
                                                 : qsTr("Press Enter to search by meaning.")
        }
        if (photosLoading) {
            return ""
        }
        if (searchErrorText.length > 0) {
            return searchErrorText
        }
        return results.length === 0 ? qsTr("No matching photos in this folder") : ""
    }
    readonly property bool photosStatusIsError: semanticRoute ? semanticApplyError.length > 0
                                                              : searchErrorText.length > 0
    readonly property bool showEmptyHint: trimmedQuery.length === 0 && recentRows.length === 0
                                          && emptyFilterRows.length === 0

    parent: Overlay.overlay
    modal: true
    focus: visible
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    x: 0
    y: 0
    padding: 0

    function categoryIconSource(category) {
        const key = String(category || "")
        if (key === "camera") {
            return "qrc:/panel_icons/camera.svg"
        }
        if (key === "date") {
            return "qrc:/panel_icons/calendar.svg"
        }
        if (key === "lens") {
            return "qrc:/panel_icons/aperture.svg"
        }
        return "qrc:/panel_icons/sparkles.svg"
    }

    function syncInteractionPolicy() {
        // InteractionPolicyController receives the natural-language switch only through this
        // push, so read the persisted SearchController state before the panel is visible.
        if (interactionPolicyController && searchController) {
            interactionPolicyController.naturalLanguageSearchEnabled =
                searchController.naturalLanguageSearchEnabled
        }
    }

    function resetPreviewState() {
        previewTimer.stop()
        semanticApplyError = ""
        matchingFilterRows = []
        if (searchController) {
            searchController.CancelSearchPreviewThumbnails()
        }
        if (searchSession) {
            searchSession.Reset()
        }
    }

    function openFromCollection() {
        resetPreviewState()
        searchField.text = ""
        emptyFilterRows = searchController ? searchController.SearchRecommendations(8) : []
        highlightIndex = -1
        syncInteractionPolicy()
        open()
        Qt.callLater(function() { searchField.forceActiveFocus() })
    }

    function refreshPreview() {
        previewTimer.stop()
        if (!searchSession || !searchController) {
            return
        }
        semanticApplyError = ""
        // A field query requests SearchSession::kPreviewRows rows; a semantic query requests
        // nothing until Enter applies it to the Library.
        searchSession.Preview(trimmedQuery)
        if (trimmedQuery.length === 0) {
            emptyFilterRows = searchController.SearchRecommendations(8)
            matchingFilterRows = []
        } else {
            matchingFilterRows = searchController.MatchingRecommendations(trimmedQuery, 4)
        }
    }

    function moveHighlight(delta) {
        if (navRows.length === 0) {
            highlightIndex = -1
            return
        }
        if (highlightIndex < 0) {
            highlightIndex = delta > 0 ? 0 : navRows.length - 1
            return
        }
        highlightIndex = Math.max(0, Math.min(navRows.length - 1, highlightIndex + delta))
    }

    function activateNavRow(navIndex) {
        if (navIndex < 0 || navIndex >= navRows.length) {
            return
        }
        const row = navRows[navIndex]
        if (row.kind === "recent") {
            applyRecent(row.index)
        } else if (row.kind === "filter") {
            applyFilter(filterRows[row.index])
        } else {
            applyQuery()
        }
    }

    // Enter: run the highlighted row; with no highlight, apply the typed query. Enter with an
    // empty query and no highlighted row does nothing.
    function handleSearchSubmit() {
        if (highlightIndex >= 0 && highlightIndex < navRows.length) {
            activateNavRow(highlightIndex)
            return
        }
        applyQuery()
    }

    function applyQuery() {
        if (!searchController || !searchSession) {
            return
        }
        const query = searchField.text.trim()
        if (query.length === 0) {
            return
        }
        // Stop the typing debounce first, so a late preview does not replace a rejection text.
        previewTimer.stop()
        if (searchSession.RouteFor(query) === "semantic") {
            const error = searchController.ApplySemanticSearch(query)
            semanticApplyError = error ? String(error) : ""
            if (semanticApplyError.length === 0) {
                close()
            }
            return
        }
        searchController.ApplyFuzzySearch(query)
        close()
    }

    function applyRecent(index) {
        if (!searchController || index < 0 || index >= recentRows.length) {
            return
        }
        searchField.text = String(recentRows[index].query)
        refreshPreview()
        const error = searchController.ApplyRecentSearch(index)
        semanticApplyError = error ? String(error) : ""
        if (semanticApplyError.length === 0) {
            close()
        } else {
            highlightIndex = 0
        }
    }

    function applyFilter(row) {
        if (!searchController || !row) {
            return
        }
        searchController.ApplyRecommendation(String(row.category), String(row.label))
        close()
    }

    function applyPhoto(elementId) {
        if (!searchController || Number(elementId) <= 0) {
            return
        }
        searchController.ApplyExactSearch(Number(elementId))
        close()
    }

    function toggleSettingsPanel() {
        if (searchController) {
            searchController.searchSettingsPanelOpen = !settingsPanelOpen
        }
    }

    onOpened: {
        syncInteractionPolicy()
        // Start the sidecar and load the text encoder while the user types the query.
        if (searchController) {
            searchController.PrewarmNaturalLanguageSearch()
        }
    }
    onClosed: resetPreviewState()

    Timer {
        id: previewTimer
        interval: 140
        repeat: false
        onTriggered: searchPalette.refreshPreview()
    }

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: searchPalette.cornerRadius
            color: "white"
            visible: false
            layer.enabled: true
            layer.smooth: true
        }

        Item {
            anchors.fill: parent
            layer.enabled: true
            layer.smooth: true
            layer.effect: MultiEffect {
                maskEnabled: searchPalette.cornerRadius > 0
                maskSource: backdropMask
            }

            MultiEffect {
                anchors.fill: parent
                source: searchPalette.blurSource
                blurEnabled: searchPalette.blurSource !== null
                blur: 0.68
                blurMax: 72
                saturation: -0.22
                brightness: -0.08
            }

            Rectangle {
                anchors.fill: parent
                color: appTheme.overlayColor
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
        }
    }

    background: Item {}

    contentItem: Item {
        id: canvas
        implicitWidth: searchPalette.width
        implicitHeight: searchPalette.height

        // Geometry of Section 2.1: the palette is min(window - 2 * margin, max width) wide. The
        // settings drawer opens inside the palette and does not change its width.
        readonly property real marginX: appTheme.searchPaletteWindowMarginX
        readonly property real marginY: appTheme.searchPaletteWindowMarginY
        readonly property real mainWidth: Math.max(0, Math.min(appTheme.searchPaletteMaxWidth,
                                                               width - 2 * marginX))
        readonly property real maxShellHeight: Math.max(0, height - 2 * marginY)

        // A click outside the panel closes the searchPalette.
        MouseArea {
            anchors.fill: parent
            onClicked: searchPalette.close()
        }

        // Fold progress of the settings drawer (DESIGN.md Motion, fold rules).
        property real drawerProgress: searchPalette.settingsPanelOpen ? 1 : 0
        Behavior on drawerProgress {
            NumberAnimation {
                duration: appTheme.reduceMotion ? 0
                                                : (searchPalette.settingsPanelOpen ? appTheme.motionFoldOpenMs
                                                                             : appTheme.motionFoldCloseMs)
                easing.type: appTheme.motionEasing
            }
        }
        // False while the drawer folds open or closed.
        readonly property bool drawerSettled: drawerProgress
                                              === (searchPalette.settingsPanelOpen ? 1 : 0)

        Rectangle {
            id: shell
            objectName: "searchPaletteShell"
            x: Math.round((canvas.width - width) / 2)
            width: canvas.mainWidth
            height: Math.min(canvas.maxShellHeight, shellColumn.implicitHeight)
            y: Math.round((canvas.height - height) / 2)
            radius: appTheme.panelRadius
            color: appTheme.bgDeepColor
            border.width: 1
            border.color: appTheme.cardBorderColor
            clip: true

            // The drawer fold drives the height frame by frame; the content fade must not lag it.
            Behavior on height {
                enabled: canvas.drawerSettled
                NumberAnimation {
                    duration: appTheme.reduceMotion ? 0 : appTheme.motionFadeMs
                    easing.type: appTheme.motionEasing
                }
            }

            // A click on the panel surface must not reach the modal backdrop.
            MouseArea {
                anchors.fill: parent
            }

            ColumnLayout {
                id: shellColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                spacing: 0

                ColumnLayout {
                    id: mainColumn
                    Layout.fillWidth: true
                    spacing: 0

                    // ── Input row ──────────────────────────────────────────────────
                    Item {
                        id: inputRow
                        Layout.fillWidth: true
                        Layout.preferredHeight: appTheme.iconButtonHitSize + appTheme.spaceMd

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: appTheme.spaceLg
                            anchors.rightMargin: appTheme.spaceMd
                            spacing: appTheme.spaceMd

                            ColorImage {
                                Layout.preferredWidth: appTheme.iconOpticalSize
                                Layout.preferredHeight: appTheme.iconOpticalSize
                                Layout.alignment: Qt.AlignVCenter
                                source: "qrc:/panel_icons/search.svg"
                                sourceSize.width: appTheme.iconSourceSize
                                sourceSize.height: appTheme.iconSourceSize
                                color: appTheme.textMutedColor
                                fillMode: Image.PreserveAspectFit
                            }

                            Item {
                                Layout.fillWidth: true
                                Layout.fillHeight: true

                                Text {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: searchField.text.length === 0
                                    text: qsTr("Search photos, cameras, lenses, dates")
                                    color: appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeSection
                                    font.weight: appTheme.fontWeightRegular
                                    elide: Text.ElideRight
                                }

                                TextInput {
                                    id: searchField
                                    objectName: "globalSearchField"
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    selectByMouse: true
                                    clip: true
                                    color: appTheme.textColor
                                    selectionColor: appTheme.buttonSelectedFillColor
                                    selectedTextColor: appTheme.textColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeSection
                                    font.weight: appTheme.fontWeightRegular
                                    verticalAlignment: TextInput.AlignVCenter
                                    Accessible.role: Accessible.EditableText
                                    Accessible.name: qsTr("Search photos")
                                    onTextChanged: {
                                        searchPalette.queryText = text
                                        searchPalette.semanticApplyError = ""
                                        searchPalette.highlightIndex = text.trim().length > 0 ? 0 : -1
                                        previewTimer.restart()
                                    }
                                    onAccepted: searchPalette.handleSearchSubmit()
                                    Keys.onUpPressed: searchPalette.moveHighlight(-1)
                                    Keys.onDownPressed: searchPalette.moveHighlight(1)
                                    Keys.onEscapePressed: searchPalette.close()
                                }
                            }

                            Label {
                                objectName: "searchPaletteRouteLabel"
                                Layout.alignment: Qt.AlignVCenter
                                visible: searchPalette.semanticRoute
                                text: qsTr("Natural language")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                            }

                            IconActionButton {
                                objectName: "searchPaletteSettingsButton"
                                Layout.alignment: Qt.AlignVCenter
                                compact: true
                                selected: searchPalette.settingsPanelOpen
                                showFocusRing: false
                                fillIdle: "transparent"
                                iconSrc: "qrc:/panel_icons/adjustments.svg"
                                actionName: qsTr("Search settings")
                                onClicked: searchPalette.toggleSettingsPanel()
                            }

                            IconActionButton {
                                objectName: "searchPaletteCloseButton"
                                Layout.alignment: Qt.AlignVCenter
                                compact: true
                                showFocusRing: false
                                fillIdle: "transparent"
                                iconSrc: "qrc:/panel_icons/close.svg"
                                iconColorDefault: appTheme.iconColor
                                iconColorMuted: appTheme.textMutedColor
                                actionName: qsTr("Close search")
                                onClicked: searchPalette.close()
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: appTheme.dividerColor
                    }

                    // ── Settings drawer ────────────────────────────────────────────
                    // Folds below the input row (DESIGN.md Motion, fold rules): the host
                    // clips, and its height and opacity follow the fold progress.
                    Item {
                        id: settingsDrawer
                        objectName: "searchSettingsDrawer"
                        readonly property real openHeight: settingsColumn.implicitHeight
                                                           + 2 * appTheme.spaceMd + 1
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.round(openHeight * canvas.drawerProgress)
                        visible: canvas.drawerProgress > 0
                        opacity: canvas.drawerProgress
                        clip: true

                        ColumnLayout {
                            id: settingsColumn
                            x: appTheme.spaceLg
                            y: appTheme.spaceMd
                            // The body keeps its open layout and is revealed by the host height.
                            width: settingsDrawer.width - 2 * appTheme.spaceLg
                            spacing: appTheme.spaceSm

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Search in")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                                wrapMode: Text.Wrap
                            }

                            GridLayout {
                                id: fieldGrid
                                Layout.fillWidth: true
                                columns: 4
                                columnSpacing: appTheme.spaceSm
                                rowSpacing: appTheme.spaceXs

                                Repeater {
                                    model: searchPalette.searchFieldOptions

                                    delegate: ThemeCheckBox {
                                        required property var modelData
                                        objectName: modelData.objectName
                                        Layout.preferredWidth: 1
                                        text: modelData.text
                                        checked: searchPalette.searchController
                                                 ? searchPalette.searchController[modelData.property]
                                                 : true
                                        enabled: searchPalette.searchFieldFiltersEnabled
                                        onToggled: function(checked) {
                                            if (searchPalette.searchController) {
                                                searchPalette.searchController[modelData.setter](checked)
                                                previewTimer.restart()
                                            }
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.topMargin: appTheme.spaceXs
                                Layout.bottomMargin: appTheme.spaceXs
                                Layout.preferredHeight: 1
                                color: appTheme.dividerColor
                            }

                            ThemeSwitch {
                                objectName: "naturalLanguageSearchSwitch"
                                // One check-box column wide: the track ends at the first column.
                                Layout.fillWidth: false
                                Layout.preferredWidth: Math.max(
                                    implicitWidth,
                                    (settingsColumn.width - 3 * fieldGrid.columnSpacing) / 4)
                                text: qsTr("Natural language search")
                                checked: searchPalette.searchController
                                         ? searchPalette.searchController.naturalLanguageSearchEnabled
                                         : false
                                onToggled: function(checked) {
                                    if (!searchPalette.searchController) {
                                        return
                                    }
                                    searchPalette.searchController.SetNaturalLanguageSearchEnabled(checked)
                                    if (checked) {
                                        searchPalette.searchController.PrewarmNaturalLanguageSearch()
                                    }
                                    if (searchPalette.interactionPolicyController) {
                                        searchPalette.interactionPolicyController.naturalLanguageSearchEnabled =
                                            checked
                                    }
                                    // Show the preview of the typed query on its new route.
                                    previewTimer.restart()
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Search by meaning with the semantic model. Press Enter to run it.")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                wrapMode: Text.Wrap
                            }
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: appTheme.dividerColor
                        }
                    }

                    // ── Sections ───────────────────────────────────────────────────
                    Flickable {
                        id: bodyFlick
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight,
                                                         Math.max(0, canvas.maxShellHeight
                                                                     - inputRow.height
                                                                     - settingsDrawer.height
                                                                     - footer.height - 2))
                        contentWidth: width
                        contentHeight: bodyColumn.implicitHeight + 2 * appTheme.spaceSm
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        interactive: contentHeight > height

                        ColumnLayout {
                            id: bodyColumn
                            x: appTheme.spaceSm
                            y: appTheme.spaceSm
                            width: bodyFlick.width - 2 * appTheme.spaceSm
                            spacing: appTheme.spaceSm

                            Label {
                                objectName: "searchPaletteEmptyHint"
                                Layout.fillWidth: true
                                Layout.topMargin: appTheme.spaceLg
                                Layout.bottomMargin: appTheme.spaceLg
                                visible: searchPalette.showEmptyHint
                                text: qsTr("Type to search photos")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                            }

                            // Recent searches (empty query).
                            ColumnLayout {
                                objectName: "searchPaletteRecentSection"
                                Layout.fillWidth: true
                                spacing: 0
                                visible: searchPalette.trimmedQuery.length === 0
                                         && searchPalette.recentRows.length > 0

                                SectionHeader {
                                    title: qsTr("Recent searches")
                                }

                                Repeater {
                                    model: searchPalette.recentRows

                                    delegate: PaletteRow {
                                        required property var modelData
                                        required property int index
                                        navIndex: searchPalette.recentNavOffset + index
                                        iconSource: "qrc:/panel_icons/clock-play.svg"
                                        title: String(modelData.query)
                                        trailingText: modelData.route === "semantic"
                                                      ? qsTr("Natural language") : ""
                                        onActivated: searchPalette.applyRecent(index)
                                    }
                                }
                            }

                            // Commands (typed query).
                            ColumnLayout {
                                objectName: "searchPaletteCommandSection"
                                Layout.fillWidth: true
                                spacing: 0
                                visible: searchPalette.trimmedQuery.length > 0

                                SectionHeader {
                                    title: qsTr("Commands")
                                }

                                PaletteRow {
                                    objectName: "searchPaletteCommandRow"
                                    navIndex: 0
                                    iconSource: "qrc:/panel_icons/search.svg"
                                    title: searchPalette.semanticRoute
                                           ? qsTr("Search photos by meaning for \"%1\"")
                                                 .arg(searchPalette.trimmedQuery)
                                           : qsTr("Search photos for \"%1\"")
                                                 .arg(searchPalette.trimmedQuery)
                                    trailingText: qsTr("Enter")
                                    onActivated: searchPalette.applyQuery()
                                }
                            }

                            // Filters (Inspector statistics filters).
                            ColumnLayout {
                                objectName: "searchPaletteFilterSection"
                                Layout.fillWidth: true
                                spacing: 0
                                visible: searchPalette.filterRows.length > 0

                                SectionHeader {
                                    title: qsTr("Filters")
                                }

                                Repeater {
                                    model: searchPalette.filterRows

                                    delegate: PaletteRow {
                                        required property var modelData
                                        required property int index
                                        objectName: "searchPaletteFilterRow"
                                        navIndex: searchPalette.filterNavOffset + index
                                        iconSource: searchPalette.categoryIconSource(modelData.category)
                                        categoryText: modelData.categoryLabel
                                                      ? String(modelData.categoryLabel) : ""
                                        title: String(modelData.label)
                                        trailingText: modelData.count !== undefined
                                                      ? String(modelData.count) : ""
                                        trailingIsData: true
                                        onActivated: searchPalette.applyFilter(modelData)
                                    }
                                }
                            }

                            // Photos (typed query).
                            ColumnLayout {
                                id: photosSection
                                objectName: "searchPalettePhotosSection"
                                Layout.fillWidth: true
                                spacing: appTheme.spaceXs
                                visible: searchPalette.trimmedQuery.length > 0

                                readonly property real tileSpacing: appTheme.spaceMd
                                readonly property real tileWidth: Math.max(
                                    0, Math.floor((width - 2 * appTheme.spaceSm
                                                   - 2 * tileSpacing) / 3))

                                SectionHeader {
                                    title: qsTr("Photos")
                                    actionText: searchPalette.queryRoute === "fuzzy"
                                                && searchPalette.previewRows.length > 0
                                                ? qsTr("View all") : ""
                                    onActionClicked: searchPalette.applyQuery()
                                }

                                Label {
                                    objectName: "searchPalettePhotosStatus"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: appTheme.spaceSm
                                    Layout.rightMargin: appTheme.spaceSm
                                    Layout.bottomMargin: appTheme.spaceSm
                                    visible: searchPalette.photosStatusText.length > 0
                                             && !searchPalette.photosStatusIsError
                                    text: searchPalette.photosStatusText
                                    color: appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeBody
                                    wrapMode: Text.Wrap
                                }

                                AlertBadge {
                                    objectName: "searchPalettePhotosError"
                                    Layout.leftMargin: appTheme.spaceSm
                                    Layout.rightMargin: appTheme.spaceSm
                                    Layout.bottomMargin: appTheme.spaceSm
                                    text: searchPalette.photosStatusIsError ? searchPalette.photosStatusText
                                                                      : ""
                                }

                                // Three placeholder frames while the preview loads.
                                Row {
                                    Layout.leftMargin: appTheme.spaceSm
                                    Layout.bottomMargin: appTheme.spaceSm
                                    spacing: photosSection.tileSpacing
                                    visible: searchPalette.photosLoading

                                    Repeater {
                                        model: searchPalette.photosLoading ? 3 : 0

                                        delegate: Column {
                                            spacing: appTheme.spaceXs

                                            SkeletonBlock {
                                                width: photosSection.tileWidth
                                                height: Math.round(photosSection.tileWidth * 3 / 4)
                                                radius: appTheme.controlRadiusSmall
                                            }

                                            SkeletonBlock {
                                                width: Math.round(photosSection.tileWidth / 2)
                                                height: appTheme.lineHeightCaption
                                                radius: appTheme.badgeRadius
                                                animated: false
                                            }
                                        }
                                    }
                                }

                                Row {
                                    objectName: "searchPalettePreviewRow"
                                    Layout.leftMargin: appTheme.spaceSm
                                    Layout.bottomMargin: appTheme.spaceSm
                                    spacing: photosSection.tileSpacing
                                    visible: searchPalette.previewRows.length > 0
                                             && !searchPalette.semanticRoute

                                    Repeater {
                                        model: searchPalette.semanticRoute ? [] : searchPalette.previewRows

                                        delegate: PreviewFrame {
                                            required property var modelData
                                            tileWidth: photosSection.tileWidth
                                            elementId: Number(modelData.elementId)
                                            imageId: Number(modelData.imageId)
                                            fileName: modelData.fileName
                                                      ? String(modelData.fileName) : ""
                                            initialThumbUrl: modelData.thumbUrl
                                                             ? String(modelData.thumbUrl) : ""
                                            initialThumbMissingSource:
                                                modelData.thumbMissingSource === true
                                            initialThumbErrorText: modelData.thumbErrorText
                                                                   ? String(modelData.thumbErrorText)
                                                                   : ""
                                            onActivated: searchPalette.applyPhoto(elementId)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: appTheme.dividerColor
                }

                // ── Footer ─────────────────────────────────────────────────────────────
                Item {
                    id: footer
                    Layout.fillWidth: true
                    Layout.preferredHeight: appTheme.lineHeightBody + 2 * appTheme.spaceSm

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: appTheme.spaceLg
                        anchors.rightMargin: appTheme.spaceLg
                        spacing: appTheme.spaceSm

                        KeyHint {
                            keyText: qsTr("Enter")
                            actionText: qsTr("Search")
                        }

                        Item {
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }

    // ── Inline components (nested in the root object; qmlcachegen rejects file-level ones) ──

    component SectionHeader: Item {
        id: header

        property string title: ""
        property string actionText: ""

        signal actionClicked()

        Layout.fillWidth: true
        implicitHeight: appTheme.lineHeightCaption + 2 * appTheme.spaceXs

        Label {
            anchors.left: parent.left
            anchors.leftMargin: appTheme.spaceSm
            anchors.verticalCenter: parent.verticalCenter
            text: header.title
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong
            font.capitalization: Font.AllUppercase
        }

        Label {
            id: actionLabel
            anchors.right: parent.right
            anchors.rightMargin: appTheme.spaceSm
            anchors.verticalCenter: parent.verticalCenter
            visible: header.actionText.length > 0
            text: header.actionText
            color: actionMouse.containsMouse ? appTheme.textColor : appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong
            Accessible.role: Accessible.Button
            Accessible.name: header.actionText
            Accessible.onPressAction: header.actionClicked()

            MouseArea {
                id: actionMouse
                anchors.fill: parent
                anchors.margins: -appTheme.spaceXs
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: header.actionClicked()
            }
        }
    }

    component PaletteRow: Rectangle {
        id: row

        property int navIndex: -1
        property string iconSource: ""
        property string categoryText: ""
        property string title: ""
        property string trailingText: ""
        // A trailing count uses the data font.
        property bool trailingIsData: false
        readonly property bool highlighted: searchPalette.highlightIndex === navIndex

        signal activated()

        Layout.fillWidth: true
        implicitHeight: Math.max(appTheme.iconButtonHitSizeCompact,
                                 rowLayout.implicitHeight + appTheme.spaceSm)
        radius: appTheme.controlRadiusSmall
        color: rowMouse.pressed ? appTheme.buttonPressedFillColor
                                : (row.highlighted ? appTheme.hoverColor : "transparent")
        Accessible.role: Accessible.Button
        Accessible.name: row.categoryText.length > 0 ? row.categoryText + " " + row.title
                                                     : row.title
        Accessible.onPressAction: row.activated()

        RowLayout {
            id: rowLayout
            anchors.fill: parent
            anchors.leftMargin: appTheme.spaceSm
            anchors.rightMargin: appTheme.spaceSm
            spacing: appTheme.spaceMd

            ColorImage {
                Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                Layout.alignment: Qt.AlignVCenter
                visible: row.iconSource.length > 0
                source: row.iconSource
                sourceSize.width: appTheme.iconSourceSizeCompact
                sourceSize.height: appTheme.iconSourceSizeCompact
                color: row.highlighted ? appTheme.textColor : appTheme.iconColor
                fillMode: Image.PreserveAspectFit
            }

            Label {
                Layout.alignment: Qt.AlignVCenter
                visible: row.categoryText.length > 0
                text: row.categoryText
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                text: row.title
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: row.highlighted ? appTheme.fontWeightStrong
                                             : appTheme.fontWeightRegular
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            Label {
                Layout.alignment: Qt.AlignVCenter
                visible: row.trailingText.length > 0
                text: row.trailingText
                color: appTheme.textMutedColor
                font.family: row.trailingIsData ? appTheme.dataFontFamily : appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
            }
        }

        MouseArea {
            id: rowMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onContainsMouseChanged: {
                if (containsMouse) {
                    searchPalette.highlightIndex = row.navIndex
                }
            }
            onClicked: row.activated()
        }
    }

    // One photo preview: a 4:3 thumbnail (k512 tier, PreserveAspectCrop) and the file name.
    // The thumbnail pin lives as long as the frame: the frame requests it when it is created
    // and releases it when it is destroyed (a new query or the palette closing).
    component PreviewFrame: Column {
        id: frame

        property real tileWidth: 0
        property int elementId: 0
        property int imageId: 0
        property string fileName: ""
        property string initialThumbUrl: ""
        property bool initialThumbMissingSource: false
        property string initialThumbErrorText: ""
        property string liveThumbUrl: initialThumbUrl
        property bool liveThumbLoading: false
        property bool liveThumbMissingSource: initialThumbMissingSource
        property string liveThumbErrorText: initialThumbErrorText
        property bool thumbnailRequested: false
        readonly property int previewMaxEdge: 512
        readonly property bool thumbReady: liveThumbUrl.length > 0
        // True when the thumbnail image decoded (Image.Ready).
        readonly property bool imageLoaded: thumbImage.status === Image.Ready
        readonly property bool thumbProblem: !thumbReady && !liveThumbLoading
                                             && (liveThumbMissingSource
                                                 || liveThumbErrorText.length > 0)
        readonly property string thumbProblemText: liveThumbErrorText.length > 0
                                                   ? liveThumbErrorText
                                                   : qsTr("Source file is unavailable")

        signal activated()

        width: tileWidth
        spacing: appTheme.spaceXs
        Accessible.role: Accessible.Button
        Accessible.name: fileName
        Accessible.onPressAction: frame.activated()

        Component.onCompleted: {
            if (searchPalette.searchController && elementId > 0 && imageId > 0 && !thumbReady) {
                thumbnailRequested = true
                searchPalette.searchController.SetSearchPreviewThumbnailVisible(elementId, imageId, true,
                                                                          previewMaxEdge)
            }
        }
        Component.onDestruction: {
            if (thumbnailRequested && searchPalette.searchController) {
                searchPalette.searchController.SetSearchPreviewThumbnailVisible(elementId, imageId, false,
                                                                          previewMaxEdge)
            }
        }

        Connections {
            target: searchPalette.searchController
            ignoreUnknownSignals: true

            function onSearchPreviewThumbnailUpdated(updatedElementId, thumbUrl, loading,
                                                     missingSource, errorText) {
                if (Number(updatedElementId) !== frame.elementId) {
                    return
                }
                frame.liveThumbUrl = thumbUrl ? String(thumbUrl) : ""
                frame.liveThumbLoading = loading === true
                frame.liveThumbMissingSource = missingSource === true
                frame.liveThumbErrorText = errorText ? String(errorText) : ""
            }
        }

        Rectangle {
            id: thumbFrame
            width: frame.tileWidth
            height: Math.round(frame.tileWidth * 3 / 4)
            radius: appTheme.controlRadiusSmall
            color: appTheme.bgBaseColor
            border.width: 1
            border.color: frameMouse.containsMouse ? appTheme.textMutedColor
                                                   : appTheme.cardBorderColor

            SkeletonBlock {
                anchors.fill: parent
                anchors.margins: 1
                radius: appTheme.controlRadiusSmall
                visible: frame.liveThumbLoading && !frame.thumbReady
            }

            Image {
                id: thumbImage
                anchors.fill: parent
                anchors.margins: 1
                source: frame.liveThumbUrl
                visible: false
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                sourceSize.width: frame.previewMaxEdge
                sourceSize.height: frame.previewMaxEdge

                onStatusChanged: {
                    if (status === Image.Error && frame.liveThumbUrl.length > 0
                            && frame.liveThumbErrorText.length === 0) {
                        frame.liveThumbErrorText = qsTr("Preview image failed to load")
                    }
                }
            }

            Rectangle {
                id: thumbMask
                anchors.fill: thumbImage
                radius: appTheme.controlRadiusSmall
                visible: false
                layer.enabled: true
            }

            MultiEffect {
                anchors.fill: thumbImage
                source: thumbImage
                maskEnabled: true
                maskSource: thumbMask
                visible: frame.thumbReady
            }

            ColorImage {
                anchors.centerIn: parent
                width: appTheme.iconOpticalSize
                height: appTheme.iconOpticalSize
                visible: !frame.thumbReady && !frame.liveThumbLoading && !frame.thumbProblem
                source: "qrc:/panel_icons/image.svg"
                sourceSize.width: appTheme.iconSourceSize
                sourceSize.height: appTheme.iconSourceSize
                color: appTheme.textMutedColor
                fillMode: Image.PreserveAspectFit
            }

            Label {
                anchors.centerIn: parent
                visible: frame.thumbProblem
                text: "!"
                color: appTheme.textMutedColor
                font.family: appTheme.dataFontFamily
                font.pixelSize: appTheme.fontSizeHeadline
                font.weight: appTheme.fontWeightHeading
            }

            MouseArea {
                id: frameMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: frame.activated()
            }

            ToolTip.visible: frame.thumbProblem && frameMouse.containsMouse
            ToolTip.text: frame.thumbProblemText
            ToolTip.delay: 160
        }

        Label {
            width: frame.tileWidth
            text: frame.fileName
            color: frameMouse.containsMouse ? appTheme.textColor : appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            elide: Text.ElideMiddle
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Footer key hint: the key name and its action as two separate labels.
    component KeyHint: RowLayout {
        id: keyHint

        property string keyText: ""
        property string actionText: ""

        spacing: appTheme.spaceXs

        Label {
            text: keyHint.keyText
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong
        }

        Label {
            text: keyHint.actionText
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }
    }
}
