import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Export preset picker: the built-in and saved presets of ExportRecipeBuilder, the preset that
// matches the panel options, and the save and delete actions of the presets the user saved.
ColumnLayout {
    id: root

    // ExportRecipeBuilder (appModules.exportRecipe).
    property var recipe: null
    // The panel options in the option form of ExportRecipeBuilder.
    property var currentOptions: ({})
    property bool optionsValid: true
    property bool controlsEnabled: true
    property bool namingPreset: false
    property bool presetNameInvalid: false
    // The user preset that was selected last; saving under its name replaces it.
    property string selectedUserPresetId: ""

    // The options of the selected preset, applied over currentOptions (the destination stays).
    signal presetSelected(var options)

    readonly property var presets: root.recipe ? root.recipe.presets : []
    readonly property string activePresetId: {
        if (!root.recipe)
            return ""
        // Reevaluate when the user presets change.
        const presetCount = root.presets.length
        return presetCount > 0 ? root.recipe.MatchingPresetIdFor(root.currentOptions) : ""
    }
    readonly property bool customOptions: root.activePresetId.length === 0
    readonly property var presetEntries: {
        const entries = []
        const source = root.presets || []
        for (let i = 0; i < source.length; ++i) {
            entries.push({
                id: String(source[i].id),
                label: root.presetLabel(source[i]),
                builtIn: source[i].builtIn === true
            })
        }
        if (root.customOptions)
            entries.push({ id: "", label: qsTr("Custom") })
        return entries
    }
    readonly property int activePresetIndex: {
        for (let i = 0; i < root.presetEntries.length; ++i) {
            if (root.presetEntries[i].id === root.activePresetId)
                return i
        }
        return Math.max(0, root.presetEntries.length - 1)
    }
    readonly property var activePresetEntry: root.presetEntries[root.activePresetIndex]
    readonly property bool canSavePreset: root.controlsEnabled && root.optionsValid
                                          && (root.customOptions || root.namingPreset)
    readonly property bool canDeletePreset: root.controlsEnabled && !root.namingPreset
                                            && !root.customOptions && root.activePresetEntry
                                            && root.activePresetEntry.builtIn === false

    spacing: appTheme.spaceSm

    onActivePresetIdChanged: {
        if (root.activePresetId.length > 0)
            root.selectedUserPresetId = root.activePresetId.startsWith("user:")
                    ? root.activePresetId : ""
    }

    function presetLabel(entry) {
        switch (String(entry.id)) {
        case "builtin:standard": return qsTr("Standard")
        case "builtin:web": return qsTr("Web")
        case "builtin:social": return qsTr("Social Media")
        case "builtin:email": return qsTr("Email")
        case "builtin:max_jpeg": return qsTr("Maximum Quality JPEG")
        case "builtin:tiff16": return qsTr("16-bit TIFF")
        case "builtin:exr_archive": return qsTr("EXR Archive")
        default: return String(entry.name)
        }
    }

    function selectPreset(presetId) {
        if (!root.recipe || presetId.length === 0)
            return
        root.presetSelected(root.recipe.OptionsWithPreset(root.currentOptions, presetId))
    }

    function userPresetName(presetId) {
        return presetId.startsWith("user:") ? presetId.substring(5) : ""
    }

    function beginPresetNaming() {
        if (!root.canSavePreset)
            return
        root.presetNameInvalid = false
        presetNameField.text = root.userPresetName(root.selectedUserPresetId)
        root.namingPreset = true
        Qt.callLater(function() {
            presetNameField.forceActiveFocus()
            presetNameField.selectAll()
        })
    }

    function saveNamedPreset() {
        const name = presetNameField.text.trim()
        if (!root.recipe || name.length === 0 || name.length > 80 || !root.optionsValid) {
            root.presetNameInvalid = true
            return false
        }
        const result = root.recipe.SavePreset(name, root.currentOptions, root.selectedUserPresetId)
        if (result.saved) {
            root.selectedUserPresetId = String(result.id)
            root.namingPreset = false
            root.presetNameInvalid = false
            return true
        }
        root.presetNameInvalid = true
        return false
    }

    function cancelPresetNaming() {
        root.namingPreset = false
        root.presetNameInvalid = false
    }

    function deletePreset(presetId) {
        if (!root.recipe || !root.recipe.DeletePreset(presetId))
            return false
        if (root.selectedUserPresetId === presetId)
            root.selectedUserPresetId = ""
        return true
    }

    QtObject {
        id: presetModel

        property string label: ""
        property var entries: root.presetEntries
        property int currentIndex: root.activePresetIndex
        property bool enabled: root.controlsEnabled

        function selectIndex(index) {
            if (index < 0 || index >= entries.length)
                return
            root.selectPreset(String(entries[index].id))
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: appTheme.spaceXs

        AdjustmentCombo {
            objectName: "exportPresetControl"
            controlObjectName: "exportPresetCombo"
            Layout.fillWidth: true
            visible: !root.namingPreset
            model: presetModel
            showResetButton: false
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            visible: root.namingPreset

            TextField {
                id: presetNameField
                objectName: "exportPresetNameField"
                anchors.fill: parent
                enabled: root.controlsEnabled
                selectByMouse: true
                activeFocusOnTab: true
                maximumLength: 80
                placeholderText: qsTr("Preset name")
                Accessible.name: qsTr("Preset name")
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                color: appTheme.textColor
                onTextEdited: root.presetNameInvalid = false
                onAccepted: root.saveNamedPreset()
                onActiveFocusChanged: {
                    if (!activeFocus && root.namingPreset) {
                        Qt.callLater(function() {
                            if (root.namingPreset && !savePresetButton.activeFocus)
                                root.saveNamedPreset()
                        })
                    }
                }
                Keys.onEscapePressed: function(event) {
                    root.cancelPresetNaming()
                    event.accepted = true
                }
                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: root.presetNameInvalid
                                  ? appTheme.dangerColor
                                  : appTheme.cardBorderColor
                }
                ToolTip.visible: root.presetNameInvalid
                ToolTip.text: qsTr("Enter a valid preset name.")
            }
        }

        IconActionButton {
            id: savePresetButton
            objectName: "exportSavePresetButton"
            compact: true
            iconSrc: "qrc:/panel_icons/plus.svg"
            actionName: root.namingPreset ? qsTr("Save export preset") : qsTr("Save as preset")
            enabled: root.canSavePreset
            fillIdle: appTheme.bgBaseColor
            fillHover: appTheme.buttonHoveredFillColor
            onClicked: {
                if (root.namingPreset)
                    root.saveNamedPreset()
                else
                    root.beginPresetNaming()
            }
        }

        IconActionButton {
            id: deletePresetButton
            objectName: "exportDeletePresetButton"
            compact: true
            iconSrc: "qrc:/panel_icons/trash.svg"
            actionName: qsTr("Delete export preset")
            enabled: root.canDeletePreset
            iconColorDefault: enabled ? appTheme.iconColor : appTheme.textMutedColor
            fillIdle: appTheme.bgBaseColor
            fillHover: appTheme.buttonHoveredFillColor
            onClicked: {
                if (root.canDeletePreset)
                    root.deletePreset(String(root.activePresetEntry.id))
            }
        }
    }
}
