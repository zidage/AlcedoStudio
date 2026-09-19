import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: combo

    property var options: []
    property string selectedValue: ""
    property string selectedLabel: ""
    property string valueRole: "value"
    property string labelRole: "label"
    property string subtitleRole: ""
    property string placeholderText: qsTr("Search")
    property string emptyText: qsTr("No matches")
    property color textColor: "#F5F1EA"
    property color mutedTextColor: "#B6B0A7"
    property color accentColor: "#9FC7D8"
    property color backgroundColor: Qt.rgba(1, 1, 1, 0.10)
    property color focusedBackgroundColor: Qt.rgba(1, 1, 1, 0.14)
    property color borderColor: Qt.rgba(1, 1, 1, 0.12)
    property color focusedBorderColor: Qt.rgba(0.62, 0.78, 0.85, 0.62)
    property color popupColor: "#202124"
    property color pressedItemColor: Qt.rgba(0.27, 0.48, 0.62, 0.24)
    property color hoverItemColor: Qt.rgba(1, 1, 1, 0.10)
    property string dataFontFamily: ""
    property bool syncingText: false
    property bool replaceTextOnEdit: true
    readonly property var filteredOptions: {
        const token = searchField.text.trim().toLocaleLowerCase()
        const source = combo.options || []
        if (token.length === 0) {
            return source
        }

        const matches = []
        for (let i = 0; i < source.length; ++i) {
            const item = source[i]
            const label = combo.optionLabel(item).toLocaleLowerCase()
            const subtitle = combo.optionSubtitle(item).toLocaleLowerCase()
            const value = combo.optionValue(item).toLocaleLowerCase()
            if (label.indexOf(token) >= 0 || subtitle.indexOf(token) >= 0
                    || value.indexOf(token) >= 0) {
                matches.push(item)
            }
        }
        return matches
    }
    signal itemSelected(var item)

    implicitHeight: 42

    function roleValue(item, roleName) {
        if (!item || roleName.length === 0) {
            return ""
        }
        const value = item[roleName]
        return value === undefined || value === null ? "" : String(value)
    }

    function optionValue(item) {
        return roleValue(item, combo.valueRole)
    }

    function optionLabel(item) {
        const label = roleValue(item, combo.labelRole)
        return label.length > 0 ? label : optionValue(item)
    }

    function optionSubtitle(item) {
        const subtitle = roleValue(item, combo.subtitleRole)
        const label = optionLabel(item)
        return subtitle.length > 0 && subtitle !== label ? subtitle : ""
    }

    function selectedOption() {
        const source = combo.options || []
        for (let i = 0; i < source.length; ++i) {
            if (optionValue(source[i]) === combo.selectedValue) {
                return source[i]
            }
        }
        return null
    }

    function syncSelectedText() {
        combo.syncingText = true
        const item = selectedOption()
        if (item) {
            searchField.text = optionLabel(item)
        } else if (combo.selectedLabel.length > 0) {
            searchField.text = combo.selectedLabel
        } else {
            searchField.text = combo.selectedValue
        }
        combo.replaceTextOnEdit = true
        combo.syncingText = false
    }

    function openSuggestions(showAll) {
        if (!combo.enabled || (combo.options || []).length === 0) {
            return
        }
        if (showAll) {
            combo.syncingText = true
            searchField.text = ""
            combo.replaceTextOnEdit = false
            combo.syncingText = false
        }
        suggestionPopup.open()
        searchField.forceActiveFocus()
    }

    function choose(item) {
        if (!item) {
            return
        }
        combo.syncingText = true
        searchField.text = optionLabel(item)
        combo.replaceTextOnEdit = true
        combo.syncingText = false
        suggestionPopup.close()
        combo.itemSelected(item)
    }

    onOptionsChanged: syncSelectedText()
    onSelectedLabelChanged: syncSelectedText()
    onSelectedValueChanged: syncSelectedText()
    Component.onCompleted: syncSelectedText()

    Rectangle {
        anchors.fill: parent
        radius: 10
        color: combo.enabled && (searchField.activeFocus || suggestionPopup.opened)
               ? combo.focusedBackgroundColor : combo.backgroundColor
        border.width: 1
        border.color: combo.enabled && (searchField.activeFocus || suggestionPopup.opened)
                      ? combo.focusedBorderColor : combo.borderColor
        opacity: combo.enabled ? 1.0 : 0.45
    }

    Label {
        anchors.left: searchField.left
        anchors.right: searchField.right
        anchors.verticalCenter: searchField.verticalCenter
        visible: searchField.text.length === 0 && (combo.options || []).length > 0
        text: combo.placeholderText
        color: combo.mutedTextColor
        opacity: 0.78
        font.family: combo.dataFontFamily
        font.pixelSize: 13
        elide: Text.ElideRight
    }

    TextInput {
        id: searchField
        anchors.left: parent.left
        anchors.right: dropButton.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: 12
        anchors.rightMargin: 4
        enabled: combo.enabled
        color: combo.textColor
        selectByMouse: true
        font.family: combo.dataFontFamily
        font.pixelSize: 13
        verticalAlignment: TextInput.AlignVCenter
        onActiveFocusChanged: {
            if (activeFocus) {
                selectAll()
                combo.openSuggestions(false)
            }
        }
        onTextEdited: {
            if (!combo.syncingText) {
                combo.openSuggestions(false)
            }
        }
        Keys.onPressed: function(event) {
            if (!combo.syncingText && combo.replaceTextOnEdit && event.text.length > 0) {
                combo.syncingText = true
                searchField.text = ""
                combo.syncingText = false
                combo.replaceTextOnEdit = false
            }
        }
        onAccepted: {
            const matches = combo.filteredOptions
            if (matches.length > 0) {
                combo.choose(matches[0])
            }
        }
        Keys.onDownPressed: {
            if (combo.filteredOptions.length > 0) {
                suggestionPopup.open()
                resultList.currentIndex = Math.max(0, resultList.currentIndex)
                resultList.forceActiveFocus()
            }
        }
        Keys.onEscapePressed: suggestionPopup.close()
    }

    Rectangle {
        id: dropButton
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: parent.height
        radius: 10
        color: dropHit.pressed
               ? Qt.rgba(1, 1, 1, 0.08)
               : (dropHit.containsMouse ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 1, 1, 0.0))

        Label {
            anchors.centerIn: parent
            text: suggestionPopup.opened ? "^" : "v"
            color: combo.mutedTextColor
            font.pixelSize: 12
            font.weight: 800
        }

        MouseArea {
            id: dropHit
            anchors.fill: parent
            enabled: combo.enabled
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: suggestionPopup.opened ? suggestionPopup.close() : combo.openSuggestions(true)
        }
    }

    Popup {
        id: suggestionPopup
        parent: combo
        x: 0
        y: combo.height + 6
        width: combo.width
        height: Math.min(280, Math.max(64, resultList.contentHeight + 12))
        padding: 6
        modal: false
        focus: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        background: Rectangle {
            radius: 10
            color: combo.popupColor
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.14)
        }

        contentItem: Item {
            implicitWidth: suggestionPopup.width
            implicitHeight: suggestionPopup.height

            ListView {
                id: resultList
                anchors.fill: parent
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                currentIndex: 0
                model: combo.filteredOptions

                delegate: Rectangle {
                    id: resultRow
                    width: ListView.view.width
                    height: Math.max(modelSubtitle.length > 0 ? 56 : 44,
                                     resultColumn.implicitHeight + appTheme.spaceMd)
                    radius: 7
                    color: rowHit.pressed
                           ? combo.pressedItemColor
                           : (rowHit.containsMouse || ListView.isCurrentItem
                              ? combo.hoverItemColor : Qt.rgba(1, 1, 1, 0.0))

                    readonly property string modelLabel: combo.optionLabel(modelData)
                    readonly property string modelSubtitle: combo.optionSubtitle(modelData)

                    ColumnLayout {
                        id: resultColumn
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 2

                        Item { Layout.fillHeight: true }

                        Label {
                            Layout.fillWidth: true
                            text: resultRow.modelLabel
                            color: combo.textColor
                            font.pixelSize: 13
                            font.weight: 700
                            wrapMode: Text.Wrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: resultRow.modelSubtitle.length > 0
                            text: resultRow.modelSubtitle
                            color: combo.mutedTextColor
                            font.family: combo.dataFontFamily
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }

                        Item { Layout.fillHeight: true }
                    }

                    MouseArea {
                        id: rowHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: resultList.currentIndex = index
                        onClicked: combo.choose(modelData)
                    }
                }

                Keys.onReturnPressed: combo.choose(combo.filteredOptions[currentIndex])
                Keys.onEnterPressed: combo.choose(combo.filteredOptions[currentIndex])
                Keys.onEscapePressed: {
                    suggestionPopup.close()
                    searchField.forceActiveFocus()
                }
            }

            Label {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                visible: resultList.count === 0
                text: combo.emptyText
                color: combo.mutedTextColor
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.Wrap
            }
        }
    }
}
