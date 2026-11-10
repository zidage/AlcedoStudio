import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Album-inspector date filter: calendar tiles or a GitHub-style activity graph.
// Style is local; both views consume the same dateStats list and emit one
// day-clicked label for StatsEngine.ToggleStatsFilter("date", …). The header band, the fold
// state, and the header actions come from InspectorSection.
InspectorSection {
    id: section
    objectName: "dateFilterSection"

    property var model: []
    property string selectedLabel: ""
    property var folderKey: 0
    property string styleKey: "calendar"
    /// False shows only the calendar tiles, without the activity graph and the style switch.
    property bool activityAvailable: true
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

    SlidingIconNav {
        id: styleNav
        objectName: "dateFilterStyleNav"
        visible: section.contentLive && section.activityAvailable
        Layout.alignment: Qt.AlignLeft
        currentKey: section.styleKey
        items: section.styleItems
        thumbObjectName: "dateFilterStyleNavThumb"
        trackColor: appTheme.bgCanvasColor
        onActivated: function(key) {
            section.styleKey = key
        }
    }

    Loader {
        id: calendarLoader
        objectName: "dateFilterCalendarLoader"
        Layout.fillWidth: true
        active: section.contentLive
                && (section.styleKey === "calendar" || !section.activityAvailable)
        visible: status === Loader.Ready
        Layout.preferredHeight: (status === Loader.Ready && item) ? item.implicitHeight : 0
        sourceComponent: calendarComponent
    }

    Loader {
        id: graphLoader
        objectName: "dateFilterGraphLoader"
        Layout.fillWidth: true
        active: section.contentLive && section.activityAvailable
                && section.styleKey === "activity"
        asynchronous: true
        visible: status === Loader.Ready
        Layout.preferredHeight: (status === Loader.Ready && item) ? item.implicitHeight : 0
        sourceComponent: graphComponent
    }

    Component {
        id: calendarComponent
        StatsCard {
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
            onDayClicked: function(label) { section.dayClicked(label) }
        }
    }
}
