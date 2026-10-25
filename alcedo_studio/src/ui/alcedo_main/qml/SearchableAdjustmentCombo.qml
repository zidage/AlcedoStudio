import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Enum adjustment combo with a search field, for long entry lists (issue #411:
// the LUT input and output encodings). Binds to an EditorAdjustmentEnumModel
// (entries: [{value, label, ...}, ...]) like AdjustmentCombo, with the same
// label, raised field and reset button.
//
// The popup opens with an empty search field that has focus. Typing filters
// the list with LensCatalogPicker's loose search: both sides are lower-cased
// and stripped to [a-z0-9], and every query token must appear in the entry's
// label, value or group label. "canon 3 2020" finds "Canon Log 3 / Rec.2020".
// Groups with no match are hidden; with no match at all the list shows a
// message. Up and Down move the highlight, Return or Enter selects it, Escape
// closes without a change.
//
// Only a selection writes: it calls model.selectIndex, one settled transaction.
// The search text is UI state only and is cleared each time the popup opens.
Item {
    id: root
    objectName: "searchableAdjustmentCombo"

    property var model: null
    property bool showResetButton: true
    // Editor panels stay dense (28), as in AdjustmentCombo.
    property int controlHeight: 28
    // Object name of the field; the search input and the list derive theirs
    // from it ("<name>Search", "<name>List", "<name>Empty").
    property string controlObjectName: "searchableAdjustmentCombo"
    // Entry key whose text heads each run of entries with the same value (for
    // example Scene / Display). Empty: the list has no headings.
    property string groupLabelRole: ""
    property string searchPlaceholder: qsTr("Search")
    // Height limit of the popup list.
    property int listMaxHeight: 280

    // ── Filter state (UI only; never submitted) ────────────────────────────
    property string query: ""
    // Visible rows: {index: entry index, label, heading: group text or ""}.
    property var filteredRows: []
    property int highlightedRow: -1
    readonly property int rowCount: filteredRows.length
    readonly property bool popupOpen: popup.opened

    readonly property var entries: root.model ? root.model.entries : []
    readonly property int currentIndex: root.model ? root.model.currentIndex : -1
    readonly property string displayText: root.entryLabel(root.currentIndex)

    implicitHeight: comboRow.implicitHeight
    Layout.fillWidth: true

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colBorder: appTheme.cardBorderColor
    // Raised control fill: the field reads apart from card and panel surfaces.
    readonly property color colField: appTheme.controlFillColor
    readonly property color colPopup: appTheme.bgBaseColor
    readonly property color colHover: appTheme.hoverColor
    readonly property color colSelectedFill: appTheme.editorListSelectedFillColor
    readonly property color colSelectedInk: appTheme.editorListSelectedInkColor

    // ── Matching ───────────────────────────────────────────────────────────
    function normalizeText(text) {
        return String(text || "").toLocaleLowerCase().replace(/[^a-z0-9]+/g, "")
    }

    function queryTokens(text) {
        const raw = String(text || "").toLocaleLowerCase().split(/[^a-z0-9]+/)
        var tokens = []
        for (var i = 0; i < raw.length; ++i) {
            if (raw[i].length > 0)
                tokens.push(raw[i])
        }
        return tokens
    }

    function entryLabel(index) {
        const list = root.entries
        if (!list || index < 0 || index >= list.length || !list[index])
            return ""
        const entry = list[index]
        return entry.label !== undefined ? String(entry.label) : String(entry.value || "")
    }

    function entryGroup(entry) {
        if (root.groupLabelRole.length === 0 || !entry || entry[root.groupLabelRole] === undefined)
            return ""
        return String(entry[root.groupLabelRole])
    }

    function entryMatches(entry, text) {
        const trimmed = String(text || "").trim()
        if (trimmed.length === 0)
            return true
        const haystack = root.normalizeText(String(entry.label || "") + " " + String(entry.value || "")
                                            + " " + root.entryGroup(entry))
        const joined = root.normalizeText(trimmed)
        if (joined.length === 0 || haystack.indexOf(joined) >= 0)
            return true
        const tokens = root.queryTokens(trimmed)
        for (var i = 0; i < tokens.length; ++i) {
            if (haystack.indexOf(tokens[i]) < 0)
                return false
        }
        return tokens.length > 0
    }

    function refilter() {
        var rows = []
        var lastGroup = null
        const items = root.entries || []
        for (var i = 0; i < items.length; ++i) {
            const entry = items[i]
            if (!entry || !root.entryMatches(entry, root.query))
                continue
            const group = root.entryGroup(entry)
            rows.push({
                index: i,
                label: root.entryLabel(i),
                heading: group.length > 0 && group !== lastGroup ? group : ""
            })
            lastGroup = group
        }
        root.filteredRows = rows
        root.highlightedRow = root.rowOfIndex(root.currentIndex)
        if (root.highlightedRow < 0 && rows.length > 0 && root.query.length > 0)
            root.highlightedRow = 0
        // Center the current entry when the popup opens; keep the highlight in view while typing.
        const mode = root.query.length === 0 ? ListView.Center : ListView.Contain
        if (root.highlightedRow >= 0)
            Qt.callLater(function () { list.positionViewAtIndex(root.highlightedRow, mode) })
    }

    function rowOfIndex(index) {
        for (var i = 0; i < root.filteredRows.length; ++i) {
            if (root.filteredRows[i].index === index)
                return i
        }
        return -1
    }

    // ── Popup and selection ────────────────────────────────────────────────
    function openPopup() {
        if (!root.model || !root.model.enabled)
            return
        searchInput.text = ""
        root.query = ""
        root.refilter()
        popup.open()
    }

    function moveHighlight(step) {
        if (root.filteredRows.length === 0)
            return
        const next = root.highlightedRow < 0
                     ? (step > 0 ? 0 : root.filteredRows.length - 1)
                     : Math.max(0, Math.min(root.highlightedRow + step, root.filteredRows.length - 1))
        root.highlightedRow = next
        list.positionViewAtIndex(next, ListView.Contain)
    }

    function pickRow(row) {
        if (row < 0 || row >= root.filteredRows.length)
            return
        const index = root.filteredRows[row].index
        popup.close()
        if (root.model)
            root.model.selectIndex(index)
    }

    function pickHighlighted() {
        root.pickRow(root.highlightedRow)
    }

    onEntriesChanged: if (popup.opened) root.refilter()

    RowLayout {
        id: comboRow
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Label {
            Layout.preferredWidth: 76
            text: root.model ? root.model.label : ""
            color: root.colText
            font.pixelSize: appTheme.fontSizeBody
            wrapMode: Text.Wrap
            visible: root.model && root.model.label && root.model.label.length > 0
            Accessible.name: root.model ? root.model.label : ""
        }

        AbstractButton {
            id: field
            objectName: root.controlObjectName
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(root.controlHeight, fieldText.implicitHeight)
            enabled: !!root.model && root.model.enabled
            activeFocusOnTab: true
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            // Read by tests and accessibility tooling, like ComboBox.displayText.
            readonly property string displayText: root.displayText
            Accessible.role: Accessible.ComboBox
            Accessible.name: root.model ? root.model.label : ""
            Accessible.description: root.displayText
            onClicked: popup.visible ? popup.close() : root.openPopup()
            Keys.onDownPressed: root.openPopup()
            Keys.onUpPressed: root.openPopup()

            background: Rectangle {
                implicitHeight: root.controlHeight
                radius: appTheme.controlRadiusSmall
                color: root.colField
                border.width: 1
                border.color: field.activeFocus || field.hovered || popup.visible
                              ? root.colMuted
                              : appTheme.controlBorderColor
            }

            contentItem: Text {
                id: fieldText
                leftPadding: appTheme.spaceSm
                rightPadding: appTheme.spaceMd + appTheme.spaceSm
                topPadding: appTheme.spaceXs
                bottomPadding: appTheme.spaceXs
                text: root.displayText
                font.pixelSize: appTheme.fontSizeBody
                color: field.enabled ? root.colText : root.colMuted
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: appTheme.spaceSm
                anchors.verticalCenter: parent.verticalCenter
                text: "▾"
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
                opacity: field.enabled ? 1.0 : 0.45
            }

            Popup {
                id: popup
                y: field.height + 2
                width: field.width
                padding: 1
                focus: true
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                onOpened: searchInput.forceActiveFocus()
                onClosed: field.forceActiveFocus()

                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: root.colPopup
                    border.width: 1
                    border.color: root.colBorder
                }

                contentItem: ColumnLayout {
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.controlHeight
                        Layout.margins: appTheme.spaceXs
                        radius: appTheme.controlRadiusSmall
                        color: appTheme.bgBaseColor
                        border.width: 1
                        border.color: searchInput.activeFocus ? root.colMuted : root.colBorder

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
                                color: root.colMuted
                            }

                            TextInput {
                                id: searchInput
                                objectName: root.controlObjectName + "Search"
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                color: root.colText
                                font.pixelSize: appTheme.fontSizeBody
                                selectByMouse: true
                                clip: true
                                verticalAlignment: TextInput.AlignVCenter
                                Accessible.role: Accessible.EditableText
                                Accessible.name: root.searchPlaceholder
                                onTextChanged: {
                                    if (root.query === text)
                                        return
                                    root.query = text
                                    root.refilter()
                                }
                                Keys.onDownPressed: root.moveHighlight(1)
                                Keys.onUpPressed: root.moveHighlight(-1)
                                Keys.onReturnPressed: root.pickHighlighted()
                                Keys.onEnterPressed: root.pickHighlighted()
                                Keys.onEscapePressed: popup.close()

                                Label {
                                    anchors.fill: parent
                                    visible: !parent.text.length
                                    text: root.searchPlaceholder
                                    color: root.colMuted
                                    font.pixelSize: appTheme.fontSizeBody
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                    }

                    Label {
                        objectName: root.controlObjectName + "Empty"
                        Layout.fillWidth: true
                        visible: root.filteredRows.length === 0
                        text: qsTr("No matches")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeBody
                        leftPadding: appTheme.spaceSm
                        topPadding: appTheme.spaceXs
                        bottomPadding: appTheme.spaceSm
                    }

                    ListView {
                        id: list
                        objectName: root.controlObjectName + "List"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, root.listMaxHeight)
                        visible: root.filteredRows.length > 0
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: root.filteredRows
                        currentIndex: root.highlightedRow
                        ScrollIndicator.vertical: ScrollIndicator {}

                        delegate: Item {
                            id: row
                            required property var modelData
                            required property int index
                            readonly property bool highlighted: root.highlightedRow === index
                            readonly property bool selected: modelData.index === root.currentIndex
                            width: ListView.view ? ListView.view.width : 0
                            height: rowContent.implicitHeight

                            Column {
                                id: rowContent
                                width: parent.width
                                spacing: 0

                                Text {
                                    visible: row.modelData.heading.length > 0
                                    width: parent.width
                                    text: row.modelData.heading
                                    color: root.colMuted
                                    font.pixelSize: appTheme.fontSizeCaption
                                    elide: Text.ElideRight
                                    leftPadding: appTheme.spaceSm
                                    topPadding: appTheme.spaceSm
                                    bottomPadding: appTheme.spaceXs
                                }

                                Rectangle {
                                    width: parent.width
                                    height: Math.max(root.controlHeight, entryText.implicitHeight)
                                    radius: 4
                                    color: row.highlighted
                                           ? root.colSelectedFill
                                           : (rowMouse.containsMouse ? root.colHover : "transparent")

                                    Text {
                                        id: entryText
                                        anchors.fill: parent
                                        leftPadding: appTheme.spaceSm
                                        rightPadding: appTheme.spaceSm
                                        topPadding: appTheme.spaceXs
                                        bottomPadding: appTheme.spaceXs
                                        text: row.modelData.label
                                        color: row.highlighted ? root.colSelectedInk : root.colText
                                        font.pixelSize: appTheme.fontSizeBody
                                        font.weight: row.selected ? appTheme.fontWeightHeading
                                                                  : Font.Normal
                                        wrapMode: Text.WordWrap
                                        verticalAlignment: Text.AlignVCenter
                                    }

                                    MouseArea {
                                        id: rowMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onEntered: root.highlightedRow = row.index
                                        onClicked: root.pickRow(row.index)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        AdjustmentResetButton {
            model: root.model
            visible: root.showResetButton
        }
    }
}
