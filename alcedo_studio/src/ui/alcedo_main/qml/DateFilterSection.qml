import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Album-inspector date filter: calendar tiles or a GitHub-style activity graph.
// Style is local; both views consume the same dateStats list and emit one
// day-clicked label for StatsEngine.ToggleStatsFilter("date", …).
ColumnLayout {
    id: section
    objectName: "dateFilterSection"
    spacing: appTheme.spaceSm

    property string title: ""
    property var model: []
    property string selectedLabel: ""
    property var folderKey: 0
    property color accentColor: appTheme.toneSteel
    property bool expanded: true
    property string styleKey: "calendar"
    signal dayClicked(string label)

    readonly property var styleItems: [
        {
            key: "calendar",
            icon: "qrc:/panel_icons/calendar.svg",
            label: qsTr("Calendar"),
            itemObjectName: "dateFilterStyleCalendar"
        },
        {
            key: "activity",
            icon: "qrc:/panel_icons/layout-grid.svg",
            label: qsTr("Activity"),
            itemObjectName: "dateFilterStyleActivity"
        }
    ]

    RowLayout {
        Layout.fillWidth: true
        spacing: appTheme.spaceXs

        Label {
            text: section.title.toUpperCase()
            color: appTheme.textMutedColor
            font.pixelSize: 10
            font.weight: 700
            font.letterSpacing: 1.6
            Layout.alignment: Qt.AlignVCenter
        }

        Item { Layout.fillWidth: true }

        Label {
            text: section.expanded ? "▲" : "▼"
            color: appTheme.textMutedColor
            font.pixelSize: 9
            Layout.alignment: Qt.AlignVCenter
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: section.expanded = !section.expanded
            }
        }
    }

    SlidingIconNav {
        id: styleNav
        objectName: "dateFilterStyleNav"
        visible: section.expanded
        Layout.alignment: Qt.AlignLeft
        currentKey: section.styleKey
        items: section.styleItems
        thumbObjectName: "dateFilterStyleNavThumb"
        onActivated: function(key) {
            section.styleKey = key
        }
    }

    Loader {
        id: calendarLoader
        objectName: "dateFilterCalendarLoader"
        Layout.fillWidth: true
        active: section.expanded && section.styleKey === "calendar"
        visible: status === Loader.Ready
        Layout.preferredHeight: (status === Loader.Ready && item) ? item.implicitHeight : 0
        sourceComponent: calendarComponent
    }

    Loader {
        id: graphLoader
        objectName: "dateFilterGraphLoader"
        Layout.fillWidth: true
        active: section.expanded && section.styleKey === "activity"
        asynchronous: true
        visible: status === Loader.Ready
        Layout.preferredHeight: (status === Loader.Ready && item) ? item.implicitHeight : 0
        sourceComponent: graphComponent
    }

    Component {
        id: calendarComponent
        StatsCard {
            title: section.title
            showHeader: false
            accentColor: section.accentColor
            model: section.model
            selectedLabel: section.selectedLabel
            displayMode: "grouped"
            onBarClicked: function(label) { section.dayClicked(label) }
        }
    }

    Component {
        id: graphComponent
        DateCommitGraph {
            model: section.model
            selectedLabel: section.selectedLabel
            folderKey: section.folderKey
            accentColor: section.accentColor
            onDayClicked: function(label) { section.dayClicked(label) }
        }
    }
}
