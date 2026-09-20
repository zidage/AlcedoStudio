pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Alcedo.Main 1.0

// Settings > Keyboard page. Scrollable grouped list over ShortcutRegistry —
// registry order is preserved within each group and only settingsVisible rows
// render. The panel enforces a single active ShortcutCaptureField so only one
// binding can be recorded at a time; all mutations go through the registry's
// validate/save/clear/restore APIs and the displayed rows read the model's
// effective state back after dataChanged.
ColumnLayout {
    id: panel
    objectName: "keyboardSettingsPanel"

    // Theme mirrors — SettingDialog passes its palette; appTheme is the default.
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color accentColor: appTheme.accentColor
    property color canvasColor: appTheme.bgBaseColor
    property color dividerColor: appTheme.dividerColor
    property color panelBorderColor: appTheme.cardBorderColor
    property color hoverColor: appTheme.hoverColor
    property color dangerColor: appTheme.dangerColor
    property string dataFontFamily: appTheme.dataFontFamily
    property string uiFontFamily: appTheme.uiFontFamily

    // The one capture session allowed across the whole page.
    property ShortcutCaptureField activeCaptureField: null

    function requestCapture(field) {
        if (activeCaptureField && activeCaptureField !== field) {
            activeCaptureField.cancelCapture()
        }
        activeCaptureField = field
    }

    function releaseCapture(field) {
        if (activeCaptureField === field) {
            activeCaptureField = null
        }
    }

    spacing: 0

    Label {
        objectName: "keyboardSettingsHint"
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.topMargin: 26
        Layout.bottomMargin: 8
        text: qsTr("Select a shortcut field, then type the new input. Enter or Escape saves, Tab cancels, plain Delete clears the binding.")
        color: panel.mutedTextColor
        font.family: panel.uiFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        wrapMode: Text.Wrap
    }

    // Instantiator (not Repeater) creates rows synchronously: the Repeater's
    // delegate-model path never completes under an offscreen test window, and
    // the row count is small enough that eager creation is fine either way.
    ColumnLayout {
        id: rowsColumn
        Layout.fillWidth: true
        spacing: 0

        Instantiator {
            id: rowRepeater
            objectName: "shortcutRowRepeater"
            model: ShortcutRegistry

            // Created rows are parented to the Instantiator itself; hand them
            // to rowsColumn so the layout manages their position and width.
            onObjectAdded: function(index, object) {
                object.parent = rowsColumn
            }

        delegate: ColumnLayout {
            id: row

            required property int index
            required property string commandId
            required property string groupText
            required property string descriptionText
            required property string bindingText
            required property string defaultBindingText
            required property bool assigned
            required property bool usesDefault
            required property int inputKind
            required property bool settingsVisible
            required property string validationError

            // First visible row of its settings group. Walks earlier siblings
            // rather than trusting index 0 so filtering stays correct.
            readonly property bool groupStart: {
                for (let i = index - 1; i >= 0; --i) {
                    const sibling = rowRepeater.objectAt(i)
                    if (sibling && sibling.visible) {
                        return sibling.groupText !== groupText
                    }
                }
                return true
            }
            // Live capture error wins; otherwise surface the row's stored
            // validation error (e.g. a rejected commit from another surface).
            readonly property string rowError:
                captureField.errorText.length > 0
                ? captureField.errorText : validationError

            objectName: "shortcutRow:" + commandId
            visible: settingsVisible
            spacing: 6
            Layout.fillWidth: true
            Layout.leftMargin: 34
            Layout.rightMargin: 34
            Layout.topMargin: groupStart ? 14 : 0
            Layout.bottomMargin: groupStart ? 4 : 2

            ColumnLayout {
                Layout.fillWidth: true
                visible: row.groupStart
                spacing: 8

                Label {
                    objectName: "shortcutGroupHeader"
                    property string groupKey: row.groupText
                    Layout.fillWidth: true
                    text: row.groupText
                    color: panel.textColor
                    font.family: panel.uiFontFamily
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: appTheme.fontWeightHeading
                    wrapMode: Text.Wrap
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: panel.dividerColor
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        objectName: "shortcutRowDescription:" + row.commandId
                        Layout.fillWidth: true
                        text: row.descriptionText
                        color: panel.textColor
                        font.family: panel.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        font.weight: appTheme.fontWeightStrong
                        wrapMode: Text.Wrap
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: row.inputKind === 1
                        text: qsTr("Presses and releases a single modifier key")
                        color: panel.mutedTextColor
                        font.family: panel.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        wrapMode: Text.Wrap
                    }
                }

                ShortcutCaptureField {
                    id: captureField
                    objectName: "shortcutCaptureField:" + row.commandId
                    Layout.preferredWidth: 190
                    Layout.alignment: Qt.AlignTop
                    commandId: row.commandId
                    inputKind: row.inputKind
                    bindingText: row.bindingText
                    descriptionText: row.descriptionText
                    assigned: row.assigned
                    usesDefault: row.usesDefault
                    captureOwner: panel
                    textColor: panel.textColor
                    mutedTextColor: panel.mutedTextColor
                    accentColor: panel.accentColor
                    fillColor: panel.canvasColor
                    borderColor: panel.panelBorderColor
                    dangerColor: panel.dangerColor
                    dataFontFamily: panel.dataFontFamily
                }

                IconActionButton {
                    id: clearButton
                    objectName: "shortcutClearButton:" + row.commandId
                    Layout.alignment: Qt.AlignTop
                    compact: true
                    iconSrc: "qrc:/panel_icons/close.svg"
                    actionName: qsTr("Clear shortcut for %1").arg(row.descriptionText)
                    toolTipText: qsTr("Clear shortcut")
                    enabled: !captureField.capturing
                    onClicked: captureField.clearBinding()
                }

                Button {
                    id: restoreButton
                    objectName: "shortcutRestoreButton:" + row.commandId
                    visible: !row.usesDefault
                    enabled: !captureField.capturing
                    text: qsTr("Restore Default")
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredHeight: 32
                    leftPadding: appTheme.spaceSm
                    rightPadding: appTheme.spaceSm
                    activeFocusOnTab: visible
                    Accessible.name: qsTr("Restore default shortcut for %1").arg(row.descriptionText)
                    Accessible.description: qsTr("Restores %1").arg(row.defaultBindingText)
                    onClicked: captureField.restoreDefault()

                    contentItem: Label {
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: restoreButton.text
                        color: restoreButton.enabled
                               ? (restoreButton.hovered || restoreButton.activeFocus
                                  ? panel.textColor : panel.mutedTextColor)
                               : panel.mutedTextColor
                        font.family: panel.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        elide: Text.ElideRight
                    }

                    background: Rectangle {
                        radius: appTheme.controlRadiusSmall
                        color: restoreButton.enabled && restoreButton.hovered
                               ? panel.hoverColor : "transparent"
                        border.width: restoreButton.activeFocus ? 1 : 0
                        border.color: Qt.rgba(panel.accentColor.r, panel.accentColor.g,
                                              panel.accentColor.b, 0.60)
                    }

                    ToolTip.visible: hovered && ToolTip.text.length > 0
                    ToolTip.text: qsTr("Restore default: %1").arg(row.defaultBindingText)
                    ToolTip.delay: 400
                }
            }

            Label {
                objectName: "shortcutCaptureHint:" + row.commandId
                Layout.fillWidth: true
                visible: captureField.capturing
                text: row.inputKind === 1
                      ? qsTr("Press and release Shift, Ctrl, Alt, or Meta to choose it.")
                      : qsTr("Type the new shortcut keys.")
                color: panel.mutedTextColor
                font.family: panel.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
            }

            Label {
                objectName: "shortcutRowError:" + row.commandId
                Layout.fillWidth: true
                visible: row.rowError.length > 0
                text: row.rowError
                color: panel.dangerColor
                font.family: panel.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
            }

            Label {
                objectName: "shortcutRowErrorScope:" + row.commandId
                Layout.fillWidth: true
                visible: captureField.conflictScope.length > 0
                text: qsTr("Conflicting scope: %1").arg(captureField.conflictScope)
                color: panel.mutedTextColor
                font.family: panel.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
            }
        }
    }
    }

    Item { Layout.preferredHeight: 26 }
}
