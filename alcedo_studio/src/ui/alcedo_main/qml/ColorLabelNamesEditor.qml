import QtQuick
import QtQml.Models
import QtQuick.Layouts

// Settings > Library > Color Labels (#770): one row per color label code (0 Red ... 6 Gray):
// a color dot, a plain TextInput in a hairline box with the translated color name as the
// placeholder, and a reset glyph on a renamed row. The editor holds no state of its own: the
// caller passes the pending user names and applies them (SettingDialog pending values).
ColumnLayout {
    id: root
    objectName: "colorLabelNamesEditor"

    // User name of each code in code order; an empty entry is a code without a user name.
    property var userNames: []
    // Translated color name of each code in code order, shown as the placeholder.
    property var defaultNames: []
    // Color of each code in code order (appTheme.colorLabelColors).
    property var colors: appTheme.colorLabelColors

    // The user typed in the field of @p code or pressed its reset glyph (@p name is empty).
    signal nameEdited(int code, string name)

    spacing: 0

    function userName(code) {
        const names = root.userNames || []
        return code < names.length && names[code] ? String(names[code]) : ""
    }

    // Applies the name rule of LibraryViewPreferences to the typed text of @p input: control
    // characters go, line breaks and tabs become spaces, and the text keeps 40 characters with
    // surrogate pairs whole (the rule of library.color_label_name.set). A changed text is
    // replaced and the field binds to the pending name again; the cursor stays after the same
    // typed character.
    function editName(row, input) {
        const typed = input.text
        const allowed = appModules.libraryView.EditableColorLabelName(typed)
        if (allowed === typed) {
            root.nameEdited(row.index, typed)
            return
        }
        const cursor = Math.min(
            allowed.length,
            appModules.libraryView.EditableColorLabelName(typed.substring(0, input.cursorPosition)).length)
        root.nameEdited(row.index, allowed)
        input.text = Qt.binding(function() { return row.name })
        input.cursorPosition = cursor
    }

    function withAlpha(color, alpha) {
        return Qt.rgba(color.r, color.g, color.b, alpha)
    }

    // Instantiator, not Repeater: the Repeater's delegate model never completes under an
    // offscreen test window (the SettingDialog category nav has the same note). The list is
    // fixed at seven rows.
    Instantiator {
        model: root.colors ? root.colors.length : 0

        onObjectAdded: function(index, object) {
            object.parent = root
        }

        delegate: RowLayout {
            id: row

            required property int index
            readonly property string name: root.userName(index)

            objectName: "colorLabelNameRow:" + index
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            spacing: 10

            Item {
                Layout.preferredWidth: 22
                Layout.fillHeight: true

                Rectangle {
                    objectName: "colorLabelDot"
                    anchors.centerIn: parent
                    width: 12
                    height: 12
                    radius: 6
                    color: root.colors[row.index]
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                radius: appTheme.controlRadiusSmall
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: nameInput.activeFocus ? appTheme.textMutedColor : appTheme.dividerColor

                TextInput {
                    id: nameInput
                    objectName: "colorLabelNameInput:" + row.index
                    anchors.fill: parent
                    anchors.leftMargin: 9
                    anchors.rightMargin: 9
                    verticalAlignment: TextInput.AlignVCenter
                    clip: true
                    selectByMouse: true
                    text: row.name
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    selectedTextColor: appTheme.textColor
                    selectionColor: root.withAlpha(appTheme.textMutedColor, 0.6)
                    Accessible.name: placeholder.text
                    onTextEdited: root.editName(row, nameInput)

                    Text {
                        id: placeholder
                        objectName: "colorLabelNamePlaceholder"
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        visible: nameInput.text.length === 0 && nameInput.preeditText.length === 0
                        text: row.index < (root.defaultNames || []).length
                              ? String(root.defaultNames[row.index]) : ""
                        color: appTheme.textMutedColor
                        font: nameInput.font
                        elide: Text.ElideRight
                    }
                }
            }

            // The reset glyph keeps its cell on every row so the fields keep one width.
            Item {
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28

                IconActionButton {
                    objectName: "colorLabelNameReset:" + row.index
                    anchors.fill: parent
                    visible: row.name.length > 0
                    compact: true
                    iconSrc: "qrc:/panel_icons/reset.svg"
                    iconColorDefault: appTheme.textMutedColor
                    fillIdle: "transparent"
                    actionName: qsTr("Reset %1").arg(placeholder.text)
                    onClicked: root.nameEdited(row.index, "")
                }
            }
        }
    }
}
