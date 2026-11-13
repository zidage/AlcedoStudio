import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Lens catalog picker for the RAW Decode lens-calibration section. Two-tier
// inline menu per the LUT-panel pattern: a collapsed sunken field expands into
// a brand filter sub-menu (itself collapsible with its own search box) plus a
// searchable lens list. With no brand selected the list shows every catalog
// entry (interchangeable lenses and fixed-lens camera profiles); a selected
// brand narrows it to that maker.
//
// Fuzzy matching mirrors lensfun's loose search: normalize both sides by
// lowercasing and dropping everything except [a-z0-9], then require every
// query token to appear inside the normalized "brand + label" haystack.
// "70-200" therefore matches "… 70-200mm f/2.8 …" entries, and brand tokens
// inside a query ("canon 70-200") still resolve.
Item {
    id: root
    objectName: "lensCatalogPicker"

    // ── Data in ──────────────────────────────────────────────────────────
    // Any object exposing `brands` ([{value,label}|string]) and
    // `modelsForBrand(brand)` ([{value,label}|string]); production passes the
    // registered EditorLensCatalogModel, tests may pass a fixture stub.
    property var catalog: null
    /// Image EXIF lens identity (EditorSessionController exifLensMake/Model).
    property string exifLensMake: ""
    property string exifLensModel: ""
    /// Submitted selection (panel lens maker/model params). "" brand = Auto.
    property string selectedValue: ""
    property string selectedBrand: ""
    /// When true the list highlights the auto-detected entry instead of the
    /// submitted maker/model pair.
    property bool autoEngaged: false
    property bool controlsEnabled: true
    property bool expanded: false

    // ── Filter state (UI-only; never submitted) ────────────────────────────
    property string brandFilter: ""
    property string brandQuery: ""
    property string lensQuery: ""
    property bool brandMenuExpanded: false
    property int highlightedIndex: -1

    // ── Derived state ──────────────────────────────────────────────────────
    property var _allEntries: []
    property var filteredBrands: []
    property var filteredEntries: []
    /// Detected catalog row for the EXIF identity, or null when recognition
    /// fails (missing EXIF lens or no catalog match).
    property var detectedEntry: null
    readonly property bool detectionAvailable: detectedEntry !== null
    readonly property int entryCount: filteredEntries.length
    readonly property string displayLensName: {
        if (root.autoEngaged && root.detectedEntry)
            return String(root.detectedEntry.label)
        return root.selectedValue
    }
    readonly property var _selectionEntry: {
        if (root.autoEngaged && root.detectedEntry)
            return root.detectedEntry
        if (root.selectedValue.length > 0)
            return { brand: root.selectedBrand, value: root.selectedValue }
        return null
    }

    // ── Fold driver (same convention as CollapsibleSection) ────────────────
    property real foldProgress: expanded ? 1 : 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs

    signal lensPicked(string brand, string value)

    implicitWidth: 200
    implicitHeight: cardShell.implicitHeight
    Layout.fillWidth: true
    activeFocusOnTab: controlsEnabled
    Accessible.role: Accessible.ComboBox
    Accessible.name: qsTr("Lens")

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space
                || (!root.expanded && event.key === Qt.Key_Down)) {
            root.toggleExpanded()
            event.accepted = true
        }
    }

    function driveFoldProgress(value) {
        foldManualDrive = true
        foldProgress = Math.max(0, Math.min(1, value))
    }

    function endFoldDrive() {
        foldManualDrive = false
        foldProgress = expanded ? 1 : 0
    }

    function toggleExpanded() {
        if (!root.controlsEnabled)
            return
        root.expanded = !root.expanded
        if (root.expanded)
            Qt.callLater(function () { lensSearchInput.forceActiveFocus() })
    }

    // ── Matching helpers ───────────────────────────────────────────────────
    function normalizeLensText(text) {
        return String(text || "").toLocaleLowerCase().replace(/[^a-z0-9]+/g, "")
    }

    function lensQueryTokens(query) {
        const raw = String(query || "").toLocaleLowerCase().split(/[^a-z0-9]+/)
        var tokens = []
        for (var i = 0; i < raw.length; ++i) {
            if (raw[i].length > 0)
                tokens.push(raw[i])
        }
        return tokens
    }

    function _entryHaystack(entry) {
        return normalizeLensText(String(entry.brand) + " " + String(entry.label))
    }

    function entryMatchesQuery(entry, query) {
        const trimmed = String(query || "").trim()
        if (trimmed.length === 0)
            return true
        const haystack = entry._haystack
        const joined = normalizeLensText(trimmed)
        if (joined.length === 0)
            return true
        if (haystack.indexOf(joined) >= 0)
            return true
        const tokens = lensQueryTokens(trimmed)
        for (var i = 0; i < tokens.length; ++i) {
            if (haystack.indexOf(tokens[i]) < 0)
                return false
        }
        return tokens.length > 0
    }

    function _matchRank(entry, query) {
        const joined = normalizeLensText(query)
        if (joined.length === 0)
            return 0
        if (entry._normValue === joined)
            return 0
        if (entry._normValue.indexOf(joined) === 0)
            return 1
        if (entry._haystack.indexOf(joined) >= 0)
            return 2
        return 3
    }

    /// Map an EXIF lens identity onto a catalog row. Exact normalized equality
    /// wins, then containment in either direction, then full token coverage;
    /// a normalized maker match on the entry brand always breaks ties.
    function findDetectedEntry(make, model) {
        const normModel = normalizeLensText(model)
        if (normModel.length === 0)
            return null
        const normMake = normalizeLensText(make)
        var best = null
        var bestScore = -1
        for (var i = 0; i < _allEntries.length; ++i) {
            const entry = _allEntries[i]
            var score = -1
            if (entry._normValue === normModel) {
                score = 100
            } else if (normModel.length >= 4 && entry._normValue.indexOf(normModel) >= 0) {
                score = 60 + 30 * (normModel.length / Math.max(1, entry._normValue.length))
            } else if (entry._normValue.length >= 4 && normModel.indexOf(entry._normValue) >= 0) {
                score = 40 + 30 * (entry._normValue.length / Math.max(1, normModel.length))
            } else {
                const tokens = lensQueryTokens(model)
                var allPresent = tokens.length > 0
                for (var t = 0; t < tokens.length; ++t) {
                    if (entry._haystack.indexOf(tokens[t]) < 0) {
                        allPresent = false
                        break
                    }
                }
                if (allPresent)
                    score = 20
            }
            if (score < 0)
                continue
            if (normMake.length > 0 && entry._normBrand.indexOf(normMake) >= 0)
                score += 1000
            if (score > bestScore) {
                bestScore = score
                best = entry
            }
        }
        return best
    }

    // ── List building / filtering ──────────────────────────────────────────
    function _entryValue(item) {
        if (item === undefined || item === null)
            return ""
        return item.value !== undefined ? String(item.value) : String(item)
    }

    function rebuildCatalog() {
        var list = []
        const brands = root.catalog && root.catalog.brands ? root.catalog.brands : []
        for (var i = 0; i < brands.length; ++i) {
            const brand = _entryValue(brands[i])
            const models = root.catalog.modelsForBrand(brand) || []
            for (var j = 0; j < models.length; ++j) {
                const value = _entryValue(models[j])
                const label = models[j] && models[j].label !== undefined
                              ? String(models[j].label) : value
                if (value.length === 0)
                    continue
                list.push({ brand: brand, value: value, label: label })
            }
        }
        for (var k = 0; k < list.length; ++k) {
            list[k]._normValue = normalizeLensText(list[k].value)
            list[k]._normBrand = normalizeLensText(list[k].brand)
            list[k]._haystack = _entryHaystack(list[k])
        }
        _allEntries = list
        refilter()
        detectedEntry = findDetectedEntry(root.exifLensMake, root.exifLensModel)
    }

    function refilter() {
        // Brands for the filter sub-menu.
        var brandRows = []
        const brands = root.catalog && root.catalog.brands ? root.catalog.brands : []
        for (var i = 0; i < brands.length; ++i) {
            const value = _entryValue(brands[i])
            if (value.length === 0)
                continue
            if (entryMatchesQuery({ _haystack: normalizeLensText(value) }, root.brandQuery))
                brandRows.push({ value: value, label: value })
        }
        filteredBrands = brandRows

        // Lenses for the main list: brand filter first, then lens query.
        var rows = []
        for (var e = 0; e < _allEntries.length; ++e) {
            const entry = _allEntries[e]
            if (root.brandFilter.length > 0 && entry.brand !== root.brandFilter)
                continue
            if (!entryMatchesQuery(entry, root.lensQuery))
                continue
            rows.push(entry)
        }
        const query = root.lensQuery
        rows.sort(function (a, b) {
            const ra = _matchRank(a, query)
            const rb = _matchRank(b, query)
            if (ra !== rb)
                return ra - rb
            return a.label < b.label ? -1 : (a.label > b.label ? 1 : 0)
        })
        filteredEntries = rows
        if (highlightedIndex >= rows.length)
            highlightedIndex = rows.length - 1
    }

    function indexOfEntry(brand, value) {
        for (var i = 0; i < filteredEntries.length; ++i) {
            if (filteredEntries[i].brand === brand && filteredEntries[i].value === value)
                return i
        }
        return -1
    }

    /// Auto-recognition re-check path: clear every filter and scroll the lens
    /// list back to the detected catalog entry.
    function revealDetected() {
        brandFilter = ""
        brandQuery = ""
        lensQuery = ""
        brandMenuExpanded = false
        refilter()
        if (!detectedEntry)
            return
        expanded = true
        const idx = indexOfEntry(detectedEntry.brand, detectedEntry.value)
        if (idx >= 0) {
            highlightedIndex = idx
            Qt.callLater(function () {
                lensView.positionViewAtIndex(idx, ListView.Contain)
            })
        }
    }

    function pickHighlighted() {
        if (highlightedIndex < 0 || highlightedIndex >= filteredEntries.length)
            return false
        const entry = filteredEntries[highlightedIndex]
        root.lensPicked(entry.brand, entry.value)
        return true
    }

    onCatalogChanged: rebuildCatalog()
    onExifLensMakeChanged: detectedEntry = findDetectedEntry(root.exifLensMake, root.exifLensModel)
    onExifLensModelChanged: detectedEntry = findDetectedEntry(root.exifLensMake, root.exifLensModel)
    onBrandFilterChanged: refilter()
    onBrandQueryChanged: refilter()
    onLensQueryChanged: refilter()

    onExpandedChanged: {
        _foldDuration = expanded ? appTheme.motionFoldOpenMs : appTheme.motionFoldCloseMs
        if (!foldManualDrive)
            foldProgress = expanded ? 1 : 0
    }

    Component.onCompleted: {
        rebuildCatalog()
        foldProgress = expanded ? 1 : 0
        _motionArmed = true
    }

    Behavior on foldProgress {
        enabled: root._motionArmed && !root.foldManualDrive
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : root._foldDuration
            easing.type: appTheme.motionEasing
        }
    }

    // ── Unified card: the collapsed bar IS the menu's header ───────────────
    // One bordered card behind both states; expanding grows the same shell
    // rather than opening a separate panel under the field.
    Rectangle {
        id: cardShell
        objectName: "lensPickerCard"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        implicitHeight: cardColumn.implicitHeight + appTheme.spaceXs * 2
        radius: appTheme.controlRadiusSmall
        color: appTheme.bgBaseColor
        border.width: 1
        border.color: fieldMouse.containsMouse || root.activeFocus
                      ? appTheme.textMutedColor
                      : appTheme.cardBorderColor
    }

    ColumnLayout {
        id: cardColumn
        anchors.left: cardShell.left
        anchors.right: cardShell.right
        anchors.top: cardShell.top
        anchors.margins: appTheme.spaceXs
        spacing: appTheme.spaceXs

        // ── Header row: current lens + Auto hint + chevron ─────────────────
        Item {
            id: fieldChrome
            objectName: "lensPickerField"
            Layout.fillWidth: true
            implicitHeight: fieldRow.implicitHeight

            RowLayout {
                id: fieldRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                spacing: appTheme.spaceSm

                Label {
                    objectName: "lensPickerFieldLabel"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    text: root.displayLensName.length > 0
                          ? root.displayLensName
                          : qsTr("Select lens")
                    color: root.displayLensName.length > 0
                           ? appTheme.textColor
                           : appTheme.textMutedColor
                    font.pixelSize: appTheme.fontSizeBody
                    wrapMode: Text.Wrap
                }

                Label {
                    objectName: "lensPickerAutoHint"
                    Layout.alignment: Qt.AlignVCenter
                    visible: root.autoEngaged && root.detectionAvailable
                    text: qsTr("Auto")
                    color: appTheme.textMutedColor
                    font.pixelSize: appTheme.fontSizeCaption
                }

                Canvas {
                    id: fieldChevron
                    Layout.preferredWidth: 12
                    Layout.preferredHeight: 12
                    Layout.alignment: Qt.AlignVCenter
                    antialiasing: true
                    // Bound property: re-evaluates on ThemeChanged and repaints.
                    property color chevronInk: appTheme.textMutedColor
                    onChevronInkChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        ctx.strokeStyle = chevronInk
                        ctx.lineWidth = 1.5
                        ctx.lineCap = "round"
                        ctx.lineJoin = "round"
                        ctx.beginPath()
                        if (root.expanded) {
                            ctx.moveTo(2, 8)
                            ctx.lineTo(6, 4)
                            ctx.lineTo(10, 8)
                        } else {
                            ctx.moveTo(2, 4)
                            ctx.lineTo(6, 8)
                            ctx.lineTo(10, 4)
                        }
                        ctx.stroke()
                    }
                    Connections {
                        target: root
                        function onExpandedChanged() { fieldChevron.requestPaint() }
                    }
                }
            }

            MouseArea {
                id: fieldMouse
                anchors.fill: parent
                enabled: root.controlsEnabled
                hoverEnabled: true
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: root.toggleExpanded()
            }
        }

        // ── Menu body (inside the same card) ───────────────────────────────
        Item {
            id: menuShell
            objectName: "lensPickerMenu"
            Layout.fillWidth: true
            implicitHeight: menuColumn.implicitHeight
            Layout.preferredHeight: implicitHeight * root.foldProgress
            visible: root.foldProgress > 0.001
            opacity: root.foldProgress
            clip: true

            ColumnLayout {
                id: menuColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                spacing: appTheme.spaceXs

                // ── Brand filter sub-menu ──────────────────────────────────
                Item {
                    id: brandFilterBox
                    objectName: "lensBrandFilterBox"
                    Layout.fillWidth: true
                    implicitHeight: brandHeader.implicitHeight
                                    + (brandSubMenu.visible
                                       ? brandSubMenu.implicitHeight + appTheme.spaceXs
                                       : 0)

                    ColumnLayout {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        spacing: appTheme.spaceXs

                        Rectangle {
                            id: brandHeader
                            objectName: "lensBrandFilterRow"
                            Layout.fillWidth: true
                            implicitHeight: brandHeaderRow.implicitHeight + appTheme.spaceXs * 2
                            radius: appTheme.badgeRadius
                            color: brandHeaderMouse.containsMouse
                                   ? appTheme.buttonHoveredFillColor
                                   : appTheme.cardSurfaceColor
                            border.width: 1
                            border.color: appTheme.cardBorderColor

                            RowLayout {
                                id: brandHeaderRow
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: appTheme.spaceSm
                                anchors.rightMargin: appTheme.spaceSm
                                spacing: appTheme.spaceSm

                                Label {
                                    text: qsTr("Brand")
                                    color: appTheme.textMutedColor
                                    font.pixelSize: appTheme.fontSizeCaption
                                }

                                Label {
                                    objectName: "lensBrandFilterValue"
                                    Layout.fillWidth: true
                                    text: root.brandFilter.length > 0
                                          ? root.brandFilter
                                          : qsTr("All brands")
                                    color: appTheme.textColor
                                    font.pixelSize: appTheme.fontSizeBody
                                    wrapMode: Text.Wrap
                                }

                                Label {
                                    text: root.brandMenuExpanded ? "▴" : "▾"
                                    color: appTheme.textMutedColor
                                    font.pixelSize: appTheme.fontSizeCaption
                                }
                            }

                            MouseArea {
                                id: brandHeaderMouse
                                anchors.fill: parent
                                enabled: root.controlsEnabled
                                hoverEnabled: true
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: root.brandMenuExpanded = !root.brandMenuExpanded
                            }
                        }

                        ColumnLayout {
                            id: brandSubMenu
                            objectName: "lensBrandSubMenu"
                            Layout.fillWidth: true
                            visible: root.brandMenuExpanded
                            spacing: appTheme.spaceXs

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: appTheme.iconOpticalSizeCompact + appTheme.spaceSm
                                radius: appTheme.badgeRadius
                                color: appTheme.bgBaseColor
                                border.width: 1
                                border.color: brandQueryInput.activeFocus
                                              ? appTheme.textMutedColor
                                              : appTheme.cardBorderColor

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: appTheme.spaceSm
                                    anchors.rightMargin: appTheme.spaceSm
                                    spacing: appTheme.spaceXs

                                    ColorImage {
                                        Layout.preferredWidth: appTheme.iconOpticalSizeCompact * 0.7
                                        Layout.preferredHeight: appTheme.iconOpticalSizeCompact * 0.7
                                        Layout.alignment: Qt.AlignVCenter
                                        source: "qrc:/panel_icons/search.svg"
                                        sourceSize.width: appTheme.iconSourceSizeCompact
                                        sourceSize.height: appTheme.iconSourceSizeCompact
                                        color: appTheme.textMutedColor
                                    }

                                    TextInput {
                                        id: brandQueryInput
                                        objectName: "lensBrandQueryInput"
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignVCenter
                                        text: root.brandQuery
                                        enabled: root.controlsEnabled
                                        color: appTheme.textColor
                                        font.pixelSize: appTheme.fontSizeBody
                                        selectByMouse: true
                                        clip: true
                                        verticalAlignment: TextInput.AlignVCenter
                                        onTextEdited: root.brandQuery = text
                                        Keys.onEscapePressed: root.brandMenuExpanded = false

                                        Label {
                                            anchors.fill: parent
                                            visible: !parent.text.length && !parent.activeFocus
                                            text: qsTr("Search brands")
                                            color: appTheme.textMutedColor
                                            font.pixelSize: appTheme.fontSizeBody
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: Math.min(brandList.contentHeight,
                                                         appTheme.lineHeightBody * 4
                                                         + appTheme.spaceXs * 3)
                                                + appTheme.spaceXs * 2
                                radius: appTheme.badgeRadius
                                color: appTheme.cardSurfaceColor
                                border.width: 1
                                border.color: appTheme.cardBorderColor
                                clip: true

                                // The list owns the wheel over its area: scroll
                                // it here and always accept so an at-bounds
                                // wheel never reaches the panel Flickable.
                                WheelHandler {
                                    acceptedDevices: PointerDevice.Mouse
                                                     | PointerDevice.TouchPad
                                    onWheel: function (event) {
                                        const step = event.pixelDelta.y !== 0
                                                     ? event.pixelDelta.y
                                                     : event.angleDelta.y / 120 * 48
                                        const maxY = Math.max(0, brandList.contentHeight
                                                              - brandList.height)
                                        brandList.contentY = Math.max(
                                            0, Math.min(maxY, brandList.contentY - step))
                                        event.accepted = true
                                    }
                                }

                                ListView {
                                    id: brandList
                                    objectName: "lensBrandListView"
                                    anchors.fill: parent
                                    anchors.margins: appTheme.spaceXs
                                    spacing: appTheme.spaceXs
                                    clip: true
                                    boundsBehavior: Flickable.StopAtBounds
                                    flickableDirection: Flickable.VerticalFlick
                                    model: [{ value: "", label: qsTr("All brands") }]
                                           .concat(root.filteredBrands)
                                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                                    delegate: Item {
                                        id: brandRow
                                        required property var modelData
                                        required property int index
                                        objectName: "lensBrandEntry"
                                        width: brandList.width
                                        height: brandRowLabel.implicitHeight + appTheme.spaceXs
                                        readonly property string rowValue: String(
                                            modelData && modelData.value !== undefined
                                            ? modelData.value : "")
                                        readonly property bool rowSelected: rowValue === root.brandFilter

                                        Rectangle {
                                            anchors.fill: parent
                                            radius: appTheme.badgeRadius
                                            color: brandRow.rowSelected
                                                   ? appTheme.editorListSelectedFillColor
                                                   : (brandRowMouse.containsMouse
                                                      ? appTheme.buttonHoveredFillColor
                                                      : "transparent")
                                        }

                                        Label {
                                            id: brandRowLabel
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: appTheme.spaceSm
                                            anchors.rightMargin: appTheme.spaceSm
                                            text: brandRow.rowValue.length > 0
                                                  ? brandRow.rowValue
                                                  : String(modelData.label)
                                            color: brandRow.rowSelected
                                                   ? appTheme.editorListSelectedInkColor
                                                   : appTheme.textColor
                                            font.pixelSize: appTheme.fontSizeBody
                                            wrapMode: Text.Wrap
                                        }

                                        MouseArea {
                                            id: brandRowMouse
                                            anchors.fill: parent
                                            enabled: root.controlsEnabled
                                            hoverEnabled: true
                                            cursorShape: enabled ? Qt.PointingHandCursor
                                                                 : Qt.ArrowCursor
                                            onClicked: {
                                                root.brandFilter = brandRow.rowValue
                                                root.brandQuery = ""
                                                root.brandMenuExpanded = false
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ── Lens search field ──────────────────────────────────────
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: appTheme.iconOpticalSizeCompact + appTheme.spaceSm
                    radius: appTheme.badgeRadius
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: lensSearchInput.activeFocus
                                  ? appTheme.textMutedColor
                                  : appTheme.cardBorderColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: appTheme.spaceSm
                        anchors.rightMargin: appTheme.spaceSm
                        spacing: appTheme.spaceXs

                        ColorImage {
                            Layout.preferredWidth: appTheme.iconOpticalSizeCompact * 0.7
                            Layout.preferredHeight: appTheme.iconOpticalSizeCompact * 0.7
                            Layout.alignment: Qt.AlignVCenter
                            source: "qrc:/panel_icons/search.svg"
                            sourceSize.width: appTheme.iconSourceSizeCompact
                            sourceSize.height: appTheme.iconSourceSizeCompact
                            color: appTheme.textMutedColor
                        }

                        TextInput {
                            id: lensSearchInput
                            objectName: "lensQueryInput"
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            text: root.lensQuery
                            enabled: root.controlsEnabled
                            color: appTheme.textColor
                            font.pixelSize: appTheme.fontSizeBody
                            selectByMouse: true
                            clip: true
                            verticalAlignment: TextInput.AlignVCenter
                            onTextEdited: {
                                root.lensQuery = text
                                root.highlightedIndex = root.filteredEntries.length > 0 ? 0 : -1
                            }
                            Keys.onDownPressed: {
                                if (root.filteredEntries.length > 0)
                                    root.highlightedIndex = Math.min(
                                        root.highlightedIndex + 1,
                                        root.filteredEntries.length - 1)
                            }
                            Keys.onUpPressed: {
                                if (root.filteredEntries.length > 0)
                                    root.highlightedIndex = Math.max(root.highlightedIndex - 1, 0)
                            }
                            Keys.onReturnPressed: root.pickHighlighted()
                            Keys.onEnterPressed: root.pickHighlighted()
                            Keys.onEscapePressed: {
                                if (root.lensQuery.length > 0) {
                                    root.lensQuery = ""
                                } else {
                                    root.expanded = false
                                }
                            }

                            Label {
                                anchors.fill: parent
                                visible: !parent.text.length && !parent.activeFocus
                                text: qsTr("Search lenses")
                                color: appTheme.textMutedColor
                                font.pixelSize: appTheme.fontSizeBody
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        Label {
                            objectName: "lensQueryClear"
                            visible: root.lensQuery.length > 0
                            text: "×"
                            color: appTheme.textMutedColor
                            font.pixelSize: appTheme.fontSizeBody

                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -appTheme.spaceXs
                                enabled: root.controlsEnabled
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: {
                                    root.lensQuery = ""
                                    lensSearchInput.forceActiveFocus()
                                }
                            }
                        }
                    }
                }

                // ── Lens list ──────────────────────────────────────────────
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Math.min(lensView.contentHeight,
                                             appTheme.lineHeightBody * 7
                                             + appTheme.spaceXs * 6)
                                    + appTheme.spaceXs * 2
                    radius: appTheme.badgeRadius
                    color: appTheme.cardSurfaceColor
                    border.width: 1
                    border.color: appTheme.cardBorderColor
                    clip: true

                    // Same wheel lock as the brand list: the wheel scrolls the
                    // list and is always accepted, so the outer panel never
                    // follows along when the list hits its bounds.
                    WheelHandler {
                        acceptedDevices: PointerDevice.Mouse
                                         | PointerDevice.TouchPad
                        onWheel: function (event) {
                            const step = event.pixelDelta.y !== 0
                                         ? event.pixelDelta.y
                                         : event.angleDelta.y / 120 * 48
                            const maxY = Math.max(0, lensView.contentHeight
                                                  - lensView.height)
                            lensView.contentY = Math.max(
                                0, Math.min(maxY, lensView.contentY - step))
                            event.accepted = true
                        }
                    }

                    ListView {
                        id: lensView
                        objectName: "lensListView"
                        anchors.fill: parent
                        anchors.margins: appTheme.spaceXs
                        spacing: appTheme.spaceXs
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        model: root.filteredEntries
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        delegate: Item {
                            id: lensRow
                            required property var modelData
                            required property int index
                            objectName: "lensEntry"
                            width: lensView.width
                            height: lensRowColumn.implicitHeight + appTheme.spaceXs
                            readonly property string rowValue: String(modelData.value || "")
                            readonly property string rowBrand: String(modelData.brand || "")
                            readonly property bool rowSelected: {
                                const sel = root._selectionEntry
                                return sel !== null && sel !== undefined
                                       && sel.value === rowValue && sel.brand === rowBrand
                            }
                            readonly property bool rowHighlighted: index === root.highlightedIndex

                            Rectangle {
                                anchors.fill: parent
                                radius: appTheme.badgeRadius
                                color: lensRow.rowSelected
                                       ? appTheme.editorListSelectedFillColor
                                       : (lensRowMouse.containsMouse || lensRow.rowHighlighted
                                          ? appTheme.buttonHoveredFillColor
                                          : "transparent")
                            }

                            ColumnLayout {
                                id: lensRowColumn
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: appTheme.spaceSm
                                anchors.rightMargin: appTheme.spaceSm
                                spacing: 0

                                Label {
                                    Layout.fillWidth: true
                                    text: lensRow.rowValue
                                    color: lensRow.rowSelected
                                           ? appTheme.editorListSelectedInkColor
                                           : appTheme.textColor
                                    font.pixelSize: appTheme.fontSizeBody
                                    wrapMode: Text.Wrap
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: root.brandFilter.length === 0
                                    text: lensRow.rowBrand
                                    color: lensRow.rowSelected
                                           ? appTheme.editorListSelectedInkColor
                                           : appTheme.textMutedColor
                                    font.pixelSize: appTheme.fontSizeCaption
                                    wrapMode: Text.Wrap
                                }
                            }

                            MouseArea {
                                id: lensRowMouse
                                anchors.fill: parent
                                enabled: root.controlsEnabled
                                hoverEnabled: true
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onEntered: root.highlightedIndex = index
                                onClicked: root.lensPicked(lensRow.rowBrand, lensRow.rowValue)
                            }
                        }
                    }

                    Label {
                        objectName: "lensEmptyHint"
                        anchors.centerIn: parent
                        visible: lensView.count === 0
                        text: qsTr("No lenses match")
                        color: appTheme.textMutedColor
                        font.pixelSize: appTheme.fontSizeCaption
                    }
                }
            }
        }
    }
}
