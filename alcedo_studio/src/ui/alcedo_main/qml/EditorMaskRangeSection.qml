import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Luminance Range and Hue Range of one Mask (Mask ranges MR4), at the top of the Mask panel and
// of the AI Mask panel. Every value and action is EditorMaskCreationAdapter's: the bars show the
// range target Mask, a handle drag shows the range coverage of that Mask until release, and
// "Show selection" keeps it shown. A new AI Mask candidate is not a Mask yet, so the bars are
// disabled for it (D11).
ColumnLayout {
    id: root

    property var maskCreation: null
    property bool controlsEnabled: true
    property var flickable: null
    // The AI Mask panel edits a new candidate: show why the bars are disabled.
    property bool newAiMaskCandidate: false

    readonly property bool available: root.controlsEnabled && !!root.maskCreation
                                      && root.maskCreation.maskRangeAvailable

    spacing: appTheme.spaceXs

    component SectionLabel: Label {
        Layout.fillWidth: true
        color: appTheme.textColor
        font.pixelSize: appTheme.fontSizeCaption
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    SectionLabel { text: qsTr("Luminance Range") }
    RangeSelectorBar {
        objectName: "editorMaskLuminanceRange"
        Layout.fillWidth: true
        enabled: root.available
        accessibleName: qsTr("Mask luminance range")
        flickable: root.flickable
        from: 0
        to: 1
        first: root.maskCreation ? root.maskCreation.maskLuminanceLow : 0
        second: root.maskCreation ? root.maskCreation.maskLuminanceHigh : 1
        minimumWidth: root.maskCreation ? root.maskCreation.luminanceRangeMinimumWidth : 0.012
        gradientStops: root.maskCreation ? root.maskCreation.luminanceGradientStops : []
        onBegan: root.maskCreation.beginRangeEdit("luminance")
        onMoved: function (low, high) { root.maskCreation.updateRangeEdit("luminance", low, high) }
        onFinished: root.maskCreation.finishRangeEdit("luminance")
        softness: root.maskCreation ? root.maskCreation.maskLuminanceSoftness : 0
        onStepped: function (part, steps, coarse) {
            root.maskCreation.stepRangeHandle("luminance", part, steps, coarse)
        }
        onResetRequested: root.maskCreation.resetRange("luminance")
    }

    SectionLabel { text: qsTr("Hue Range") }
    RangeSelectorBar {
        objectName: "editorMaskHueRange"
        Layout.fillWidth: true
        enabled: root.available
        accessibleName: qsTr("Mask hue range")
        flickable: root.flickable
        wraps: true
        from: 0
        to: 360
        first: root.maskCreation ? root.maskCreation.maskHueStart : 0
        second: root.maskCreation ? root.maskCreation.maskHueEnd : 360
        minimumWidth: root.maskCreation ? root.maskCreation.hueRangeMinimumWidth : 1.2
        gradientStops: root.maskCreation ? root.maskCreation.hueGradientStops : []
        onBegan: root.maskCreation.beginRangeEdit("hue")
        onMoved: function (start, end) { root.maskCreation.updateRangeEdit("hue", start, end) }
        onFinished: root.maskCreation.finishRangeEdit("hue")
        softness: root.maskCreation ? root.maskCreation.maskHueSoftness : 0
        onStepped: function (part, steps, coarse) {
            root.maskCreation.stepRangeHandle("hue", part, steps, coarse)
        }
        onResetRequested: root.maskCreation.resetRange("hue")
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

    Label {
        objectName: "editorMaskRangeCandidateNotice"
        Layout.fillWidth: true
        visible: root.newAiMaskCandidate
        text: qsTr("Confirm the new AI Mask to set its ranges.")
        color: appTheme.textMutedColor
        wrapMode: Text.Wrap
        font.pixelSize: appTheme.fontSizeCaption
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
}
