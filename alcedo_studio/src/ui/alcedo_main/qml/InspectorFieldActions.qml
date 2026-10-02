import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Album Inspector field header actions: ascending and descending photo-sort actions and the
// Group action of one field. The sort icons mean ORDER BY; the group icon means GROUP BY. Both
// bind to LibraryModule's accepted options and call its focused operations; this component
// keeps no copy of the choices and never changes a filter or the section expansion.
RowLayout {
    id: root
    objectName: "inspectorFieldActions_" + field

    /// Inspector field name: date, import, camera, lens, rating, or label.
    property string field: ""
    /// Field name for accessible names and tooltips, for example "capture time".
    property string fieldTitle: ""
    /// True for capture and import time (day groups, full timestamps inside a day).
    property bool timeField: false
    property string ascendingTitle: qsTr("Ascending")
    property string descendingTitle: qsTr("Descending")

    readonly property var library: appModules.library
    readonly property bool sortedAscending: library.sortField === field && !library.sortDescending
    readonly property bool sortedDescending: library.sortField === field && library.sortDescending
    readonly property bool grouped: library.groupField === field

    spacing: appTheme.spaceXs

    function sortTip(descending) {
        const order = descending ? root.descendingTitle : root.ascendingTitle
        let tip = qsTr("Sort photos by %1: %2.").arg(root.fieldTitle).arg(order)
        if (root.timeField && root.grouped) {
            tip += " " + qsTr("Inside each day, the full time orders the photos.")
        } else if (!root.timeField && (root.library.groupField === "date"
                                       || root.library.groupField === "import")) {
            tip += " " + qsTr("Equal values inside a day keep the newest time first.")
        } else if (!root.timeField && root.grouped && root.field !== "label") {
            tip += " " + qsTr("Equal values follow file ID order.")
        }
        if (!root.timeField) {
            tip += " " + qsTr("Text sorts by character code, not by language rules.")
        }
        tip += " " + qsTr("Select it again to clear the sort.")
        return tip
    }

    component FieldAction: Item {
        id: action
        property bool selected: false
        property url iconSource: ""
        property string accessibleText: ""
        property string tip: ""
        signal triggered()

        implicitWidth: appTheme.inspectorHeaderActionSize
        implicitHeight: appTheme.inspectorHeaderActionSize
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: accessibleText
        Accessible.checkable: true
        Accessible.checked: selected
        Accessible.onPressAction: action.triggered()

        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                    || event.key === Qt.Key_Enter) {
                action.triggered()
                event.accepted = true
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: appTheme.badgeRadius
            color: action.selected
                   ? appTheme.editorListSelectedFillColor
                   : (hit.containsMouse ? appTheme.buttonHoveredFillColor : "transparent")
            border.width: action.activeFocus ? 1 : 0
            border.color: appTheme.textMutedColor
        }

        ColorImage {
            anchors.centerIn: parent
            width: appTheme.iconOpticalSizeCompact
            height: appTheme.iconOpticalSizeCompact
            source: action.iconSource
            sourceSize.width: appTheme.iconSourceSizeCompact
            sourceSize.height: appTheme.iconSourceSizeCompact
            fillMode: Image.PreserveAspectFit
            smooth: true
            color: action.selected ? appTheme.editorListSelectedInkColor
                                   : (hit.containsMouse ? appTheme.textColor
                                                        : appTheme.textMutedColor)
        }

        MouseArea {
            id: hit
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: action.triggered()
        }

        ToolTip.visible: hit.containsMouse && action.tip.length > 0
        ToolTip.delay: 500
        ToolTip.text: action.tip
    }

    FieldAction {
        id: ascendingAction
        objectName: "inspectorSortAscending_" + root.field
        iconSource: "qrc:/panel_icons/sort-ascending.svg"
        selected: root.sortedAscending
        accessibleText: qsTr("Sort by %1, %2").arg(root.fieldTitle).arg(root.ascendingTitle)
        tip: root.sortTip(false)
        function activate() { root.library.ToggleInspectorSort(root.field, false) }
        onTriggered: activate()
    }

    FieldAction {
        id: descendingAction
        objectName: "inspectorSortDescending_" + root.field
        iconSource: "qrc:/panel_icons/sort-descending.svg"
        selected: root.sortedDescending
        accessibleText: qsTr("Sort by %1, %2").arg(root.fieldTitle).arg(root.descendingTitle)
        tip: root.sortTip(true)
        function activate() { root.library.ToggleInspectorSort(root.field, true) }
        onTriggered: activate()
    }

    FieldAction {
        objectName: "inspectorGroupButton_" + root.field
        iconSource: "qrc:/panel_icons/category-plus.svg"
        selected: root.grouped
        accessibleText: qsTr("Group photos by %1").arg(root.fieldTitle)
        tip: root.grouped ? qsTr("Stop grouping photos by %1.").arg(root.fieldTitle)
                          : qsTr("Group photos by %1").arg(root.fieldTitle)
        function activate() { root.library.SetInspectorGrouping(root.field, !root.grouped) }
        onTriggered: activate()
    }
}
