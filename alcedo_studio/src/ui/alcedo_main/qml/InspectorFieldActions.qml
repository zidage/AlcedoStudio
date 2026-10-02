import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Album Inspector field header actions: ascending and descending photo-sort actions and the
// Group checkbox of one field. The arrows mean ORDER BY; the checkbox means GROUP BY. Both
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

    component SortAction: Item {
        id: action
        property bool descending: false
        property bool selected: false
        property string glyph: ""
        property string accessibleText: ""
        property string tip: ""

        implicitWidth: appTheme.inspectorHeaderActionSize
        implicitHeight: appTheme.inspectorHeaderActionSize
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: accessibleText
        Accessible.checkable: true
        Accessible.checked: selected
        Accessible.onPressAction: activate()

        function activate() {
            root.library.ToggleInspectorSort(root.field, action.descending)
        }

        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                    || event.key === Qt.Key_Enter) {
                action.activate()
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

        Text {
            anchors.centerIn: parent
            text: action.glyph
            color: action.selected ? appTheme.editorListSelectedInkColor
                                   : (hit.containsMouse ? appTheme.textColor
                                                        : appTheme.textMutedColor)
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
            font.weight: appTheme.fontWeightStrong
        }

        MouseArea {
            id: hit
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: action.activate()
        }

        ToolTip.visible: hit.containsMouse && action.tip.length > 0
        ToolTip.delay: 500
        ToolTip.text: action.tip
    }

    SortAction {
        objectName: "inspectorSortAscending_" + root.field
        glyph: "↑"
        descending: false
        selected: root.sortedAscending
        accessibleText: qsTr("Sort by %1, %2").arg(root.fieldTitle).arg(root.ascendingTitle)
        tip: root.sortTip(false)
    }

    SortAction {
        objectName: "inspectorSortDescending_" + root.field
        glyph: "↓"
        descending: true
        selected: root.sortedDescending
        accessibleText: qsTr("Sort by %1, %2").arg(root.fieldTitle).arg(root.descendingTitle)
        tip: root.sortTip(true)
    }

    ThemeCheckBox {
        objectName: "inspectorGroupCheckBox_" + root.field
        Layout.fillWidth: false
        text: qsTr("Group")
        accessibleText: qsTr("Group photos by %1").arg(root.fieldTitle)
        checked: root.grouped
        alwaysPrimaryText: false
        onToggled: function(checked) {
            root.library.SetInspectorGrouping(root.field, checked)
        }
    }
}
