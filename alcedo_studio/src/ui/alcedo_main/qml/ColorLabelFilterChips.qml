import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl

// Content of the By Custom Label section of the Library Overview (#772): one chip per color
// label code in code order (0 Red ... 6 Gray), always present, then "Deleted". A chip shows the
// color, the user name of the code (Settings > Library > Color Labels), and the photos of the
// match set with it. A click toggles the filter of that chip (StatsEngine "colorLabel" with the
// code, or "deleted"). The chips follow the section card rules of DESIGN.md "Library workspace
// chrome" (controlFillColor; the selected chip is the editorListSelectedFillColor well).
Flow {
    id: root
    objectName: "colorLabelFilterChips"

    // User names of the codes, index = code.
    property var names: appModules.libraryView.colorLabelNames
    property var colors: appTheme.colorLabelColors
    // Photos of the match set with each code, index = code.
    property var counts: appModules.stats.colorLabelCounts
    property int deletedCount: appModules.stats.deleteMarkCount
    // Code of the active color label filter, or -1.
    property int selectedCode: appModules.stats.statsFilterColorLabel
    property bool deletedSelected: appModules.stats.statsFilterDeleteMark

    signal codeClicked(int code)
    signal deletedClicked()

    spacing: 8

    // The count in at most three significant digits: below 10,000 the grouped count; above it K
    // and M (12.8K, 102K, 1.25M), or 万 and 亿 in Chinese (1.28万). Same rule as the large counts
    // of the Settings pages (DESIGN.md "Data text").
    function compactCount(value) {
        const locale = Qt.locale()
        const count = Math.max(0, Math.floor(Number(value) || 0))
        if (count < 10000)
            return count.toLocaleString(locale, "f", 0)
        const language = typeof languageManager !== "undefined" && languageManager
                         ? languageManager.currentLanguageCode : locale.name
        const chinese = String(language).startsWith("zh")
        const units = chinese ? [[100000000, "亿"], [10000, "万"]]
                              : [[1000000000, "B"], [1000000, "M"], [1000, "K"]]
        for (let index = 0; index < units.length; ++index) {
            const size = units[index][0]
            if (count >= size) {
                const scaled = count / size
                const decimals = scaled >= 100 ? 0 : (scaled >= 10 ? 1 : 2)
                const factor = Math.pow(10, decimals)
                let text = (Math.floor(scaled * factor) / factor).toLocaleString(locale, "f",
                                                                                 decimals)
                if (decimals > 0) {
                    while (text.endsWith("0"))
                        text = text.slice(0, -1)
                    if (text.endsWith(locale.decimalPoint))
                        text = text.slice(0, -locale.decimalPoint.length)
                }
                return text + units[index][1]
            }
        }
        return count.toLocaleString(locale, "f", 0)
    }

    component Chip: Rectangle {
        id: chip
        property string name: ""
        property int count: 0
        property bool selected: false
        property color dotColor: "transparent"
        property bool showDot: true
        property bool showTrash: false
        property string countText: ""
        signal clicked()

        // The height and the shape of the By Labels chips (StatsCard).
        height: 38
        radius: height / 2
        implicitWidth: chipRow.implicitWidth + 24
        color: selected ? appTheme.editorListSelectedFillColor
                        : (chipHit.containsMouse ? appTheme.controlHoveredFillColor
                                                 : appTheme.controlFillColor)
        border.width: 1
        border.color: selected ? appTheme.editorListSelectedFillColor : appTheme.cardBorderColor

        Accessible.role: Accessible.Button
        Accessible.name: name
        Accessible.checkable: true
        Accessible.checked: selected
        Accessible.onPressAction: chip.clicked()

        Row {
            id: chipRow
            anchors.centerIn: parent
            spacing: 6

            Rectangle {
                visible: chip.showDot
                anchors.verticalCenter: parent.verticalCenter
                width: 10
                height: 10
                radius: 5
                color: chip.dotColor
            }

            ColorImage {
                visible: chip.showTrash
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                source: "qrc:/panel_icons/trash.svg"
                sourceSize.width: 24
                sourceSize.height: 24
                color: chip.selected ? appTheme.editorListSelectedInkColor : appTheme.textMutedColor
            }

            Label {
                objectName: "colorLabelChipName"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, 120)
                elide: Text.ElideRight
                text: chip.name
                color: chip.selected ? appTheme.editorListSelectedInkColor : appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: 11
                font.weight: chip.selected ? 700 : 600
            }

            Label {
                objectName: "colorLabelChipCount"
                anchors.verticalCenter: parent.verticalCenter
                text: chip.countText
                color: chip.selected ? appTheme.editorListSelectedInkColor : appTheme.textMutedColor
                font.family: appTheme.dataFontFamily
                font.pixelSize: 11
                font.weight: 500
                font.features: { "tnum": 1 }
            }
        }

        MouseArea {
            id: chipHit
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.clicked()
        }
    }

    Repeater {
        model: root.colors ? root.colors.length : 0

        Chip {
            required property int index
            objectName: "colorLabelChip_" + index
            name: root.names && index < root.names.length ? String(root.names[index]) : ""
            count: root.counts && index < root.counts.length ? Number(root.counts[index]) : 0
            countText: root.compactCount(count)
            dotColor: root.colors[index]
            selected: root.selectedCode === index
            onClicked: root.codeClicked(index)
        }
    }

    Chip {
        objectName: "colorLabelChipDeleted"
        name: qsTr("Deleted")
        count: root.deletedCount
        countText: root.compactCount(root.deletedCount)
        showDot: false
        showTrash: true
        selected: root.deletedSelected
        onClicked: root.deletedClicked()
    }
}
