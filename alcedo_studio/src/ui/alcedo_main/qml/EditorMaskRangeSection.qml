import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Range section of the Mask panel (Mask ranges MR4, #564): the same for every Mask kind and fixed
// above the Mask-specific controls. It holds the Luminance Range and Hue Range bars with their
// value fields, a Softness slider for each range, the saturation threshold of the Hue Range, and
// "Show selection". Every value and action is EditorMaskCreationAdapter's: the bars show the
// range target Mask, a drag or a burst of steps shows the range coverage of that Mask until it
// settles, and "Show selection" keeps it shown. During the edit of a new AI Mask the target is its
// candidate (#604): the values are written with the new Mask at Confirm. The Mask panel puts it in
// a collapsible section.
ColumnLayout {
    id: root

    property var maskCreation: null
    property bool controlsEnabled: true

    readonly property bool available: root.controlsEnabled && !!root.maskCreation
                                      && root.maskCreation.maskRangeAvailable
    readonly property var limits: root.maskCreation ? root.maskCreation.rangeParameterLimits : ({})
    readonly property bool luminanceRestricted: !!root.maskCreation
                                                && (root.maskCreation.maskLuminanceLow > 0
                                                    || root.maskCreation.maskLuminanceHigh < 1)
    readonly property bool hueRestricted: !!root.maskCreation
                                          && (root.maskCreation.maskHueStart > 0
                                              || root.maskCreation.maskHueEnd < 360)

    spacing: appTheme.spaceXs

    component SectionLabel: Label {
        Layout.fillWidth: true
        color: appTheme.textColor
        font.pixelSize: appTheme.fontSizeCaption
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    // One end of a range as a typed value: model value × scale, one decimal. Enter or focus-out
    // writes it as one commit; the adapter clamps it to the minimum handle distance.
    component RangeValueField: TextField {
        id: field
        property string kind: ""
        property string part: ""
        property real modelValue: 0
        property real scale: 1
        property real minimumUi: 0
        property real maximumUi: 100
        readonly property string shownText: (modelValue * scale).toFixed(1)

        Layout.preferredWidth: 48
        Layout.preferredHeight: 20
        leftPadding: 2
        rightPadding: 2
        topPadding: 0
        bottomPadding: 0
        enabled: root.available
        color: appTheme.textMutedColor
        font.family: appTheme.dataFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        horizontalAlignment: TextInput.AlignRight
        verticalAlignment: TextInput.AlignVCenter
        selectByMouse: true
        activeFocusOnTab: true
        validator: DoubleValidator {
            bottom: field.minimumUi
            top: field.maximumUi
            decimals: 1
            notation: DoubleValidator.StandardNotation
        }
        Accessible.role: Accessible.EditableText
        background: Rectangle {
            color: "transparent"
            radius: appTheme.controlRadiusSmall
            border.width: field.activeFocus ? 1 : 0
            border.color: appTheme.textMutedColor
        }
        text: shownText
        onShownTextChanged: {
            if (!activeFocus)
                text = shownText
        }
        onActiveFocusChanged: {
            if (activeFocus)
                selectAll()
            else
                text = shownText
        }
        onEditingFinished: {
            var n = parseFloat(text)
            if (root.maskCreation && !isNaN(n) && isFinite(n)) {
                if (root.maskCreation.editRangeValue(kind, part, n / scale))
                    root.maskCreation.commitRangeEdits()
            }
            text = shownText
        }
    }

    // AdjustmentSlider model of a Softness or threshold value of one range. The slider shows
    // model value × scale; a drag is one range edit, keys and typed values settle as one commit.
    component RangeParameterModel: QtObject {
        property string kind: ""
        property string field: ""
        property string label: ""
        property real scale: 1
        property real sourceValue: 0
        property real minimum: 0
        property real maximum: 1
        property real defaultValue: 0
        property real step: 0.1
        property int precision: 1
        property string suffix: ""
        property bool enabled: false
        property bool valid: true
        readonly property real value: sourceValue * scale

        function beginDrag() {
            valid = true
            if (root.maskCreation)
                root.maskCreation.beginRangeEdit(kind)
        }
        function updateDrag(v) {
            if (root.maskCreation)
                root.maskCreation.editRangeValue(kind, field, v / scale)
        }
        function finishDrag() {
            if (root.maskCreation)
                root.maskCreation.finishRangeEdit(kind)
        }
        function editValue(v) {
            valid = true
            if (root.maskCreation)
                root.maskCreation.editRangeValue(kind, field, v / scale)
        }
        function commitImmediately() {
            if (root.maskCreation)
                root.maskCreation.commitRangeEdits()
        }
        function reset() {
            editValue(defaultValue)
            commitImmediately()
        }
        function setInvalid(message) {
            void message
            valid = false
        }
    }

    RangeParameterModel {
        id: luminanceSoftnessModel
        kind: "luminance"
        field: "softness"
        label: qsTr("Softness")
        scale: 100
        sourceValue: root.maskCreation ? root.maskCreation.maskLuminanceSoftness : 0.05
        minimum: (root.limits.luminanceSoftnessMinimum || 0.005) * 100
        maximum: (root.limits.luminanceSoftnessMaximum || 0.5) * 100
        defaultValue: (root.limits.luminanceSoftnessDefault || 0.05) * 100
        step: 0.5
        enabled: root.available && root.luminanceRestricted
    }
    RangeParameterModel {
        id: hueSoftnessModel
        kind: "hue"
        field: "softness"
        label: qsTr("Softness")
        sourceValue: root.maskCreation ? root.maskCreation.maskHueSoftness : 12
        minimum: root.limits.hueSoftnessMinimum || 0.5
        maximum: root.limits.hueSoftnessMaximum || 90
        defaultValue: root.limits.hueSoftnessDefault || 12
        step: 0.5
        suffix: "°"
        enabled: root.available && root.hueRestricted
    }
    RangeParameterModel {
        id: minChromaModel
        kind: "hue"
        field: "minChroma"
        label: qsTr("Saturation Threshold")
        scale: 100
        sourceValue: root.maskCreation ? root.maskCreation.maskHueMinChroma : 0.03
        minimum: (root.limits.minChromaMinimum || 0.015) * 100
        maximum: (root.limits.minChromaMaximum || 0.2) * 100
        defaultValue: (root.limits.minChromaDefault || 0.03) * 100
        step: 0.1
        enabled: root.available && root.hueRestricted
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 2
        SectionLabel { text: qsTr("Luminance Range") }
        RangeValueField {
            objectName: "editorMaskLuminanceLowField"
            kind: "luminance"
            part: "first"
            scale: 100
            modelValue: root.maskCreation ? root.maskCreation.maskLuminanceLow : 0
            Accessible.name: qsTr("Luminance low")
        }
        Label { text: "–"; color: appTheme.textMutedColor }
        RangeValueField {
            objectName: "editorMaskLuminanceHighField"
            kind: "luminance"
            part: "second"
            scale: 100
            modelValue: root.maskCreation ? root.maskCreation.maskLuminanceHigh : 1
            Accessible.name: qsTr("Luminance high")
        }
    }
    RangeSelectorBar {
        objectName: "editorMaskLuminanceRange"
        Layout.fillWidth: true
        enabled: root.available
        accessibleName: qsTr("Mask luminance range")
        from: 0
        to: 1
        first: root.maskCreation ? root.maskCreation.maskLuminanceLow : 0
        second: root.maskCreation ? root.maskCreation.maskLuminanceHigh : 1
        minimumWidth: root.maskCreation ? root.maskCreation.luminanceRangeMinimumWidth : 0.012
        gradientStops: root.maskCreation ? root.maskCreation.luminanceGradientStops : []
        softness: root.maskCreation ? root.maskCreation.maskLuminanceSoftness : 0
        onBegan: root.maskCreation.beginRangeEdit("luminance")
        onMoved: function (low, high) { root.maskCreation.updateRangeEdit("luminance", low, high) }
        onFinished: root.maskCreation.finishRangeEdit("luminance")
        onStepped: function (part, steps, coarse) {
            root.maskCreation.stepRangeHandle("luminance", part, steps, coarse)
        }
        onResetRequested: root.maskCreation.resetRange("luminance")
    }
    AdjustmentSlider {
        objectName: "editorMaskLuminanceSoftness"
        Layout.fillWidth: true
        model: luminanceSoftnessModel
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: appTheme.spaceXs
        spacing: 2
        SectionLabel { text: qsTr("Hue Range") }
        RangeValueField {
            objectName: "editorMaskHueStartField"
            kind: "hue"
            part: "first"
            modelValue: root.maskCreation ? root.maskCreation.maskHueStart : 0
            maximumUi: 360
            Accessible.name: qsTr("Hue start")
        }
        Label { text: "–"; color: appTheme.textMutedColor }
        RangeValueField {
            objectName: "editorMaskHueEndField"
            kind: "hue"
            part: "second"
            modelValue: root.maskCreation ? root.maskCreation.maskHueEnd : 360
            maximumUi: 360
            Accessible.name: qsTr("Hue end")
        }
    }
    RangeSelectorBar {
        objectName: "editorMaskHueRange"
        Layout.fillWidth: true
        enabled: root.available
        accessibleName: qsTr("Mask hue range")
        wraps: true
        from: 0
        to: 360
        first: root.maskCreation ? root.maskCreation.maskHueStart : 0
        second: root.maskCreation ? root.maskCreation.maskHueEnd : 360
        minimumWidth: root.maskCreation ? root.maskCreation.hueRangeMinimumWidth : 1.2
        gradientStops: root.maskCreation ? root.maskCreation.hueGradientStops : []
        softness: root.maskCreation ? root.maskCreation.maskHueSoftness : 0
        onBegan: root.maskCreation.beginRangeEdit("hue")
        onMoved: function (start, end) { root.maskCreation.updateRangeEdit("hue", start, end) }
        onFinished: root.maskCreation.finishRangeEdit("hue")
        onStepped: function (part, steps, coarse) {
            root.maskCreation.stepRangeHandle("hue", part, steps, coarse)
        }
        onResetRequested: root.maskCreation.resetRange("hue")
    }
    AdjustmentSlider {
        objectName: "editorMaskHueSoftness"
        Layout.fillWidth: true
        model: hueSoftnessModel
    }
    AdjustmentSlider {
        objectName: "editorMaskHueMinChroma"
        Layout.fillWidth: true
        model: minChromaModel
    }

    ThemeCheckBox {
        objectName: "editorMaskRangeShowSelection"
        Layout.fillWidth: true
        enabled: root.available
        text: qsTr("Show selection")
        checked: root.maskCreation ? root.maskCreation.maskRangeCoverageVisible : false
        onToggled: function (checked) {
            if (root.maskCreation)
                root.maskCreation.setRangeCoverageVisible(checked)
        }
    }
}
