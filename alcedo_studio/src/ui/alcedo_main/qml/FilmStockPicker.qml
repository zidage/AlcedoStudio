import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Film or print material picker of the film print page (issue #657). The
// collapsed card shows the swatch, the brand, and the short name of the
// current entry. A click opens the browser inside the same card: the entries
// grouped by brand, then by family. A pick keeps the browser open; a click on
// the card or Escape closes it. When the list has more than
// `searchThreshold` entries, a search field and brand filter chips show.
//
// The caller gives the entries of one browser list in browser order
// (EditorParameterCatalog.filmBrowserEntries) and applies a pick through
// `picked(value)`. The picker does not write a parameter.
Item {
    id: root
    objectName: "filmStockPicker"

    // ── Data in ──────────────────────────────────────────────────────────
    /// Browser entries {value, label, brand, family, shortLabel, shape, colors}.
    property var entries: []
    /// Brands {name, color} in browser order.
    property var brands: []
    property string currentValue: ""
    property bool controlsEnabled: true
    property bool expanded: false
    /// Show the family of the current entry on the card (print materials).
    property bool showFamilyOnCard: false
    property string accessibleName: ""
    property string searchPlaceholder: ""
    property string emptyText: ""
    /// Translates an English family caption (the caller owns the translation context).
    property var familyText: function (family) { return family }
    readonly property int searchThreshold: 8
    readonly property bool searchable: root.entries.length > root.searchThreshold

    // ── Browser state (UI only, never written) ────────────────────────────
    property string query: ""
    property string brandFilter: ""
    property int highlightedRow: -1
    property var rows: []

    readonly property var currentEntry: {
        for (var i = 0; i < root.entries.length; ++i) {
            if (String(root.entries[i].value) === root.currentValue)
                return root.entries[i]
        }
        return null
    }

    // ── Fold driver (same convention as CollapsibleSection) ────────────────
    property real foldProgress: expanded ? 1 : 0
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs

    signal picked(string value)

    implicitWidth: 200
    implicitHeight: cardShell.implicitHeight
    Layout.fillWidth: true
    activeFocusOnTab: controlsEnabled
    Accessible.role: Accessible.ComboBox
    Accessible.name: root.accessibleName

    // The card has focus when the list has no search field: it moves the highlight.
    Keys.onPressed: function (event) {
        if (!root.expanded) {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                    || event.key === Qt.Key_Space || event.key === Qt.Key_Down) {
                root.toggleExpanded()
                event.accepted = true
            }
            return
        }
        event.accepted = root.handleBrowserKey(event.key)
    }

    function handleBrowserKey(key) {
        if (key === Qt.Key_Down) {
            root.moveHighlight(1)
            return true
        }
        if (key === Qt.Key_Up) {
            root.moveHighlight(-1)
            return true
        }
        if (key === Qt.Key_Return || key === Qt.Key_Enter) {
            root.pickHighlighted()
            return true
        }
        if (key === Qt.Key_Escape) {
            if (root.query.length > 0) {
                root.query = ""
            } else {
                root.expanded = false
                root.forceActiveFocus()
            }
            return true
        }
        return false
    }

    function toggleExpanded() {
        if (!root.controlsEnabled)
            return
        root.expanded = !root.expanded
    }

    function familyLabel(family) {
        return family && family.length > 0 ? String(root.familyText(String(family))) : ""
    }

    function normalized(text) {
        return String(text || "").toLocaleLowerCase().replace(/[^a-z0-9]+/g, "")
    }

    function entryMatches(entry) {
        if (root.brandFilter.length > 0 && String(entry.brand) !== root.brandFilter)
            return false
        const tokens = String(root.query || "").toLocaleLowerCase().split(/[^a-z0-9]+/)
        const haystack = root.normalized(String(entry.brand) + " " + String(entry.label) + " "
                                         + String(entry.shortLabel) + " "
                                         + root.familyLabel(entry.family))
        for (var i = 0; i < tokens.length; ++i) {
            if (tokens[i].length > 0 && haystack.indexOf(tokens[i]) < 0)
                return false
        }
        return true
    }

    function brandColor(name) {
        for (var i = 0; i < root.brands.length; ++i) {
            if (String(root.brands[i].name) === name)
                return String(root.brands[i].color)
        }
        return appTheme.textMutedColor
    }

    function brandCount(name) {
        var count = 0
        for (var i = 0; i < root.entries.length; ++i) {
            if (String(root.entries[i].brand) === name)
                ++count
        }
        return count
    }

    /// Filter chips: All, then each brand that has an entry in this list.
    readonly property var chips: {
        var result = [{ name: "", label: qsTr("All"), color: "", count: root.entries.length }]
        for (var i = 0; i < root.brands.length; ++i) {
            const name = String(root.brands[i].name)
            const count = root.brandCount(name)
            if (count > 0)
                result.push({ name: name, label: name, color: String(root.brands[i].color),
                              count: count })
        }
        return result
    }

    /// Flat browser rows: brand headings, family captions, and entries.
    function rebuildRows() {
        var matching = []
        for (var i = 0; i < root.entries.length; ++i) {
            if (root.entryMatches(root.entries[i]))
                matching.push(root.entries[i])
        }
        var result = []
        var brand = null
        var family = null
        for (var j = 0; j < matching.length; ++j) {
            const entry = matching[j]
            const entryBrand = String(entry.brand)
            const entryFamily = String(entry.family)
            if (entryBrand !== brand) {
                brand = entryBrand
                family = null
                if (entryBrand.length > 0) {
                    var count = 0
                    for (var k = j; k < matching.length; ++k) {
                        if (String(matching[k].brand) === entryBrand)
                            ++count
                    }
                    result.push({ kind: "brand", text: entryBrand.toLocaleUpperCase(),
                                  color: root.brandColor(entryBrand), count: count })
                }
            }
            if (entryFamily !== family) {
                family = entryFamily
                if (entryFamily.length > 0)
                    result.push({ kind: "family", text: root.familyLabel(entryFamily) })
            }
            result.push({ kind: "entry", entry: entry })
        }
        root.rows = result
        root.highlightedRow = root.rowOfValue(root.currentValue)
        if (root.highlightedRow < 0)
            root.highlightedRow = root.nextEntryRow(-1, 1)
    }

    function rowOfValue(value) {
        for (var i = 0; i < root.rows.length; ++i) {
            if (root.rows[i].kind === "entry" && String(root.rows[i].entry.value) === value)
                return i
        }
        return -1
    }

    function nextEntryRow(from, step) {
        for (var i = from + step; i >= 0 && i < root.rows.length; i += step) {
            if (root.rows[i].kind === "entry")
                return i
        }
        return -1
    }

    function moveHighlight(step) {
        const next = root.nextEntryRow(root.highlightedRow, step)
        if (next >= 0) {
            root.highlightedRow = next
            entryView.positionViewAtIndex(next, ListView.Contain)
        }
    }

    function pickHighlighted() {
        if (root.highlightedRow < 0 || root.highlightedRow >= root.rows.length)
            return
        const row = root.rows[root.highlightedRow]
        if (row.kind === "entry")
            root.pickValue(String(row.entry.value))
    }

    /// A pick applies the entry and keeps the browser open, with the entry highlighted; the caller
    /// makes it current. The browser closes on a click on the card or on Escape (issue 661).
    function pickValue(value) {
        const row = root.rowOfValue(value)
        if (row >= 0)
            root.highlightedRow = row
        root.picked(value)
    }

    onEntriesChanged: rebuildRows()
    onQueryChanged: rebuildRows()
    onBrandFilterChanged: rebuildRows()
    onCurrentValueChanged: {
        if (!root.expanded)
            rebuildRows()
    }
    onExpandedChanged: {
        _foldDuration = expanded ? appTheme.motionFoldOpenMs : appTheme.motionFoldCloseMs
        foldProgress = expanded ? 1 : 0
        if (expanded) {
            root.rebuildRows()
            Qt.callLater(function () {
                if (root.highlightedRow >= 0)
                    entryView.positionViewAtIndex(root.highlightedRow, ListView.Center)
                if (root.searchable)
                    searchInput.forceActiveFocus()
                else
                    root.forceActiveFocus()
            })
        } else {
            root.query = ""
            root.brandFilter = ""
        }
    }
    onControlsEnabledChanged: {
        if (!controlsEnabled)
            expanded = false
    }

    Component.onCompleted: {
        rebuildRows()
        foldProgress = expanded ? 1 : 0
        _motionArmed = true
    }

    Behavior on foldProgress {
        enabled: root._motionArmed
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : root._foldDuration
            easing.type: appTheme.motionEasing
        }
    }

    // ── One card for both states: the collapsed card is the browser header ──
    Rectangle {
        id: cardShell
        objectName: "filmStockPickerCard"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        implicitHeight: cardColumn.implicitHeight + appTheme.spaceXs * 2
        radius: appTheme.controlRadiusSmall
        color: appTheme.controlFillColor
        border.width: 1
        border.color: cardMouse.containsMouse || root.activeFocus
                      ? appTheme.textMutedColor
                      : appTheme.controlBorderColor
        opacity: root.controlsEnabled ? 1.0 : 0.55

        // Colour strip of the current entry along the top edge.
        Item {
            id: colorStrip
            objectName: "filmStockPickerStrip"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: parent.radius
            anchors.rightMargin: parent.radius
            height: 3
            visible: root.currentEntry !== null
            readonly property string shape: root.currentEntry ? String(root.currentEntry.shape) : ""
            readonly property var colors: root.currentEntry ? root.currentEntry.colors : []
            readonly property bool gradientStrip: shape === "print_paper"
                                                  || shape === "cinema_print"
                                                  || shape === "display_material"

            Rectangle {
                anchors.fill: parent
                radius: 1.5
                visible: colorStrip.gradientStrip
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: colorStrip.colors.length > 0 ? colorStrip.colors[0] : "transparent" }
                    GradientStop { position: 1; color: colorStrip.colors.length > 1 ? colorStrip.colors[1] : "transparent" }
                }
            }

            Row {
                anchors.fill: parent
                visible: !colorStrip.gradientStrip
                Repeater {
                    model: colorStrip.gradientStrip ? [] : colorStrip.colors
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        height: colorStrip.height
                        // A photo negative tab shows the brand colour on 56 %.
                        width: colorStrip.shape === "photo_negative" && colorStrip.colors.length === 2
                               ? colorStrip.width * (index === 0 ? 0.56 : 0.44)
                               : colorStrip.width / Math.max(1, colorStrip.colors.length)
                        color: String(modelData)
                    }
                }
            }
        }
    }

    ColumnLayout {
        id: cardColumn
        anchors.left: cardShell.left
        anchors.right: cardShell.right
        anchors.top: cardShell.top
        anchors.margins: appTheme.spaceXs
        spacing: appTheme.spaceXs

        // ── Header: current entry + chevron ────────────────────────────────
        Item {
            objectName: "filmStockPickerField"
            Layout.fillWidth: true
            implicitHeight: fieldRow.implicitHeight + appTheme.spaceSm

            RowLayout {
                id: fieldRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                spacing: appTheme.spaceSm

                FilmSwatch {
                    objectName: "filmStockPickerSwatch"
                    Layout.alignment: Qt.AlignVCenter
                    large: true
                    shape: root.currentEntry ? String(root.currentEntry.shape) : ""
                    colors: root.currentEntry ? root.currentEntry.colors : []
                    visible: root.currentEntry !== null
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 0

                    Label {
                        objectName: "filmStockPickerBrand"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.currentEntry ? String(root.currentEntry.brand).toLocaleUpperCase() : ""
                        color: appTheme.textMutedColor
                        font.pixelSize: appTheme.fontSizeCaption - 1
                        font.weight: appTheme.fontWeightHeading
                        font.letterSpacing: 0.8
                        elide: Text.ElideRight
                    }

                    Label {
                        objectName: "filmStockPickerName"
                        Layout.fillWidth: true
                        text: root.currentEntry ? String(root.currentEntry.shortLabel) : root.currentValue
                        color: appTheme.textColor
                        font.pixelSize: appTheme.fontSizeBody + 2
                        font.weight: appTheme.fontWeightStrong
                        wrapMode: Text.Wrap
                    }

                    Label {
                        objectName: "filmStockPickerFamily"
                        Layout.fillWidth: true
                        visible: root.showFamilyOnCard && text.length > 0
                        text: root.currentEntry ? root.familyLabel(root.currentEntry.family) : ""
                        color: appTheme.textMutedColor
                        font.pixelSize: appTheme.fontSizeCaption
                        elide: Text.ElideRight
                    }
                }

                Canvas {
                    id: chevron
                    Layout.preferredWidth: 12
                    Layout.preferredHeight: 12
                    Layout.alignment: Qt.AlignVCenter
                    antialiasing: true
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
                        function onExpandedChanged() { chevron.requestPaint() }
                    }
                }
            }

            MouseArea {
                id: cardMouse
                anchors.fill: parent
                enabled: root.controlsEnabled
                hoverEnabled: true
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: {
                    root.forceActiveFocus()
                    root.toggleExpanded()
                }
            }
        }

        // ── Browser (inside the same card) ─────────────────────────────────
        Item {
            id: browserShell
            objectName: "filmStockPickerBrowser"
            Layout.fillWidth: true
            implicitHeight: browserColumn.implicitHeight
            Layout.preferredHeight: implicitHeight * root.foldProgress
            visible: root.foldProgress > 0.001
            opacity: root.foldProgress
            clip: true

            ColumnLayout {
                id: browserColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                spacing: appTheme.spaceXs

                // Search field.
                Rectangle {
                    objectName: "filmStockPickerSearch"
                    Layout.fillWidth: true
                    visible: root.searchable
                    implicitHeight: appTheme.iconOpticalSizeCompact + appTheme.spaceSm
                    radius: appTheme.badgeRadius
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: searchInput.activeFocus ? appTheme.textMutedColor
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
                            id: searchInput
                            objectName: "filmStockPickerSearchInput"
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            text: root.query
                            enabled: root.controlsEnabled
                            color: appTheme.textColor
                            font.pixelSize: appTheme.fontSizeBody
                            selectByMouse: true
                            clip: true
                            verticalAlignment: TextInput.AlignVCenter
                            onTextEdited: root.query = text
                            Keys.onPressed: function (event) {
                                event.accepted = root.handleBrowserKey(event.key)
                            }

                            Label {
                                anchors.fill: parent
                                visible: !parent.text.length
                                text: root.searchPlaceholder
                                color: appTheme.textMutedColor
                                font.pixelSize: appTheme.fontSizeBody
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }

                // Brand filter chips.
                Flow {
                    objectName: "filmStockPickerChips"
                    Layout.fillWidth: true
                    visible: root.searchable && root.chips.length > 2
                    spacing: appTheme.spaceXs

                    Repeater {
                        model: root.chips
                        delegate: Rectangle {
                            id: chip
                            required property var modelData
                            objectName: "filmStockPickerChip_" + (modelData.name.length > 0 ? modelData.name : "all")
                            readonly property bool chipSelected: root.brandFilter === String(modelData.name)
                            implicitWidth: chipRow.implicitWidth + appTheme.spaceSm * 2
                            implicitHeight: chipRow.implicitHeight + appTheme.spaceXs * 2
                            radius: height / 2
                            color: chipSelected ? appTheme.editorListSelectedFillColor
                                                : (chipMouse.containsMouse ? appTheme.buttonHoveredFillColor
                                                                           : "transparent")
                            border.width: 1
                            border.color: chipSelected ? appTheme.editorListSelectedFillColor
                                                       : appTheme.cardBorderColor

                            Row {
                                id: chipRow
                                anchors.centerIn: parent
                                spacing: appTheme.spaceXs

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: String(chip.modelData.color).length > 0
                                    width: 8
                                    height: 8
                                    radius: 2
                                    color: String(chip.modelData.color).length > 0
                                           ? chip.modelData.color : "transparent"
                                }

                                Label {
                                    text: String(chip.modelData.label)
                                    color: chip.chipSelected ? appTheme.editorListSelectedInkColor
                                                             : appTheme.textColor
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }

                                Label {
                                    text: String(chip.modelData.count)
                                    color: chip.chipSelected ? appTheme.editorListSelectedInkColor
                                                             : appTheme.textMutedColor
                                    opacity: 0.7
                                    font.pixelSize: appTheme.fontSizeCaption
                                }
                            }

                            MouseArea {
                                id: chipMouse
                                anchors.fill: parent
                                enabled: root.controlsEnabled
                                hoverEnabled: true
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: root.brandFilter = String(chip.modelData.name)
                            }
                        }
                    }
                }

                // Entry list.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Math.max(32, Math.min(entryView.contentHeight, 340)) + appTheme.spaceXs * 2
                    radius: appTheme.badgeRadius
                    color: appTheme.cardSurfaceColor
                    border.width: 1
                    border.color: appTheme.cardBorderColor
                    clip: true

                    // The wheel scrolls the list and is always accepted, so the
                    // panel does not scroll when the list is at its end.
                    WheelHandler {
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onWheel: function (event) {
                            const step = event.pixelDelta.y !== 0
                                         ? event.pixelDelta.y
                                         : event.angleDelta.y / 120 * 48
                            const maxY = Math.max(0, entryView.contentHeight - entryView.height)
                            entryView.contentY = Math.max(0, Math.min(maxY, entryView.contentY - step))
                            event.accepted = true
                        }
                    }

                    ListView {
                        id: entryView
                        objectName: "filmStockPickerList"
                        anchors.fill: parent
                        anchors.margins: appTheme.spaceXs
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.VerticalFlick
                        model: root.rows
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        delegate: Item {
                            id: rowItem
                            required property var modelData
                            required property int index
                            readonly property string kind: String(modelData.kind)
                            readonly property var entry: kind === "entry" ? modelData.entry : null
                            readonly property string entryValue: entry ? String(entry.value) : ""
                            readonly property bool rowSelected: entry !== null && entryValue === root.currentValue
                            readonly property bool rowHighlighted: index === root.highlightedRow
                            objectName: kind === "entry" ? "filmStockEntry_" + entryValue
                                                         : "filmStockRow_" + kind
                            width: entryView.width
                            height: kind === "brand" ? 26 : (kind === "family" ? 20 : 32)

                            // Brand heading.
                            Row {
                                visible: rowItem.kind === "brand"
                                anchors.left: parent.left
                                anchors.leftMargin: appTheme.spaceXs
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 4
                                spacing: appTheme.spaceSm

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 14
                                    height: 4
                                    radius: 2
                                    color: rowItem.kind === "brand" ? rowItem.modelData.color : "transparent"
                                }

                                Label {
                                    text: rowItem.kind === "brand" ? String(rowItem.modelData.text) : ""
                                    color: appTheme.textColor
                                    font.pixelSize: appTheme.fontSizeCaption - 1
                                    font.weight: appTheme.fontWeightHeading
                                    font.letterSpacing: 1.0
                                }

                                Label {
                                    text: rowItem.kind === "brand" ? String(rowItem.modelData.count) : ""
                                    color: appTheme.textMutedColor
                                    font.pixelSize: appTheme.fontSizeCaption - 1
                                }
                            }

                            // Family caption.
                            Label {
                                visible: rowItem.kind === "family"
                                anchors.left: parent.left
                                anchors.leftMargin: appTheme.spaceSm
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 2
                                text: rowItem.kind === "family" ? String(rowItem.modelData.text) : ""
                                color: appTheme.textMutedColor
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            // Entry.
                            Rectangle {
                                visible: rowItem.kind === "entry"
                                anchors.fill: parent
                                anchors.topMargin: 1
                                anchors.bottomMargin: 1
                                radius: appTheme.badgeRadius
                                color: rowItem.rowSelected
                                       ? appTheme.buttonSelectedFillColor
                                       : (entryMouse.containsMouse || rowItem.rowHighlighted
                                          ? appTheme.buttonHoveredFillColor
                                          : "transparent")
                                border.width: rowItem.rowSelected ? 1 : 0
                                border.color: appTheme.controlBorderColor

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: appTheme.spaceSm
                                    anchors.rightMargin: appTheme.spaceSm
                                    spacing: appTheme.spaceSm

                                    FilmSwatch {
                                        Layout.alignment: Qt.AlignVCenter
                                        shape: rowItem.entry ? String(rowItem.entry.shape) : ""
                                        colors: rowItem.entry ? rowItem.entry.colors : []
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignVCenter
                                        text: rowItem.entry ? String(rowItem.entry.shortLabel) : ""
                                        color: appTheme.textColor
                                        font.pixelSize: appTheme.fontSizeBody
                                        font.weight: rowItem.rowSelected ? appTheme.fontWeightStrong
                                                                         : appTheme.fontWeightRegular
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        Layout.alignment: Qt.AlignVCenter
                                        visible: rowItem.rowSelected
                                        text: "✓"
                                        color: appTheme.textColor
                                        font.pixelSize: appTheme.fontSizeBody
                                        font.weight: appTheme.fontWeightHeading
                                    }
                                }

                                MouseArea {
                                    id: entryMouse
                                    anchors.fill: parent
                                    enabled: root.controlsEnabled && rowItem.kind === "entry"
                                    hoverEnabled: true
                                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    onEntered: root.highlightedRow = rowItem.index
                                    onClicked: root.pickValue(rowItem.entryValue)
                                }
                            }
                        }
                    }

                    Label {
                        objectName: "filmStockPickerEmpty"
                        anchors.centerIn: parent
                        visible: entryView.count === 0
                        text: root.emptyText
                        color: appTheme.textMutedColor
                        font.pixelSize: appTheme.fontSizeCaption
                    }
                }
            }
        }
    }
}
