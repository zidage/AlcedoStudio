import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: card
    implicitHeight: cardCol.implicitHeight

    property string title: ""
    property color accentColor: appTheme.accentColor
    property var model: []
    property bool expanded: true
    readonly property int previewLimit: 10
    readonly property int chipPreviewLimit: 3
    property string selectedLabel: ""
    property string displayMode: "bars"
    property bool showHeader: true
    property var yearExpansion: ({})
    signal barClicked(string label)

    readonly property var groupedItems: {
        if (!model) return [];
        const groups = {};
        const order = [];
        for (let i = 0; i < model.length; i++) {
            const entry = model[i];
            const label = String(entry.label);
            const count = Number(entry.count);
            const m = label.match(/^(\d{4})-(\d{2})-(\d{2})$/);
            const year = m ? m[1] : label;
            if (!groups[year]) {
                groups[year] = {
                    year: year,
                    total: 0,
                    months: [],
                    monthMap: {},
                    containsSelected: false,
                    isDateGroup: !!m
                };
                order.push(year);
            }
            const group = groups[year];
            group.total += count;
            group.containsSelected = group.containsSelected || (label === selectedLabel);
            if (m) {
                const month = m[2];
                if (!group.monthMap[month]) {
                    group.monthMap[month] = { month: month, total: 0, items: [] };
                    group.months.push(group.monthMap[month]);
                }
                const monthGroup = group.monthMap[month];
                monthGroup.total += count;
                monthGroup.items.push({
                    label: label,
                    month: month,
                    day: m[3],
                    count: count
                });
            }
        }
        return order.map(function(y) {
            const group = groups[y];
            delete group.monthMap;
            return group;
        });
    }

    function monthName(mm) {
        const names = ["January", "February", "March", "April", "May", "June",
                       "July", "August", "September", "October", "November", "December"];
        const idx = parseInt(mm) - 1;
        return (idx >= 0 && idx < 12) ? names[idx] : mm;
    }

    function withAlpha(color, alpha) {
        return Qt.rgba(color.r, color.g, color.b, alpha);
    }

    function lensDotColor(index, isSelected) {
        if (isSelected || index === 0) return card.accentColor;
        if (index === 1) return appTheme.accentSecondaryColor;
        if (index === 2) return card.withAlpha(appTheme.textMutedColor, 0.55);
        return card.withAlpha(appTheme.textMutedColor, 0.75);
    }

    readonly property int totalItems: model ? model.length : 0
    property bool showAll: false
    readonly property var visibleItems: {
        if (!model) return [];
        if (displayMode === "grouped") return model;

        const n = model.length;
        if (displayMode === "chips") {
            if (showAll) {
                return model;
            }
            const limit = Math.min(chipPreviewLimit, n);
            const entries = [];
            for (let i = 0; i < limit; ++i) entries.push(model[i]);

            if (selectedLabel !== "") {
                let alreadyVisible = false;
                for (let i = 0; i < entries.length; ++i) {
                    if (String(entries[i].label) === selectedLabel) {
                        alreadyVisible = true;
                        break;
                    }
                }
                if (!alreadyVisible) {
                    for (let i = limit; i < n; ++i) {
                        if (String(model[i].label) === selectedLabel) {
                            if (entries.length < chipPreviewLimit) {
                                entries.push(model[i]);
                            } else if (entries.length > 0) {
                                entries[entries.length - 1] = model[i];
                            }
                            break;
                        }
                    }
                }
            }
            return entries;
        }

        const limit = showAll ? n : Math.min(n, previewLimit);
        const entries = [];
        for (let i = 0; i < limit; ++i) entries.push(model[i]);
        return entries;
    }
    readonly property int visibleCount: visibleItems.length
    readonly property int hiddenCount: Math.max(0, totalItems - visibleCount)
    readonly property bool hasOverflow: hiddenCount > 0

    function isYearExpanded(group, index) {
        if (!group) return false;
        if (yearExpansion.hasOwnProperty(group.year))
            return !!yearExpansion[group.year];
        return group.containsSelected || index === 0;
    }

    function toggleYearExpanded(group, index) {
        if (!group) return;
        const next = {};
        for (let key in yearExpansion)
            next[key] = yearExpansion[key];
        next[group.year] = !isYearExpanded(group, index);
        yearExpansion = next;
    }

    function maxCount() {
        let m = 1;
        if (!visibleItems) return m;
        for (let i = 0; i < visibleItems.length; ++i) {
            const c = Number(visibleItems[i].count);
            if (c > m) m = c;
        }
        return m;
    }

    ColumnLayout {
        id: cardCol
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            visible: card.showHeader

            Label {
                text: card.title.toUpperCase()
                color: appTheme.textMutedColor
                font.pixelSize: 10
                font.weight: 700
                font.letterSpacing: 1.6
            }
            Item { Layout.fillWidth: true }
            Label {
                text: card.expanded ? "▲" : "▼"
                color: appTheme.textMutedColor
                font.pixelSize: 9
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: card.expanded = !card.expanded
                }
            }
        }

        // --- bars ---
        ColumnLayout {
            visible: card.displayMode === "bars" && card.expanded
            Layout.fillWidth: true
            spacing: 3

            Repeater {
                model: card.visibleCount
                delegate: Item {
                    required property int index
                    Layout.fillWidth: true
                    implicitHeight: 24

                    readonly property var entry: card.visibleItems[index]
                    readonly property string entryLabel: entry ? String(entry.label) : ""
                    readonly property int entryCount: entry ? Number(entry.count) : 0
                    readonly property real fraction: entryCount / card.maxCount()
                    readonly property bool isSelected: entryLabel === card.selectedLabel
                                                      && card.selectedLabel !== ""

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        onClicked: card.barClicked(entryLabel)

                        Rectangle {
                            anchors.fill: parent
                            radius: 3
                            color: "#FFFFFF"
                            opacity: parent.containsMouse && !isSelected ? 0.04 : 0
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * fraction
                        height: 20
                        radius: 3
                        color: card.accentColor
                        opacity: isSelected ? 0.55 : 0.18
                    }

                    Rectangle {
                        visible: isSelected
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 3
                        height: 16
                        radius: 1.5
                        color: card.accentColor
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6

                        Label {
                            Layout.fillWidth: true
                            text: entryLabel
                            color: appTheme.textColor
                            font.family: appTheme.dataFontFamily
                            font.pixelSize: 11
                            font.weight: 400
                            elide: Text.ElideRight
                        }

                        Label {
                            text: entryCount
                            color: appTheme.textMutedColor
                            font.family: appTheme.dataFontFamily
                            font.pixelSize: 11
                            font.weight: 500
                        }
                    }
                }
            }

            Label {
                visible: card.totalItems === 0
                text: qsTr("No data available")
                color: appTheme.textMutedColor
                font.pixelSize: 11
                font.italic: true
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 4
            }
        }

        // --- chips ---
        Item {
            visible: card.displayMode === "chips" && card.expanded
            Layout.fillWidth: true
            implicitHeight: chipsFlow.implicitHeight

            Flow {
                id: chipsFlow
                width: parent.width
                spacing: 8

                Repeater {
                    model: card.visibleCount
                    delegate: Rectangle {
                        required property int index
                        readonly property var entry: card.visibleItems[index]
                        readonly property string entryLabel: entry ? String(entry.label) : ""
                        readonly property bool isSelected: entryLabel === card.selectedLabel
                                                          && card.selectedLabel !== ""

                        id: chip
                        height: 38
                        radius: 19
                        implicitWidth: Math.min(chipLabel.implicitWidth + 28, 170)
                        color: isSelected
                            ? card.withAlpha(appTheme.selectedTintColor, 0.26)
                            : chipHitArea.containsMouse
                                ? card.withAlpha(appTheme.hoverColor, 0.78)
                                : card.withAlpha(appTheme.bgBaseColor, 0.62)
                        border.width: 1
                        border.color: isSelected
                            ? card.withAlpha(card.accentColor, 0.55)
                            : card.withAlpha(appTheme.glassStrokeColor, 0.35)

                        MouseArea {
                            id: chipHitArea
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            hoverEnabled: true
                            onClicked: card.barClicked(entryLabel)
                        }

                        Label {
                            id: chipLabel
                            anchors.centerIn: parent
                            width: Math.min(implicitWidth, 142)
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: entryLabel
                            color: isSelected ? card.accentColor : appTheme.textColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: 11
                            font.weight: isSelected ? 700 : 600
                        }
                    }
                }

                Rectangle {
                    visible: card.hiddenCount > 0
                    height: 38
                    radius: 19
                    implicitWidth: moreLabel.implicitWidth + 28
                    color: card.withAlpha(appTheme.bgBaseColor, 0.35)
                    border.width: 1
                    border.color: card.withAlpha(appTheme.glassStrokeColor, 0.3)

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        onClicked: card.showAll = true
                    }

                    Label {
                        id: moreLabel
                        anchors.centerIn: parent
                        text: "+" + card.hiddenCount + qsTr(" more")
                        color: appTheme.textColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: 11
                        font.weight: 700
                    }
                }

                Rectangle {
                    visible: card.showAll && card.totalItems > card.chipPreviewLimit
                    height: 38
                    radius: 19
                    implicitWidth: collapseLabel.implicitWidth + 28
                    color: card.withAlpha(appTheme.bgBaseColor, 0.35)
                    border.width: 1
                    border.color: card.withAlpha(appTheme.glassStrokeColor, 0.3)

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        onClicked: card.showAll = false
                    }

                    Label {
                        id: collapseLabel
                        anchors.centerIn: parent
                        text: qsTr("Show less")
                        color: appTheme.textColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: 11
                        font.weight: 700
                    }
                }

                Label {
                    visible: card.totalItems === 0
                    text: qsTr("No data available")
                    color: appTheme.textMutedColor
                    font.pixelSize: 11
                    font.italic: true
                }
            }
        }

        // --- dots ---
        ColumnLayout {
            visible: card.displayMode === "dots" && card.expanded
            Layout.fillWidth: true
            spacing: 6

            Repeater {
                model: card.visibleCount
                delegate: Item {
                    required property int index
                    Layout.fillWidth: true
                    implicitHeight: 42

                    readonly property var entry: card.visibleItems[index]
                    readonly property string entryLabel: entry ? String(entry.label) : ""
                    readonly property int entryCount: entry ? Number(entry.count) : 0
                    readonly property bool isSelected: entryLabel === card.selectedLabel
                                                      && card.selectedLabel !== ""

                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: isSelected
                            ? card.withAlpha(appTheme.bgBaseColor, 0.92)
                            : dotHitArea.containsMouse
                                ? card.withAlpha(appTheme.hoverColor, 0.86)
                                : "transparent"
                        border.width: isSelected ? 1 : 0
                        border.color: card.withAlpha(card.accentColor, 0.42)
                    }

                    MouseArea {
                        id: dotHitArea
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        onClicked: card.barClicked(entryLabel)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        Rectangle {
                            implicitWidth: 9
                            implicitHeight: 9
                            radius: 4.5
                            color: card.lensDotColor(index, isSelected)
                            Layout.alignment: Qt.AlignVCenter
                        }

                        Label {
                            Layout.fillWidth: true
                            text: entryLabel
                            color: isSelected ? appTheme.textColor
                                              : card.withAlpha(appTheme.textColor, 0.78)
                            font.family: appTheme.dataFontFamily
                            font.pixelSize: 12
                            font.weight: isSelected ? 600 : 500
                            elide: Text.ElideRight
                        }

                        Label {
                            text: entryCount
                            color: isSelected
                                ? card.withAlpha(appTheme.textColor, 0.82)
                                : appTheme.textMutedColor
                            font.family: appTheme.dataFontFamily
                            font.pixelSize: 12
                            font.weight: 500
                        }
                    }
                }
            }

            Label {
                visible: card.totalItems === 0
                text: qsTr("No data available")
                color: appTheme.textMutedColor
                font.pixelSize: 11
                font.italic: true
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 4
            }
        }

        // --- grouped (year + month + day tiles) ---
        ColumnLayout {
            visible: card.displayMode === "grouped" && card.expanded
            Layout.fillWidth: true
            spacing: 14

            Repeater {
                model: card.groupedItems.length

                delegate: ColumnLayout {
                    required property int index
                    Layout.fillWidth: true
                    spacing: 10

                    readonly property var group: card.groupedItems[index]
                    readonly property bool isExpanded: card.isYearExpanded(group, index)

                    Item {
                        Layout.fillWidth: true
                        implicitHeight: 28

                        RowLayout {
                            anchors.fill: parent
                            spacing: 8

                            Label {
                                text: isExpanded ? "▼" : "▶"
                                color: card.withAlpha(appTheme.textMutedColor, 0.9)
                                font.pixelSize: 8
                                font.weight: 700
                                Layout.alignment: Qt.AlignVCenter
                            }

                            Label {
                                text: group ? group.year : ""
                                color: group && group.containsSelected ? card.accentColor : appTheme.textColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: 13
                                font.weight: 700
                                Layout.alignment: Qt.AlignVCenter
                            }

                            Item { Layout.fillWidth: true }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: card.toggleYearExpanded(group, index)
                        }
                    }

                    ColumnLayout {
                        visible: group && group.isDateGroup && group.months.length > 0 && isExpanded
                        Layout.fillWidth: true
                        Layout.leftMargin: 18
                        spacing: 12

                        Repeater {
                            model: group ? group.months.length : 0

                            delegate: ColumnLayout {
                                required property int index
                                Layout.fillWidth: true
                                spacing: 8

                                readonly property var monthGroup: group.months[index]

                                Label {
                                    text: monthGroup ? card.monthName(monthGroup.month).toUpperCase() : ""
                                    color: card.withAlpha(appTheme.textMutedColor, 0.96)
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: 11
                                    font.weight: 700
                                    font.letterSpacing: 1.1
                                }

                                Item {
                                    Layout.fillWidth: true
                                    implicitHeight: dateFlow.implicitHeight

                                    Flow {
                                        id: dateFlow
                                        width: parent.width
                                        spacing: 6

                                        Repeater {
                                            model: monthGroup ? monthGroup.items.length : 0

                                            delegate: Rectangle {
                                                required property int index
                                                readonly property var dayEntry: monthGroup.items[index]
                                                readonly property bool isSelected: dayEntry
                                                    ? dayEntry.label === card.selectedLabel
                                                    : false

                                                id: dateTile
                                                width: 44
                                                height: 50
                                                radius: 3
                                                color: isSelected
                                                    ? card.withAlpha(appTheme.selectedTintColor, 0.18)
                                                    : dateHitArea.containsMouse
                                                        ? card.withAlpha(appTheme.hoverColor, 0.72)
                                                        : card.withAlpha(appTheme.bgBaseColor, 0.44)
                                                border.width: 1
                                                border.color: isSelected
                                                    ? card.withAlpha(card.accentColor, 0.75)
                                                    : card.withAlpha(appTheme.glassStrokeColor, 0.18)

                                                MouseArea {
                                                    id: dateHitArea
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    hoverEnabled: true
                                                    onClicked: card.barClicked(dayEntry ? dayEntry.label : "")
                                                }

                                                Column {
                                                    anchors.centerIn: parent
                                                    spacing: 1

                                                    Label {
                                                        anchors.horizontalCenter: parent.horizontalCenter
                                                        text: dayEntry ? dayEntry.day : ""
                                                        color: isSelected ? card.accentColor : appTheme.textColor
                                                        font.family: appTheme.dataFontFamily
                                                        font.pixelSize: 18
                                                        font.weight: 700
                                                    }

                                                    Label {
                                                        anchors.horizontalCenter: parent.horizontalCenter
                                                        text: dayEntry ? dayEntry.count : ""
                                                        color: isSelected
                                                            ? card.withAlpha(card.accentColor, 0.9)
                                                            : appTheme.textMutedColor
                                                        font.family: appTheme.dataFontFamily
                                                        font.pixelSize: 11
                                                        font.weight: 600
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Label {
                visible: card.groupedItems.length === 0
                text: qsTr("No data available")
                color: appTheme.textMutedColor
                font.pixelSize: 11
                font.italic: true
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 4
            }
        }

        Label {
            visible: card.hasOverflow
                     && card.expanded
                     && (card.displayMode === "bars" || card.displayMode === "dots")
            text: card.showAll
                ? qsTr("Show less ▲")
                : qsTr("Show all %1 ▼").arg(card.totalItems)
            color: card.accentColor
            font.pixelSize: 10
            font.underline: true
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 2

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: card.showAll = !card.showAll
            }
        }
    }
}
