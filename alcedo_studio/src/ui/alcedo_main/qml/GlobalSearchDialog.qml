import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Effects

Dialog {
    id: dialog
    font.family: appTheme.uiFontFamily

    property var searchController: null
    property var interactionPolicyController: null
    // Gate driven by InteractionPolicyController: when natural-language search is
    // on, the field-filter checkboxes are disabled (mutual exclusion). Defaults
    // to enabled when the controller is unavailable (e.g. in isolated tests).
    readonly property bool searchFieldFiltersEnabled: interactionPolicyController
        ? interactionPolicyController.canChangeSearchFieldFilters
        : true
    property var theme
    property var recommendations: []
    property var results: []
    property var previewThumbs: ({})
    property string lastQuery: ""
    property int resultPageSize: 24
    property int resultWindowCapacity: 48
    property int resultWindowStart: 0
    property int lastWindowDropCount: 0
    property int lastWindowPrependCount: 0
    property int searchOffset: 0
    property int searchTotal: 0
    property bool searchHasMore: false
    property bool searchHasPrevious: false
    property bool searchLoading: false
    property int searchRequestGeneration: 0
    property string pendingSearchKind: ""
    property string pendingSearchQuery: ""
    property int pendingSearchOffset: 0
    property int pendingSearchLimit: 0
    property string pendingSearchMode: "replace"
    property int pendingSearchGeneration: 0
    property var activeSearchRequestId: 0
    property bool previewSyncForcePending: false
    // Natural-language-search control-layer state (5B). The toggle itself lives on
    // searchController; these track the in-dialog preview lifecycle so QML only
    // reflects state and never decides runtime behavior.
    property bool naturalLanguagePreviewActive: false
    property string currentRoute: ""
    property string naturalLanguageStatusText: ""
    property Item blurSource: null
    property real cornerRadius: 0

    property color panelColor: theme ? theme.colBgPanel : "#1C1C1D"
    property color canvasColor: theme ? theme.colBgCanvas : "#111214"
    property color textColor: theme ? theme.colText : "#F5F1EA"
    property color mutedTextColor: theme ? theme.colTextMuted : "#AAA59D"
    property color accentColor: theme ? theme.colAccentSecondary : "#6D93B7"
    property color hoverColor: theme ? theme.colHover : Qt.rgba(1, 1, 1, 0.07)
    property color dividerColor: theme ? theme.colDivider : Qt.rgba(1, 1, 1, 0.08)
    property color overlayColor: theme ? theme.colOverlay : Qt.rgba(11 / 255, 12 / 255, 14 / 255, 0.60)
    property string headlineFontFamily: appTheme.headlineFontFamily
    readonly property string dataFontFamily: appTheme.dataFontFamily

    parent: Overlay.overlay
    modal: true
    focus: visible
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    x: 0
    y: 0
    padding: 0

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    function suggestionIconSource(category) {
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
        return "qrc:/panel_icons/search.svg"
    }

    function resetPreviewState() {
        previewTimer.stop()
        searchExecutionTimer.stop()
        searchRequestGeneration += 1
        if (searchController) {
            searchController.CancelSearchPreviewThumbnails()
        }
        previewThumbs = ({})
        lastWindowDropCount = 0
        lastWindowPrependCount = 0
        naturalLanguagePreviewActive = false
        currentRoute = ""
        naturalLanguageStatusText = ""
        pendingSearchKind = ""
        pendingSearchQuery = ""
        activeSearchRequestId = 0
        searchLoading = false
    }

    function openFromCollection() {
        resetPreviewState()
        lastQuery = ""
        searchField.text = ""
        results = []
        resultWindowStart = 0
        lastWindowDropCount = 0
        searchOffset = 0
        searchTotal = 0
        searchHasMore = false
        searchHasPrevious = false
        searchLoading = false
        recommendations = searchController ? searchController.SearchRecommendations(12) : []
        // Re-sync the interaction-policy NL gate to the persisted SearchController
        // state BEFORE opening. The policy controller's naturalLanguageSearchEnabled
        // copy is only pushed imperatively (on toggle below), so after a restart it
        // lags the persisted value and the field-filter checkboxes would wrongly
        // enable. Doing this here (synchronously, before open()) makes the drawer's
        // disabled state correct from the first visible frame; onOpened is a backup
        // for any direct open() call.
        if (interactionPolicyController && searchController) {
            interactionPolicyController.naturalLanguageSearchEnabled =
                searchController.naturalLanguageSearchEnabled
        }
        open()
        Qt.callLater(function() { searchField.forceActiveFocus() })
    }

    function visibleResultContains(elementId) {
        const target = Number(elementId)
        for (let i = 0; i < results.length; ++i) {
            const row = results[i]
            if (row && Number(row.elementId) === target) {
                return true
            }
        }
        return false
    }

    function prunePreviewThumbs() {
        const visibleKeys = ({})
        for (let i = 0; i < results.length; ++i) {
            const row = results[i]
            if (row && Number(row.elementId) > 0) {
                visibleKeys[String(Number(row.elementId))] = true
            }
        }

        const next = ({})
        const keys = Object.keys(previewThumbs)
        for (let i = 0; i < keys.length; ++i) {
            const key = keys[i]
            if (visibleKeys[key] === true) {
                next[key] = previewThumbs[key]
            }
        }
        previewThumbs = next
    }

    function refreshVisiblePreviewDelegates(force) {
        if (!resultList || !resultList.visible || !resultList.contentItem) {
            return
        }
        const children = resultList.contentItem.children
        for (let i = 0; i < children.length; ++i) {
            const child = children[i]
            if (child && typeof child.syncPreviewThumbnailLifetime === "function") {
                child.syncPreviewThumbnailLifetime(force === true)
            }
        }
    }

    function scheduleVisiblePreviewSync(force) {
        previewSyncForcePending = previewSyncForcePending || force === true
        previewSyncTimer.restart()
    }

    function readPreviewResponse(response, mode) {
        const rows = response && response.rows ? response.rows : []
        lastWindowDropCount = 0
        lastWindowPrependCount = 0
        if (mode === "replace") {
            if (rows.length > resultWindowCapacity) {
                const dropCount = rows.length - resultWindowCapacity
                results = rows.slice(dropCount)
                resultWindowStart = (response && response.offset !== undefined
                                     ? Number(response.offset) : 0) + dropCount
                lastWindowDropCount = dropCount
            } else {
                results = rows
                resultWindowStart = response && response.offset !== undefined ? Number(response.offset) : 0
            }
        } else if (mode === "append") {
            let nextRows = results.concat(rows)
            if (nextRows.length > resultWindowCapacity) {
                const dropCount = nextRows.length - resultWindowCapacity
                nextRows = nextRows.slice(dropCount)
                resultWindowStart += dropCount
                lastWindowDropCount = dropCount
            }
            results = nextRows
        } else if (mode === "prepend") {
            let nextRows = rows.concat(results)
            resultWindowStart = response && response.offset !== undefined ? Number(response.offset) : 0
            lastWindowPrependCount = rows.length
            if (nextRows.length > resultWindowCapacity) {
                nextRows = nextRows.slice(0, resultWindowCapacity)
            }
            results = nextRows
        }
        prunePreviewThumbs()
        searchOffset = resultWindowStart + results.length
        searchTotal = response && response.total !== undefined ? Number(response.total) : results.length
        searchHasPrevious = resultWindowStart > 0
        searchHasMore = response && response.hasMore !== undefined
                ? response.hasMore === true
                : searchOffset < searchTotal
        scheduleVisiblePreviewSync(true)
    }

    function resultCountText() {
        if (searchTotal <= 0) {
            return qsTr("%1 matches").arg(results.length)
        }
        if (results.length === 0) {
            return qsTr("0 of %1 matches").arg(searchTotal)
        }
        const first = resultWindowStart + 1
        const last = resultWindowStart + results.length
        return qsTr("%1-%2 of %3 matches").arg(first).arg(last).arg(searchTotal)
    }

    function refreshPreview() {
        if (!searchController) {
            return
        }
        const query = searchField.text.trim()
        lastQuery = query
        naturalLanguagePreviewActive = false
        naturalLanguageStatusText = ""
        searchController.CancelSearchPreviewThumbnails()
        previewThumbs = ({})
        resultWindowStart = 0
        lastWindowDropCount = 0
        lastWindowPrependCount = 0
        searchOffset = 0
        searchTotal = 0
        searchHasMore = false
        searchHasPrevious = false
        if (query.length === 0) {
            searchRequestGeneration += 1
            searchExecutionTimer.stop()
            currentRoute = "empty"
            results = []
            recommendations = searchController.SearchRecommendations(12)
            searchLoading = false
            return
        }
        beginSearchRequest("preview", query, 0, resultPageSize, "replace")
    }

    // Unified submit entry for Enter and the Search button (5B). Routing is
    // decided in C++ (ClassifyQuery); QML only branches on the returned route.
    function handleSearchSubmit() {
        if (!searchController) {
            return
        }
        const query = searchField.text.trim()
        if (query.length === 0) {
            return
        }
        if (searchController.naturalLanguageSearchEnabled
                && searchController.ClassifyQuery(query) === "semantic") {
            runSemanticSubmit(0, "replace")
        } else {
            applyBroadSearch()
        }
    }

    function runSemanticSubmit(offset, mode) {
        if (!searchController) {
            return
        }
        const query = searchField.text.trim()
        if (query.length === 0) {
            return
        }
        lastQuery = query
        naturalLanguagePreviewActive = true
        naturalLanguageStatusText = ""
        searchController.CancelSearchPreviewThumbnails()
        if (mode === "replace") {
            previewThumbs = ({})
            resultWindowStart = 0
            lastWindowDropCount = 0
            lastWindowPrependCount = 0
            searchOffset = 0
            searchTotal = 0
            searchHasMore = false
            searchHasPrevious = false
        }
        beginSearchRequest("submit", query, offset, resultPageSize, mode)
    }

    function beginSearchRequest(kind, query, offset, limit, mode) {
        searchRequestGeneration += 1
        pendingSearchKind = kind
        pendingSearchQuery = query
        pendingSearchOffset = offset
        pendingSearchLimit = limit
        pendingSearchMode = mode
        pendingSearchGeneration = searchRequestGeneration
        activeSearchRequestId = 0
        searchLoading = true
        searchExecutionTimer.restart()
    }

    function finishSearchRequest(generation) {
        if (generation === searchRequestGeneration) {
            pendingSearchKind = ""
            searchLoading = false
        }
    }

    function executePendingSearch() {
        if (!searchController || pendingSearchKind.length === 0) {
            finishSearchRequest(pendingSearchGeneration)
            return
        }
        if (pendingSearchKind === "submit") {
            activeSearchRequestId = searchController.RequestSubmitSearch(pendingSearchQuery,
                                                                         pendingSearchOffset,
                                                                         pendingSearchLimit,
                                                                         pendingSearchMode)
            return
        }

        const generation = pendingSearchGeneration
        const query = pendingSearchQuery
        const mode = pendingSearchMode
        const response = searchController.SearchPreview(query, pendingSearchOffset,
                                                        pendingSearchLimit)
        if (generation !== searchRequestGeneration || query !== lastQuery) {
            return
        }
        applySearchResponse(activeSearchRequestId, mode, response)
    }

    function applySearchResponse(requestId, mode, response) {
        if ((Number(activeSearchRequestId) !== 0
             && Number(requestId) !== Number(activeSearchRequestId))
                || pendingSearchGeneration !== searchRequestGeneration
                || pendingSearchQuery !== lastQuery) {
            return
        }
        const generation = pendingSearchGeneration
        currentRoute = response && response.route ? String(response.route) : "semantic"
        if (response && response.tooLong === true) {
            naturalLanguageStatusText = qsTr("Query is too long for natural language search")
        } else if (response && response.semanticUnavailable === true) {
            naturalLanguageStatusText = response.semanticErrorText
                    ? String(response.semanticErrorText)
                    : qsTr("Natural language search is not available yet")
        } else if (response && response.awaitingSubmit === true) {
            naturalLanguageStatusText = qsTr("Press Enter or click Search for natural language search")
        }
        readPreviewResponse(response, mode)
        if (mode === "append" && lastWindowDropCount > 0) {
            const droppedHeight = lastWindowDropCount * 82
            Qt.callLater(function() {
                resultList.contentY = Math.max(0, resultList.contentY - droppedHeight)
                dialog.finishSearchRequest(generation)
            })
            return
        }
        if (mode === "prepend" && lastWindowPrependCount > 0) {
            const prependedHeight = lastWindowPrependCount * 82
            Qt.callLater(function() {
                resultList.contentY = resultList.contentY + prependedHeight
                dialog.finishSearchRequest(generation)
            })
            return
        }
        finishSearchRequest(generation)
    }

    function loadMorePreview() {
        if (!searchController || searchLoading || !searchHasMore) {
            return
        }
        const query = searchField.text.trim()
        if (query.length === 0 || query !== lastQuery) {
            return
        }
        beginSearchRequest(naturalLanguagePreviewActive ? "submit" : "preview", query, searchOffset,
                           resultPageSize, "append")
    }

    function loadPreviousPreview() {
        if (!searchController || searchLoading || !searchHasPrevious) {
            return
        }
        const query = searchField.text.trim()
        if (query.length === 0 || query !== lastQuery) {
            return
        }
        const nextOffset = Math.max(0, resultWindowStart - resultPageSize)
        const nextLimit = resultWindowStart - nextOffset
        if (nextLimit <= 0) {
            return
        }
        beginSearchRequest(naturalLanguagePreviewActive ? "submit" : "preview", query, nextOffset,
                           nextLimit, "prepend")
    }

    function applyBroadSearch() {
        if (!searchController) {
            return
        }
        searchController.ApplyFuzzySearch(searchField.text)
        close()
    }

    function applyRecommendation(row) {
        if (!searchController || !row) {
            return
        }
        searchController.ApplyFuzzySearch(row.query ? String(row.query) : String(row.label))
        close()
    }

    function applyExact(row) {
        if (!searchController || !row) {
            return
        }
        searchController.ApplyExactSearch(Number(row.elementId))
        close()
    }

    onOpened: {
        recommendations = searchController ? searchController.SearchRecommendations(12) : []
        // Re-sync the interaction-policy NL gate to the persisted SearchController
        // state. The policy controller's naturalLanguageSearchEnabled copy is only
        // pushed imperatively (on toggle below), so after a restart it lags the
        // persisted value and the field-filter checkboxes would wrongly enable.
        // Sync on every open so the drawer shows the correct disabled state.
        if (interactionPolicyController && searchController) {
            interactionPolicyController.naturalLanguageSearchEnabled =
                searchController.naturalLanguageSearchEnabled
        }
    }
    onClosed: resetPreviewState()

    Connections {
        target: searchController
        ignoreUnknownSignals: true

        function onSearchPreviewThumbnailUpdated(elementId, dataUrl, loading, missingSource, errorText) {
            if (!dialog.visibleResultContains(elementId)) {
                return
            }
            const next = Object.assign({}, dialog.previewThumbs)
            next[String(Number(elementId))] = {
                url: dataUrl ? String(dataUrl) : "",
                loading: loading === true,
                missingSource: missingSource === true,
                errorText: errorText ? String(errorText) : ""
            }
            dialog.previewThumbs = next
        }

        function onSearchResponseReady(requestId, mode, response) {
            dialog.applySearchResponse(requestId, mode, response)
        }
    }

    Timer {
        id: previewTimer
        interval: 140
        repeat: false
        onTriggered: dialog.refreshPreview()
    }

    Timer {
        id: previewSyncTimer
        interval: 0
        repeat: false
        onTriggered: {
            const force = dialog.previewSyncForcePending
            dialog.previewSyncForcePending = false
            dialog.refreshVisiblePreviewDelegates(force)
        }
    }

    Timer {
        id: searchExecutionTimer
        interval: 24
        repeat: false
        onTriggered: dialog.executePendingSearch()
    }

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: dialog.cornerRadius
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
                maskEnabled: dialog.cornerRadius > 0
                maskSource: backdropMask
            }

            MultiEffect {
                anchors.fill: parent
                source: dialog.blurSource
                blurEnabled: dialog.blurSource !== null
                blur: 0.68
                blurMax: 72
                saturation: -0.22
                brightness: -0.08
            }

            Rectangle {
                anchors.fill: parent
                color: dialog.overlayColor
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
        }
    }

    background: Rectangle {
        radius: 0
        color: "transparent"
    }

    contentItem: Item {
        implicitWidth: dialog.width
        implicitHeight: dialog.height

        Rectangle {
            id: shell
            anchors.centerIn: parent
            width: Math.min(parent.width - 56, 1120)
            height: Math.min(parent.height - 72, 710)
            radius: 16
            color: Qt.rgba(dialog.panelColor.r, dialog.panelColor.g, dialog.panelColor.b, 0.94)
            border.width: 1
            border.color: dialog.withAlpha(dialog.textColor, 0.09)
            clip: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 108

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 28
                        anchors.rightMargin: 24
                        spacing: 18

                        Image {
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 26
                            source: "qrc:/panel_icons/search.svg"
                            sourceSize.width: 26
                            sourceSize.height: 26
                            fillMode: Image.PreserveAspectFit
                            opacity: 0.76
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 58
                            radius: 10
                            color: dialog.withAlpha(dialog.canvasColor, 0.46)
                            border.width: 1
                            border.color: searchField.activeFocus
                                          ? dialog.withAlpha(dialog.accentColor, 0.48)
                                          : dialog.withAlpha(dialog.textColor, 0.08)

                            Text {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 16
                                anchors.rightMargin: 16
                                visible: searchField.text.length === 0
                                text: qsTr("Search photos, cameras, lenses, dates...")
                                color: dialog.withAlpha(dialog.textColor, 0.42)
                                font.family: dialog.dataFontFamily
                                font.pixelSize: 17
                                font.weight: 500
                                elide: Text.ElideRight
                            }

                            TextInput {
                                id: searchField
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 16
                                anchors.rightMargin: 16
                                height: Math.max(30, implicitHeight)
                                selectByMouse: true
                                clip: true
                                color: dialog.textColor
                                selectionColor: dialog.withAlpha(dialog.accentColor, 0.36)
                                selectedTextColor: dialog.textColor
                                font.family: dialog.dataFontFamily
                                font.pixelSize: 17
                                font.weight: 600
                                verticalAlignment: TextInput.AlignVCenter
                                onTextChanged: previewTimer.restart()
                                onAccepted: dialog.handleSearchSubmit()
                                Keys.onEscapePressed: dialog.close()
                            }
                        }

                        Button {
                            id: searchSettingsButton
                            Layout.preferredHeight: 40
                            text: qsTr("Search settings ▾")
                            contentItem: Text {
                                text: searchSettingsButton.text
                                color: searchSettingsButton.enabled
                                       ? dialog.textColor
                                       : dialog.withAlpha(dialog.textColor, 0.4)
                                font.family: dialog.dataFontFamily
                                font.pixelSize: 14
                                font.weight: 660
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 8
                                color: searchSettingsPopup.visible
                                       ? dialog.withAlpha(dialog.accentColor, 0.34)
                                       : (searchSettingsButton.down
                                              ? dialog.withAlpha(dialog.accentColor, 0.55)
                                              : (searchSettingsButton.hovered
                                                     ? dialog.withAlpha(dialog.accentColor, 0.22)
                                                     : "transparent"))
                                border.width: searchSettingsPopup.visible ? 1 : 0
                                border.color: dialog.withAlpha(dialog.accentColor, 0.48)
                            }
                            onClicked: {
                                if (searchSettingsPopup.visible) {
                                    searchSettingsPopup.close()
                                } else {
                                    searchSettingsPopup.open()
                                }
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Search settings — choose which fields the search scans, or enable natural-language search")
                        }

                        Button {
                            Layout.preferredHeight: 40
                            text: qsTr("Search")
                            enabled: searchField.text.trim().length > 0 && !dialog.searchLoading
                            onClicked: dialog.handleSearchSubmit()
                            Material.foreground: dialog.textColor
                            contentItem: Text {
                                text: parent.text
                                color: parent.enabled ? dialog.textColor
                                                      : dialog.withAlpha(dialog.textColor, 0.4)
                                font.family: dialog.dataFontFamily
                                font.pixelSize: 14
                                font.weight: 660
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 8
                                color: parent.down
                                       ? dialog.withAlpha(dialog.accentColor, 0.55)
                                       : (parent.hovered
                                              ? dialog.withAlpha(dialog.accentColor, 0.34)
                                              : dialog.withAlpha(dialog.accentColor, 0.22))
                            }
                        }

                        ToolButton {
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: 40
                            text: "\u00d7"
                            font.pixelSize: 28
                            font.weight: 300
                            Material.foreground: dialog.withAlpha(dialog.textColor, 0.78)
                            onClicked: dialog.close()
                            background: Rectangle {
                                radius: 8
                                color: parent.down
                                       ? dialog.withAlpha(dialog.textColor, 0.08)
                                       : (parent.hovered ? dialog.hoverColor : "transparent")
                            }
                        }
                    }

                    Popup {
                        id: searchSettingsPopup
                        parent: searchSettingsButton
                        x: searchSettingsButton.width - width
                        y: searchSettingsButton.height + 8
                        width: 288
                        modal: false
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                        padding: 16

                        background: Rectangle {
                            radius: 10
                            color: Qt.rgba(dialog.panelColor.r, dialog.panelColor.g,
                                           dialog.panelColor.b, 0.98)
                            border.width: 1
                            border.color: dialog.withAlpha(dialog.textColor, 0.12)
                        }

                        contentItem: ColumnLayout {
                            spacing: 10

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Search conditions")
                                color: dialog.withAlpha(dialog.textColor, 0.68)
                                font.pixelSize: 13
                                font.weight: 760
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: !dialog.searchFieldFiltersEnabled
                                text: dialog.interactionPolicyController
                                      ? dialog.interactionPolicyController.searchFieldFiltersReason
                                      : ""
                                color: dialog.withAlpha(dialog.textColor, 0.46)
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                            }

                            CheckBox {
                                Layout.fillWidth: true
                                text: qsTr("Filename")
                                checked: searchController ? searchController.searchFieldFilenameEnabled : true
                                enabled: dialog.searchFieldFiltersEnabled
                                onToggled: {
                                    if (searchController) {
                                        searchController.SetSearchFieldFilenameEnabled(checked)
                                        previewTimer.restart()
                                    }
                                }
                                Material.foreground: dialog.textColor
                                Material.accent: dialog.accentColor
                            }

                            CheckBox {
                                Layout.fillWidth: true
                                text: qsTr("EXIF info")
                                checked: searchController ? searchController.searchFieldExifEnabled : true
                                enabled: dialog.searchFieldFiltersEnabled
                                onToggled: {
                                    if (searchController) {
                                        searchController.SetSearchFieldExifEnabled(checked)
                                        previewTimer.restart()
                                    }
                                }
                                Material.foreground: dialog.textColor
                                Material.accent: dialog.accentColor
                            }

                            CheckBox {
                                Layout.fillWidth: true
                                text: qsTr("AI description")
                                checked: searchController ? searchController.searchFieldAiDescriptionEnabled : true
                                enabled: dialog.searchFieldFiltersEnabled
                                onToggled: {
                                    if (searchController) {
                                        searchController.SetSearchFieldAiDescriptionEnabled(checked)
                                        previewTimer.restart()
                                    }
                                }
                                Material.foreground: dialog.textColor
                                Material.accent: dialog.accentColor
                            }

                            CheckBox {
                                Layout.fillWidth: true
                                text: qsTr("AI tags")
                                checked: searchController ? searchController.searchFieldAiTagsEnabled : true
                                enabled: dialog.searchFieldFiltersEnabled
                                onToggled: {
                                    if (searchController) {
                                        searchController.SetSearchFieldAiTagsEnabled(checked)
                                        previewTimer.restart()
                                    }
                                }
                                Material.foreground: dialog.textColor
                                Material.accent: dialog.accentColor
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                color: dialog.dividerColor
                            }

                            Switch {
                                Layout.fillWidth: true
                                text: qsTr("Natural language search")
                                checked: searchController ? searchController.naturalLanguageSearchEnabled : false
                                onToggled: {
                                    if (searchController) {
                                        searchController.SetNaturalLanguageSearchEnabled(checked)
                                        if (dialog.interactionPolicyController) {
                                            dialog.interactionPolicyController.naturalLanguageSearchEnabled = checked
                                        }
                                        // Re-run the typed query so the preview reflects the new route.
                                        previewTimer.restart()
                                    }
                                }
                                Material.foreground: dialog.withAlpha(dialog.textColor, 0.78)
                                Material.accent: dialog.accentColor
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Natural language search — use the CLIP model to search by meaning (Enter or Search button to run). Mutually exclusive with the field filters above.")
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: dialog.dividerColor
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.leftMargin: 26
                    Layout.rightMargin: 26
                    Layout.topMargin: 20
                    Layout.bottomMargin: 18
                    spacing: 16

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        spacing: 14

                        Label {
                            text: searchField.text.trim().length === 0 ? qsTr("Suggestion")
                                                                       : qsTr("Results")
                            color: dialog.withAlpha(dialog.textColor, 0.68)
                            font.pixelSize: 14
                            font.weight: 760
                        }

                        Label {
                            visible: searchField.text.trim().length > 0
                            text: dialog.resultCountText()
                            color: dialog.withAlpha(dialog.textColor, 0.42)
                            font.family: dialog.dataFontFamily
                            font.pixelSize: 12
                            font.weight: 600
                        }

                        BusyIndicator {
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 18
                            running: visible
                            visible: dialog.searchLoading && searchField.text.trim().length > 0
                        }

                        Item {
                            Layout.fillWidth: true
                        }

                        Label {
                            visible: dialog.searchController !== null && dialog.searchController !== undefined
                                     && dialog.searchController.activeSearchQuery.length > 0
                            text: qsTr("Active: %1").arg(dialog.searchController
                                                        ? dialog.searchController.activeSearchQuery : "")
                            color: dialog.withAlpha(dialog.textColor, 0.46)
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            Layout.maximumWidth: 320
                        }
                    }

                    ListView {
                        id: recommendationList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: searchField.text.trim().length === 0
                        clip: true
                        spacing: 0
                        model: dialog.recommendations

                        delegate: SearchRow {
                            required property var modelData
                            width: recommendationList.width
                            title: modelData.label ? String(modelData.label) : ""
                            subtitle: modelData.categoryLabel ? String(modelData.categoryLabel) : ""
                            countText: modelData.count ? String(modelData.count) : ""
                            iconSource: dialog.suggestionIconSource(modelData.category)
                            framedIcon: false
                            rowHeight: 60
                            titlePixelSize: 13
                            titleWeight: 620
                            subtitlePixelSize: 11
                            thumbnailWidth: 42
                            thumbnailHeight: 34
                            iconSize: 17
                            onActivated: dialog.applyRecommendation(modelData)
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: recommendationList.count === 0
                            text: qsTr("No recent suggestions")
                            color: dialog.withAlpha(dialog.textColor, 0.44)
                            font.pixelSize: 13
                        }
                    }

                    ListView {
                        id: resultList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: searchField.text.trim().length > 0
                        clip: true
                        spacing: 0
                        model: dialog.results
                        cacheBuffer: 0
                        reuseItems: false

                        ScrollIndicator.vertical: ScrollIndicator {}

                        onContentYChanged: {
                            dialog.scheduleVisiblePreviewSync(false)
                            if (dialog.searchHasMore
                                    && contentY + height >= contentHeight - 160) {
                                dialog.loadMorePreview()
                            }
                        }
                        onMovementEnded: {
                            dialog.scheduleVisiblePreviewSync(false)
                            if (dialog.searchHasPrevious
                                    && contentY <= originY + 120) {
                                dialog.loadPreviousPreview()
                            }
                            if (dialog.searchHasMore
                                    && contentY + height >= contentHeight - 160) {
                                dialog.loadMorePreview()
                            }
                        }
                        onCountChanged: dialog.scheduleVisiblePreviewSync(true)

                        header: Item {
                            width: resultList.width
                            height: dialog.searchHasPrevious ? 54 : 10

                            BusyIndicator {
                                anchors.centerIn: parent
                                width: 24
                                height: 24
                                running: visible
                                visible: dialog.searchHasPrevious && dialog.searchLoading
                            }

                            Button {
                                anchors.centerIn: parent
                                visible: dialog.searchHasPrevious && !dialog.searchLoading
                                enabled: !dialog.searchLoading
                                text: qsTr("Load previous")
                                onClicked: dialog.loadPreviousPreview()
                            }
                        }

                        delegate: SearchRow {
                            required property var modelData

                            width: resultList.width
                            elementId: Number(modelData.elementId)
                            imageId: Number(modelData.imageId)
                            dynamicPreviewThumbnail: true
                            previewMaxEdge: 256
                            title: modelData.fileName ? String(modelData.fileName) : qsTr("(unnamed)")
                            subtitle: qsTr("%1  |  %2").arg(modelData.cameraModel).arg(modelData.captureDate)
                            detailText: Number(modelData.rating) > 0
                                        ? qsTr("Rating %1/5").arg(Number(modelData.rating))
                                        : (modelData.lens ? String(modelData.lens) : "")
                            iconSource: "qrc:/panel_icons/image.svg"
                            initialThumbUrl: {
                                const state = dialog.previewThumbs[String(Number(modelData.elementId))] || ({})
                                if (state.url) {
                                    return String(state.url)
                                }
                                return modelData.thumbUrl ? String(modelData.thumbUrl) : ""
                            }
                            initialThumbLoading: {
                                const state = dialog.previewThumbs[String(Number(modelData.elementId))] || ({})
                                return state.loading === true || modelData.thumbLoading === true
                            }
                            initialThumbMissingSource: {
                                const state = dialog.previewThumbs[String(Number(modelData.elementId))] || ({})
                                return state.missingSource === true || modelData.thumbMissingSource === true
                            }
                            initialThumbErrorText: {
                                const state = dialog.previewThumbs[String(Number(modelData.elementId))] || ({})
                                if (state.errorText) {
                                    return String(state.errorText)
                                }
                                return modelData.thumbErrorText ? String(modelData.thumbErrorText) : ""
                            }
                            onActivated: dialog.applyExact(modelData)
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: !dialog.searchLoading && resultList.count === 0
                                     && searchField.text.trim().length > 0
                            text: naturalLanguageStatusText.length > 0
                                  ? naturalLanguageStatusText
                                  : qsTr("No matches")
                            color: dialog.withAlpha(dialog.textColor, 0.48)
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            width: parent.width - 120
                        }

                        BusyIndicator {
                            anchors.centerIn: parent
                            width: 34
                            height: 34
                            running: visible
                            visible: dialog.searchLoading && resultList.count === 0
                                     && searchField.text.trim().length > 0
                        }

                        footer: Item {
                            width: resultList.width
                            height: dialog.searchHasMore ? 54 : 10

                            BusyIndicator {
                                anchors.centerIn: parent
                                width: 24
                                height: 24
                                running: visible
                                visible: dialog.searchHasMore && dialog.searchLoading
                            }

                            Button {
                                anchors.centerIn: parent
                                visible: dialog.searchHasMore && !dialog.searchLoading
                                enabled: !dialog.searchLoading
                                text: qsTr("Load more")
                                onClicked: dialog.loadMorePreview()
                            }
                        }
                    }
                }

            }
        }
    }

    component SearchRow: Rectangle {
        id: row

        property string title: ""
        property string subtitle: ""
        property string detailText: ""
        property string countText: ""
        property string iconSource: ""
        property string iconText: ""
        property bool framedIcon: true
        property int rowHeight: 82
        property int titlePixelSize: 17
        property int titleWeight: 690
        property int subtitlePixelSize: 12
        property int thumbnailWidth: 64
        property int thumbnailHeight: 48
        property int iconSize: 21
        property int elementId: 0
        property int imageId: 0
        property bool dynamicPreviewThumbnail: false
        property int previewMaxEdge: 256
        property string initialThumbUrl: ""
        property bool initialThumbLoading: false
        property bool initialThumbMissingSource: false
        property string initialThumbErrorText: ""
        property string liveThumbUrl: initialThumbUrl
        property bool liveThumbLoading: initialThumbLoading
        property bool liveThumbMissingSource: initialThumbMissingSource
        property string liveThumbErrorText: initialThumbErrorText
        property int pinnedElementId: 0
        property int pinnedImageId: 0
        property int pinnedMaxEdge: 0
        readonly property bool thumbReady: liveThumbUrl.length > 0
        readonly property bool thumbProblem: !thumbReady && !liveThumbLoading
                                             && (liveThumbMissingSource || liveThumbErrorText.length > 0)
        readonly property string thumbProblemText: liveThumbErrorText.length > 0
                                                   ? liveThumbErrorText
                                                   : qsTr("Source file is unavailable")

        signal activated()

        onInitialThumbUrlChanged: liveThumbUrl = initialThumbUrl
        onInitialThumbLoadingChanged: liveThumbLoading = initialThumbLoading
        onInitialThumbMissingSourceChanged: liveThumbMissingSource = initialThumbMissingSource
        onInitialThumbErrorTextChanged: liveThumbErrorText = initialThumbErrorText

        function applyInitialPreviewState() {
            liveThumbUrl = initialThumbUrl
            liveThumbLoading = initialThumbLoading
            liveThumbMissingSource = initialThumbMissingSource
            liveThumbErrorText = initialThumbErrorText
        }

        function releasePreviewThumbnail() {
            if (dialog.searchController && pinnedElementId !== 0 && pinnedImageId !== 0) {
                dialog.searchController.SetSearchPreviewThumbnailVisible(pinnedElementId,
                                                                         pinnedImageId, false,
                                                                         pinnedMaxEdge)
            }
            pinnedElementId = 0
            pinnedImageId = 0
            pinnedMaxEdge = 0
        }

        function syncPreviewThumbnailLifetime(force) {
            bindPreviewThumbnailLifetime(force === true)
        }

        function bindPreviewThumbnailLifetime(force) {
            if (!dynamicPreviewThumbnail) {
                releasePreviewThumbnail()
                return
            }
            if (!force && pinnedElementId === elementId && pinnedImageId === imageId
                    && pinnedMaxEdge === previewMaxEdge) {
                return
            }

            if (pinnedElementId === elementId && pinnedImageId === imageId
                    && pinnedMaxEdge === previewMaxEdge) {
                applyInitialPreviewState()
                if (dialog.searchController && pinnedElementId !== 0 && pinnedImageId !== 0
                        && !thumbReady) {
                    dialog.searchController.SetSearchPreviewThumbnailVisible(pinnedElementId,
                                                                             pinnedImageId, true,
                                                                             pinnedMaxEdge)
                }
                return
            }

            releasePreviewThumbnail()
            pinnedElementId = elementId
            pinnedImageId = imageId
            pinnedMaxEdge = previewMaxEdge
            applyInitialPreviewState()
            if (dialog.searchController && pinnedElementId !== 0 && pinnedImageId !== 0
                    && !thumbReady) {
                dialog.searchController.SetSearchPreviewThumbnailVisible(pinnedElementId,
                                                                         pinnedImageId, true,
                                                                         pinnedMaxEdge)
            }
        }

        Component.onCompleted: bindPreviewThumbnailLifetime(false)
        onElementIdChanged: bindPreviewThumbnailLifetime(false)
        onImageIdChanged: bindPreviewThumbnailLifetime(false)
        onDynamicPreviewThumbnailChanged: bindPreviewThumbnailLifetime(false)
        onPreviewMaxEdgeChanged: bindPreviewThumbnailLifetime(false)
        Component.onDestruction: releasePreviewThumbnail()

        height: rowHeight
        radius: 9
        color: rowMouse.pressed
               ? dialog.withAlpha(dialog.textColor, 0.075)
               : (rowMouse.containsMouse ? dialog.hoverColor : "transparent")

        Connections {
            target: dialog.searchController
            ignoreUnknownSignals: true

            function onSearchPreviewThumbnailUpdated(updatedElementId, dataUrl, loading, missingSource, errorText) {
                if (Number(updatedElementId) !== row.elementId) {
                    return
                }
                row.liveThumbUrl = dataUrl ? String(dataUrl) : ""
                row.liveThumbLoading = loading === true
                row.liveThumbMissingSource = missingSource === true
                row.liveThumbErrorText = errorText ? String(errorText) : ""
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: dialog.withAlpha(dialog.textColor, 0.07)
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 2
            anchors.rightMargin: 12
            spacing: 14

            Item {
                Layout.preferredWidth: row.thumbnailWidth
                Layout.preferredHeight: row.thumbnailHeight

                Rectangle {
                    anchors.fill: parent
                    visible: row.framedIcon
                    radius: 8
                    color: row.thumbReady ? "transparent" : dialog.withAlpha(dialog.canvasColor, 0.72)
                    border.width: row.thumbReady ? 0 : 1
                    border.color: dialog.withAlpha(dialog.textColor, 0.08)
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    running: visible
                    visible: row.framedIcon && row.liveThumbLoading
                }

                Image {
                    id: thumbImage
                    anchors.fill: parent
                    source: row.liveThumbUrl
                    visible: false
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true

                    onStatusChanged: {
                        if (status === Image.Error && row.liveThumbUrl.length > 0
                                && row.liveThumbErrorText.length === 0) {
                            row.liveThumbErrorText = qsTr("Preview image failed to load")
                        }
                    }
                }

                Rectangle {
                    id: thumbMask
                    anchors.fill: thumbImage
                    radius: 7
                    visible: false
                    layer.enabled: true
                }

                MultiEffect {
                    anchors.fill: thumbImage
                    source: thumbImage
                    maskEnabled: true
                    maskSource: thumbMask
                    visible: row.framedIcon && row.thumbReady
                }

                Image {
                    anchors.centerIn: parent
                    width: row.iconSize
                    height: row.iconSize
                    source: row.iconSource
                    visible: !row.thumbReady && !row.liveThumbLoading && !row.thumbProblem
                             && row.iconSource.length > 0
                    sourceSize.width: row.iconSize
                    sourceSize.height: row.iconSize
                    opacity: row.framedIcon ? 0.58 : 0.72
                    fillMode: Image.PreserveAspectFit
                }

                Label {
                    anchors.centerIn: parent
                    visible: !row.thumbReady && !row.liveThumbLoading && !row.thumbProblem
                             && row.iconSource.length === 0 && row.iconText.length > 0
                    text: row.iconText
                    color: dialog.withAlpha(dialog.textColor, 0.62)
                    font.family: dialog.dataFontFamily
                    font.pixelSize: 11
                    font.weight: 800
                }

                Label {
                    anchors.centerIn: parent
                    visible: row.thumbProblem
                    text: "!"
                    color: dialog.accentColor
                    font.family: dialog.dataFontFamily
                    font.pixelSize: 22
                    font.weight: 800
                }

                HoverHandler {
                    id: thumbHover
                }
                ToolTip.visible: row.thumbProblem && thumbHover.hovered
                ToolTip.text: row.thumbProblemText
                ToolTip.delay: 160
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5

                Label {
                    Layout.fillWidth: true
                    text: row.title
                    color: dialog.withAlpha(dialog.textColor, 0.88)
                    font.family: dialog.dataFontFamily
                    font.pixelSize: row.titlePixelSize
                    font.weight: row.titleWeight
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: row.detailText.length > 0
                          ? qsTr("%1  -  %2").arg(row.subtitle).arg(row.detailText)
                          : row.subtitle
                    visible: text.length > 0
                    color: dialog.withAlpha(dialog.textColor, 0.50)
                    font.family: dialog.dataFontFamily
                    font.pixelSize: row.subtitlePixelSize
                    font.weight: 560
                    elide: Text.ElideRight
                }
            }

            Label {
                visible: row.countText.length > 0
                text: row.countText
                color: dialog.withAlpha(dialog.textColor, 0.42)
                font.family: dialog.dataFontFamily
                font.pixelSize: 12
                font.weight: 700
            }
        }

        MouseArea {
            id: rowMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: row.activated()
        }
    }
}
