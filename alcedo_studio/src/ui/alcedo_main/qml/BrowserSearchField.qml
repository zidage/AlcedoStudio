import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// The search field of the browser toolbars (LUT browser, preset browser): a sunken
// track with the search icon, the text, a muted placeholder, and a clear button
// while there is text. The owner keeps the query: `text` follows it, and the field
// reports edits, Return, and clearing. Escape clears the text; with no text it asks
// the owner to move focus to the results.
Rectangle {
    id: field

    property string text: ""
    property string placeholderText: ""
    property string inputObjectName: ""
    property string clearButtonObjectName: ""
    property int chromeHeight: Math.max(appTheme.iconOpticalSizeCompact + appTheme.spaceSm,
                                        appTheme.iconButtonHitSizeCompact - appTheme.spaceSm)
    readonly property bool inputActiveFocus: searchInput.activeFocus

    signal textEdited(string text)
    signal submitted()
    signal cleared()
    signal leaveRequested()

    function focusInput() {
        searchInput.forceActiveFocus()
        searchInput.selectAll()
    }

    implicitHeight: chromeHeight
    radius: appTheme.controlRadiusSmall
    color: appTheme.bgBaseColor
    border.width: 1
    border.color: searchInput.activeFocus ? appTheme.textMutedColor : appTheme.cardBorderColor

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
            color: appTheme.textMutedColor
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextInput {
                id: searchInput
                objectName: field.inputObjectName
                anchors.fill: parent
                verticalAlignment: TextInput.AlignVCenter
                color: appTheme.textColor
                selectionColor: appTheme.editorListSelectedFillColor
                selectedTextColor: appTheme.editorListSelectedInkColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightRegular
                clip: true
                text: field.text
                Accessible.role: Accessible.EditableText
                Accessible.name: field.placeholderText
                onTextEdited: field.textEdited(text)
                // The field keeps Return and Escape: a TextInput passes a handled key on
                // to its parents, and a browser list applies its selection on Enter.
                Keys.onReturnPressed: function (event) {
                    field.submitted()
                    event.accepted = true
                }
                Keys.onEnterPressed: function (event) {
                    field.submitted()
                    event.accepted = true
                }
                Keys.onEscapePressed: function (event) {
                    if (text.length > 0)
                        field.cleared()
                    else
                        field.leaveRequested()
                    event.accepted = true
                }
            }

            Text {
                anchors.fill: searchInput
                verticalAlignment: Text.AlignVCenter
                visible: searchInput.text.length === 0 && !searchInput.activeFocus
                text: field.placeholderText
                color: appTheme.textMutedColor
                elide: Text.ElideRight
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightRegular
            }
        }

        IconActionButton {
            objectName: field.clearButtonObjectName
            visible: searchInput.text.length > 0
            Layout.preferredWidth: field.chromeHeight - appTheme.spaceXs
            Layout.preferredHeight: field.chromeHeight - appTheme.spaceXs
            Layout.minimumWidth: Layout.preferredWidth
            Layout.minimumHeight: Layout.preferredHeight
            compact: true
            iconColorDefault: appTheme.iconColor
            iconColorMuted: appTheme.textMutedColor
            fillIdle: "transparent"
            fillHover: appTheme.buttonHoveredFillColor
            fillSelected: appTheme.buttonSelectedFillColor
            iconSrc: "qrc:/panel_icons/close.svg"
            actionName: qsTr("Clear search")
            focusOnPointerPress: false
            onClicked: field.cleared()
        }
    }
}
