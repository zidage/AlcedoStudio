import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root

    property alias pattern: patternField.text
    property bool controlsEnabled: true
    property string sampleSourceName: "DSCF2074"
    property string outputExtension: ".jpg"
    property var savedPresets: []
    property var savePreset: null
    property var deletePreset: null
    property bool namingPreset: false
    property bool presetNameInvalid: false
    property string selectedSavedPresetName: ""

    readonly property bool patternValid: validatePattern(pattern)
    readonly property string previewName: makePreview(pattern) + outputExtension
    readonly property bool customPattern: activePresetIndex === presetEntries.length - 1
    readonly property var activePresetEntry: presetEntries[activePresetIndex]
    readonly property bool canDeletePreset: !namingPreset && !customPattern
                                             && activePresetEntry
                                             && activePresetEntry.deletable === true
    readonly property int activePresetIndex: {
        for (let i = 0; i < presetEntries.length; ++i) {
            if (presetEntries[i].pattern !== undefined
                    && presetEntries[i].pattern === pattern)
                return i
        }
        return presetEntries.length - 1
    }

    readonly property var presetEntries: {
        const entries = [
            { label: qsTr("Source"), pattern: "{source}" },
            { label: qsTr("Date + No."), pattern: "{date:yyyy MM dd}-{sequence:0000}" },
            { label: qsTr("Custom + No."),
              pattern: qsTr("Export") + "-{sequence:0000}" }
        ]
        const stored = root.savedPresets || []
        for (let i = 0; i < stored.length; ++i) {
            const entry = stored[i]
            if (entry && entry.name && entry.pattern) {
                entries.push({
                    label: String(entry.name),
                    pattern: String(entry.pattern),
                    deletable: true
                })
            }
        }
        entries.push({ label: qsTr("Custom") })
        return entries
    }
    readonly property var fieldEntries: [
        { label: qsTr("Source"), token: "{source}" },
        { label: qsTr("Date"), token: "{date:yyyy MM dd}" },
        { label: qsTr("Sequence"), token: "{sequence:0000}" },
        { label: qsTr("Make"), token: "{cameraMake}" },
        { label: qsTr("Camera"), token: "{cameraModel}" },
        { label: qsTr("Lens"), token: "{lens}" },
        { label: qsTr("ISO"), token: "{iso}" },
        { label: qsTr("Aperture"), token: "{aperture}" },
        { label: qsTr("Shutter"), token: "{shutter}" },
        { label: qsTr("Focal"), token: "{focal}" },
        { label: qsTr("Rating"), token: "{rating}" }
    ]

    spacing: appTheme.spaceSm

    onActivePresetIndexChanged: {
        const entry = presetEntries[activePresetIndex]
        if (entry && entry.pattern !== undefined)
            root.selectedSavedPresetName = entry.deletable ? String(entry.label) : ""
    }

    function selectPreset(entry) {
        root.selectedSavedPresetName = entry.deletable ? String(entry.label) : ""
        patternField.text = String(entry.pattern)
        patternField.cursorPosition = patternField.text.length
        patternField.forceActiveFocus()
    }

    function beginPresetNaming() {
        if (!root.customPattern || !root.patternValid)
            return
        root.presetNameInvalid = false
        presetNameField.text = root.selectedSavedPresetName
        root.namingPreset = true
        Qt.callLater(function() {
            presetNameField.forceActiveFocus()
            presetNameField.selectAll()
        })
    }

    function saveNamedPreset() {
        const name = presetNameField.text.trim()
        if (name.length === 0 || name.length > 80 || !root.patternValid
                || typeof root.savePreset !== "function") {
            root.presetNameInvalid = true
            return false
        }
        if (root.savePreset(name, root.pattern, root.selectedSavedPresetName)) {
            root.selectedSavedPresetName = name
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

    function deleteSavedPreset(name) {
        if (typeof root.deletePreset !== "function" || !root.deletePreset(name))
            return false
        if (root.selectedSavedPresetName.toLowerCase() === String(name).toLowerCase())
            root.selectedSavedPresetName = ""
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
            const entry = entries[index]
            if (entry.pattern !== undefined)
                root.selectPreset(entry)
        }

    }

    function insertToken(token) {
        const position = Math.max(0, patternField.cursorPosition)
        patternField.insert(position, token)
        patternField.cursorPosition = position + token.length
        patternField.forceActiveFocus()
    }

    function tokenValue(token) {
        const separator = token.indexOf(":")
        const field = (separator >= 0 ? token.substring(0, separator) : token).toLowerCase()
        const argument = separator >= 0 ? token.substring(separator + 1) : ""
        switch (field) {
        case "source": return sampleSourceName.replace(/\.[^.]+$/, "")
        case "date": {
            let dateFormat = argument.length > 0 ? argument : "yyyyMMdd"
            dateFormat = dateFormat.replace(/yyyy/g, "2026")
                                   .replace(/MM/g, "08")
                                   .replace(/dd/g, "10")
                                   .replace(/HH/g, "12")
                                   .replace(/mm/g, "34")
                                   .replace(/ss/g, "56")
            return dateFormat
        }
        case "sequence": {
            const width = argument.length
            return width > 1 ? "1".padStart(width, "0") : "1"
        }
        case "cameramake": return "FUJIFILM"
        case "cameramodel": return "X-T5"
        case "lens": return "XF 33mm F1.4"
        case "iso": return "400"
        case "aperture": return "1.4"
        case "shutter": return "1-250"
        case "focal": return "33.0mm"
        case "rating": return "5"
        default: return ""
        }
    }

    function validatePattern(value) {
        if (!value || value.length === 0)
            return false
        const protectedOpen = value.replace(/\{\{/g, "").replace(/\}\}/g, "")
        let depth = 0
        for (let i = 0; i < protectedOpen.length; ++i) {
            if (protectedOpen[i] === "{")
                depth += 1
            else if (protectedOpen[i] === "}")
                depth -= 1
            if (depth < 0)
                return false
        }
        if (depth !== 0)
            return false
        let valid = true
        protectedOpen.replace(/\{([^{}]+)\}/g, function(match, token) {
            const separator = token.indexOf(":")
            const field = (separator >= 0 ? token.substring(0, separator) : token).toLowerCase()
            const argument = separator >= 0 ? token.substring(separator + 1) : ""
            const known = ["source", "date", "sequence", "cameramake", "cameramodel",
                           "lens", "iso", "aperture", "shutter", "focal", "rating"]
            if (known.indexOf(field) < 0 || (field === "sequence" && /[^0]/.test(argument)))
                valid = false
            return match
        })
        return valid
    }

    function makePreview(value) {
        if (!validatePattern(value))
            return qsTr("Invalid pattern")
        const openMarker = "\uE000"
        const closeMarker = "\uE001"
        let preview = value.replace(/\{\{/g, openMarker).replace(/\}\}/g, closeMarker)
        preview = preview.replace(/\{([^{}]+)\}/g, function(match, token) {
            return root.tokenValue(token)
        })
        return preview.replace(new RegExp(openMarker, "g"), "{")
                      .replace(new RegExp(closeMarker, "g"), "}")
    }

    Label {
        text: qsTr("Preset")
        color: appTheme.textMutedColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        font.weight: appTheme.fontWeightStrong
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: appTheme.spaceXs

        AdjustmentCombo {
            objectName: "exportNamingPresetControl"
            controlObjectName: "exportNamingPresetCombo"
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
                objectName: "exportNamingPresetNameField"
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
            objectName: "exportNamingSavePresetButton"
            compact: true
            iconSrc: "qrc:/panel_icons/plus.svg"
            actionName: root.namingPreset
                        ? qsTr("Save naming preset")
                        : qsTr("Name custom preset")
            enabled: root.controlsEnabled && root.patternValid
                     && (root.customPattern || root.namingPreset)
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
            objectName: "exportNamingDeletePresetButton"
            compact: true
            iconSrc: "qrc:/panel_icons/trash.svg"
            actionName: qsTr("Delete naming preset")
            enabled: root.controlsEnabled && root.canDeletePreset
            iconColorDefault: enabled ? appTheme.iconColor : appTheme.textMutedColor
            fillIdle: appTheme.bgBaseColor
            fillHover: appTheme.buttonHoveredFillColor
            onClicked: {
                if (root.canDeletePreset)
                    root.deleteSavedPreset(String(root.activePresetEntry.label))
            }
        }
    }

    Label {
        text: qsTr("Pattern")
        color: appTheme.textMutedColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        font.weight: appTheme.fontWeightStrong
    }

    TextField {
        id: patternField
        objectName: "exportFileNamePatternField"
        Layout.fillWidth: true
        Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
        text: "{source}"
        enabled: root.controlsEnabled
        selectByMouse: true
        activeFocusOnTab: true
        Accessible.name: qsTr("File name pattern")
        font.family: appTheme.dataFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        color: appTheme.textColor
        background: Rectangle {
            radius: appTheme.controlRadiusSmall
            color: appTheme.bgBaseColor
            border.width: 1
            border.color: root.patternValid ? appTheme.cardBorderColor : appTheme.dangerColor
        }
    }

    Label {
        text: qsTr("Insert field")
        color: appTheme.textMutedColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        font.weight: appTheme.fontWeightStrong
    }

    Flow {
        Layout.fillWidth: true
        Layout.preferredHeight: childrenRect.height
        spacing: appTheme.spaceXs

        Repeater {
            model: root.fieldEntries

            delegate: Button {
                required property var modelData

                implicitWidth: Math.max(contentItem.implicitWidth + appTheme.spaceMd,
                                        appTheme.iconButtonHitSizeCompact)
                implicitHeight: appTheme.iconButtonHitSizeCompact
                enabled: root.controlsEnabled
                activeFocusOnTab: true
                Accessible.name: qsTr("Insert %1 field").arg(modelData.label)
                onClicked: root.insertToken(modelData.token)
                contentItem: Label {
                    text: parent.modelData.label
                    color: parent.enabled ? appTheme.textColor : appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: parent.down ? appTheme.buttonPressedFillColor
                           : (parent.hovered ? appTheme.buttonHoveredFillColor
                                             : appTheme.bgBaseColor)
                    border.width: 1
                    border.color: appTheme.cardBorderColor
                }
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: previewColumn.implicitHeight + appTheme.spaceMd
        radius: appTheme.controlRadiusSmall
        color: appTheme.bgBaseColor
        border.width: 1
        border.color: root.patternValid ? appTheme.cardBorderColor : appTheme.dangerColor

        ColumnLayout {
            id: previewColumn
            anchors.fill: parent
            anchors.margins: appTheme.spaceSm
            spacing: appTheme.spaceXs

            Label {
                text: root.patternValid ? qsTr("Example") : qsTr("Pattern error")
                color: root.patternValid ? appTheme.textMutedColor : appTheme.dangerColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
            }

            Label {
                Layout.fillWidth: true
                text: root.previewName
                color: root.patternValid ? appTheme.textColor : appTheme.dangerColor
                font.family: appTheme.dataFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                elide: Text.ElideMiddle
            }
        }
    }
}
