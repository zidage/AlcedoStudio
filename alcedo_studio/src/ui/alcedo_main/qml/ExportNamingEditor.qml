import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root

    property alias pattern: patternField.text
    property bool controlsEnabled: true
    property string sampleSourceName: "DSCF2074"
    property string outputExtension: ".jpg"
    // ExportRecipeBuilder (appModules.exportRecipe): IsFileNamePatternValid(pattern).
    property var patternRules: null

    readonly property bool patternValid: validatePattern(pattern)
    readonly property string previewName: makePreview(pattern) + outputExtension
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

    // The pattern rule of the export (ParseExportFileNamePattern), read through
    // ExportRecipeBuilder. Without the rules object no pattern is valid.
    function validatePattern(value) {
        if (!root.patternRules || !value || value.length === 0)
            return false
        return root.patternRules.IsFileNamePatternValid(String(value))
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
                wrapMode: Text.Wrap
            }
        }
    }
}
