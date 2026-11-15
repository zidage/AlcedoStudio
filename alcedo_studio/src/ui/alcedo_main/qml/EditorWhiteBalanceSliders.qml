import QtQuick
import QtQuick.Layouts
import Alcedo.Main 1.0

// Temperature + Tint slider pair shared by RAW Decode White Balance and the
// Look page Color Grade white balance. The temperature rail uses the RAW
// non-linear Kelvin position scale (6000 K at the midpoint) and shows and takes
// Kelvin in its value field; tint is linear. The rail ranges come from the
// editor parameter catalog entry of fieldKey. The owner model applies edits and
// supplies values through EditorDragSlider: a drag is begin -> update... ->
// finish, and a typed value or an arrow key step is one begin -> update -> finish.
ColumnLayout {
    id: root

    property var flickable: null
    property bool slidersEnabled: true
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property string temperatureObjectName: ""
    property string tintObjectName: ""
    /// Catalog field and its Kelvin property: color_temp/kelvin or grade_white_balance/temperature.
    property string fieldKey: "color_temp"
    property string temperatureProperty: "kelvin"
    readonly property var temperatureSpec: parameterCatalog.propertySpec(root.fieldKey,
                                                                         root.temperatureProperty)
    readonly property var temperatureSlider: temperatureSpec.ui_slider
    readonly property var tintSpec: parameterCatalog.propertySpec(root.fieldKey, "tint")
    property real temperature: 6000
    property int temperatureSliderPos: 2048
    property real tint: 0
    /// Values that a double-click reset returns to; the rails mark them.
    property real temperatureDefault: 6000
    property real tintDefault: 0

    signal temperatureDragBegin()
    signal temperatureDragUpdate(int pos)
    signal temperatureDragFinish()
    signal temperatureReset()
    signal tintDragBegin()
    signal tintDragUpdate(real value)
    signal tintDragFinish()
    signal tintReset()

    spacing: appTheme.spaceSm

    EditorParameterCatalog {
        id: parameterCatalog
    }

    EditorDragSlider {
        id: temperatureRow
        objectName: root.temperatureObjectName
        partName: "whiteBalanceTemperature"
        label: qsTr("Temperature")
        enabled: root.slidersEnabled
        flickable: root.flickable
        from: root.temperatureSlider.position_min
        to: root.temperatureSlider.position_max
        stepSize: 1
        precision: 0
        origin: from
        value: root.temperatureSliderPos
        defaultValue: parameterCatalog.kelvinToSliderPosition(root.temperatureDefault)
        valueFormatter: function (v) { return Math.round(root.temperature) + " K" }
        editFormatter: function (v) { return String(Math.round(root.temperature)) }
        editParser: function (text) {
            var s = text.replace(/k/gi, "").trim()
            var kelvin = Number(s)
            if (s.length === 0 || !isFinite(kelvin))
                return NaN
            kelvin = Math.max(root.temperatureSpec.ui_min,
                              Math.min(root.temperatureSpec.ui_max, kelvin))
            return parameterCatalog.kelvinToSliderPosition(kelvin)
        }
        railGradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#9BD8FF" }
            GradientStop { position: 0.5; color: "#FFE8B0" }
            GradientStop { position: 1.0; color: "#FF8A3D" }
        }
        beginEdit: function () { root.temperatureDragBegin() }
        updateEdit: function (v) { root.temperatureDragUpdate(Math.round(v)) }
        finishEdit: function () { root.temperatureDragFinish() }
        resetEdit: function () { root.temperatureReset() }
    }

    EditorDragSlider {
        id: tintRow
        objectName: root.tintObjectName
        partName: "whiteBalanceTint"
        label: qsTr("Tint")
        enabled: root.slidersEnabled
        flickable: root.flickable
        from: root.tintSpec.ui_min
        to: root.tintSpec.ui_max
        stepSize: root.tintSpec.ui_step > 0 ? root.tintSpec.ui_step : 1
        precision: root.tintSpec.ui_decimals !== undefined ? root.tintSpec.ui_decimals : 0
        value: root.tint
        defaultValue: root.tintDefault
        railGradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#49C26D" }
            GradientStop { position: 0.5; color: "#E6E6E6" }
            GradientStop { position: 1.0; color: "#A85AE6" }
        }
        beginEdit: function () { root.tintDragBegin() }
        updateEdit: function (v) { root.tintDragUpdate(v) }
        finishEdit: function () { root.tintDragFinish() }
        resetEdit: function () { root.tintReset() }
    }
}
