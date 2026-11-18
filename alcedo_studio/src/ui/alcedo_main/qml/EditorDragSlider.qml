import QtQuick
import QtQuick.Layouts

// AlcedoSlider (#922) for an owner that takes each edit as a drag: beginEdit -> updateEdit... ->
// finishEdit, one committed transaction per completed drag. A typed value and an arrow key step
// are one beginEdit -> updateEdit -> finishEdit. A double-click on the label or the rail calls
// resetEdit, or applies `defaultValue` as one edit when resetEdit is unset.
AlcedoSlider {
    id: root

    property var beginEdit: function () {}
    property var updateEdit: function (v) {}
    property var finishEdit: function () {}
    property var resetEdit: null

    layout: "stacked"
    Layout.fillWidth: true

    function applyEdit(v) {
        root.beginEdit()
        root.updateEdit(v)
        root.finishEdit()
    }

    onDragStarted: root.beginEdit()
    onValueEdited: function (v) {
        if (root.dragging)
            root.updateEdit(v)
        else
            root.applyEdit(v)
    }
    onDragFinished: root.finishEdit()
    onValueEntered: function (v) { root.applyEdit(v) }
    onResetRequested: {
        if (typeof root.resetEdit === "function")
            root.resetEdit()
        else
            root.applyEdit(root.defaultValue)
    }
}
