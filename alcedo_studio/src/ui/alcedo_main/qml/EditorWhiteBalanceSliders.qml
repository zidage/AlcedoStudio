import QtQuick
import QtQuick.Layouts

// Temperature + Tint slider pair shared by RAW Decode White Balance and the
// Look page Color Grade white balance. The temperature track uses the RAW
// non-linear Kelvin position scale (0..4096, 6000 K at the midpoint); tint is
// linear on -150..150. The owner model applies edits and supplies values.
ColumnLayout {
    id: root

    property var flickable: null
    property bool slidersEnabled: true
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property string temperatureObjectName: ""
    property string tintObjectName: ""
    property real temperature: 6000
    property int temperatureSliderPos: 2048
    property real tint: 0

    signal temperatureDragBegin()
    signal temperatureDragUpdate(int pos)
    signal temperatureDragFinish()
    signal temperatureReset()
    signal tintDragBegin()
    signal tintDragUpdate(real value)
    signal tintDragFinish()
    signal tintReset()

    spacing: appTheme.spaceSm

    SliderRow {
        objectName: root.temperatureObjectName
        title: qsTr("Temperature")
        valueText: Math.round(root.temperature) + " K"
        from: 0
        to: 4096
        value: root.temperatureSliderPos
        gradientStops: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#9BD8FF" }
            GradientStop { position: 0.5; color: "#FFE8B0" }
            GradientStop { position: 1.0; color: "#FF8A3D" }
        }
        onBegin: function () { root.temperatureDragBegin() }
        onUpdate: function (v) { root.temperatureDragUpdate(Math.round(v)) }
        onFinish: function () { root.temperatureDragFinish() }
        onReset: function () { root.temperatureReset() }
    }

    SliderRow {
        objectName: root.tintObjectName
        title: qsTr("Tint")
        valueText: String(Math.round(root.tint))
        from: -150
        to: 150
        value: root.tint
        gradientStops: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#49C26D" }
            GradientStop { position: 0.5; color: "#E6E6E6" }
            GradientStop { position: 1.0; color: "#A85AE6" }
        }
        onBegin: function () { root.tintDragBegin() }
        onUpdate: function (v) { root.tintDragUpdate(v) }
        onFinish: function () { root.tintDragFinish() }
        onReset: function () { root.tintReset() }
    }

    component SliderRow: ColumnLayout {
        id: row
        property string title: ""
        property string valueText: ""
        property real from: 0
        property real to: 100
        property real value: 0
        property var gradientStops: null
        property var onBegin: function () {}
        property var onUpdate: function (v) {}
        property var onFinish: function () {}
        property var onReset: function () {}

        Layout.fillWidth: true
        spacing: 2

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(appTheme.lineHeightCaption,
                                             wbRowTitle.implicitHeight)
            spacing: appTheme.spaceSm
            Text {
                id: wbRowTitle
                Layout.fillWidth: true
                text: row.title
                color: root.textColor
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
            }
            Text {
                text: row.valueText
                color: root.mutedColor
                font.pixelSize: appTheme.fontSizeCaption
                font.family: appTheme.dataFontFamily
            }
        }
        EditorMonoSlider {
            from: row.from
            to: row.to
            externalValue: row.value
            enabled: root.slidersEnabled
            gradientStops: row.gradientStops
            flickable: root.flickable
            onBegin: function () { row.onBegin() }
            onUpdate: function (v) { row.onUpdate(v) }
            onFinish: function () { row.onFinish() }
            onReset: function () { row.onReset() }
        }
    }
}
