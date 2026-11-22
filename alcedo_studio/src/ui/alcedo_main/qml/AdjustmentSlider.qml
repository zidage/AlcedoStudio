import QtQuick
import QtQuick.Layouts

// AlcedoSlider (#922) bound to an EditorAdjustmentValueModel. The model owns the value and the
// submits:
//   - Rail drag and value drag: beginDrag -> updateDrag (per move) -> finishDrag, one settled
//     transaction per completed drag.
//   - Arrow keys on the rail: editValue (interactive + debounced settled).
//   - Typed value (Enter, focus-out, Up/Down): editValue + commitImmediately.
//   - Text that is not a number: setInvalid, no submit.
//   - Double-click on the label or the rail: reset (one settled transaction).
// The rail covers [minimum, maximum]; [railMinimum, railMaximum] of the model are its soft
// limits (hatched extra zones outside, a rail drag holds at each limit).
// Optional `flickable`: locked while a drag runs so parent scroll areas cannot steal it.
AlcedoSlider {
    id: root
    objectName: "adjustmentSlider"
    partName: "adjustmentSlider"

    property var model: null

    layout: "stacked"
    infoObjectName: "adjustmentSliderInfoHint"
    Layout.fillWidth: true
    enabled: root.model ? root.model.enabled : false

    label: root.model ? root.model.label : ""
    value: root.model ? root.model.value : 0
    from: root.model ? root.model.minimum : 0
    to: root.model ? root.model.maximum : 1
    softFrom: root.model ? root.model.railMinimum : from
    softTo: root.model ? root.model.railMaximum : to
    stepSize: root.model && root.model.step > 0 ? root.model.step : 0.01
    precision: root.model ? root.model.precision : 2
    defaultValue: root.model ? root.model.defaultValue : 0
    suffix: root.model && root.model.suffix ? root.model.suffix : ""
    valueValid: root.model ? root.model.valid : true

    onDragStarted: {
        if (root.model)
            root.model.beginDrag()
    }
    onValueEdited: function (v) {
        if (!root.model)
            return
        if (root.dragging)
            root.model.updateDrag(v)
        else
            root.model.editValue(v)
    }
    onDragFinished: {
        if (root.model)
            root.model.finishDrag()
    }
    onValueEntered: function (v) {
        if (!root.model)
            return
        root.model.editValue(v)
        root.model.commitImmediately()
    }
    onTextRejected: function (text) {
        if (root.model)
            root.model.setInvalid(qsTr("Invalid number"))
    }
    onResetRequested: {
        if (root.model && root.model.enabled)
            root.model.reset()
    }
}
