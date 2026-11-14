import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Alcedo.Main 1.0

// RAW Import section of Settings > Default Behavior (#920): the look that new RAW imports write
// into the history root of each image. The rows edit the pending values of
// RawImportLookSettingsModel; the Done button of the dialog writes them (model.apply()). Each row
// has a reset that shows while its value differs from the app default.
ColumnLayout {
    id: section
    objectName: "rawImportLookSection"

    // RawImportLookSettingsModel (appModules.rawImportLook).
    property var model: null
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color dividerColor: appTheme.dividerColor
    property color dangerColor: appTheme.dangerColor

    readonly property int labelWidth: 160
    readonly property bool openDrtSelected: model ? model.drtMethod === "open_drt" : true
    readonly property real axisGrey: model ? model.axisGrey : 0.4135
    readonly property real axisStop: model ? model.axisStop : 0.0571
    // Room under the plot for the EV labels, and at each side for half of the widest label.
    readonly property int labelInset: appTheme.fontSizeMicro + appTheme.spaceSm
    readonly property int labelSideInset: Math.ceil(axisLabelMetrics.advanceWidth / 2)

    spacing: 14

    TextMetrics {
        id: axisLabelMetrics
        font.family: appTheme.dataFontFamily
        font.pixelSize: appTheme.fontSizeMicro
        text: "+10"
    }

    function indexOfValue(entries, value) {
        for (let i = 0; i < entries.length; ++i) {
            if (entries[i].value === value)
                return i
        }
        return -1
    }

    // Grid lines every 2 EV from 18% grey inside [0, 1], as in the Tone Curve section.
    function evTicks(withLabels) {
        var ticks = []
        for (var ev = -8; ev <= 10; ev += 2) {
            const value = section.axisGrey + ev * section.axisStop
            if (value < 0 || value > 1)
                continue
            var label = ""
            if (withLabels)
                label = ev === 0 ? "0" : (ev > 0 ? "+" + ev : "−" + Math.abs(ev))
            ticks.push({ value: value, label: label, strong: ev === 0 })
        }
        return ticks
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("RAW Import")
        color: section.textColor
        font.pixelSize: appTheme.fontSizeSection
        font.weight: appTheme.fontWeightStrong
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: section.dividerColor
    }

    // Adapters: the shared controls read their value and limits from a model object.
    QtObject {
        id: exposureControl
        property string label: ""
        property bool enabled: section.model !== null
        property bool valid: true
        readonly property real value: section.model ? section.model.exposureEv : 0
        // A value that the model accepted or read again clears the error of the field.
        onValueChanged: valid = true
        readonly property real minimum: -10
        readonly property real maximum: 10
        readonly property real step: 0.01
        readonly property int precision: 2
        readonly property string suffix: ""
        function beginDrag() {}
        function updateDrag(v) { editValue(v) }
        function finishDrag() {}
        function editValue(v) {
            if (section.model)
                valid = section.model.setExposureEv(v)
        }
        function commitImmediately() {}
        function setInvalid(message) { valid = false }
        function reset() {
            valid = true
            if (section.model)
                section.model.resetRow("exposureEv")
        }
    }

    QtObject {
        id: saturationControl
        property string label: ""
        property bool enabled: section.model !== null
        property bool valid: true
        readonly property real value: section.model ? section.model.saturation : 0
        // A value that the model accepted or read again clears the error of the field.
        onValueChanged: valid = true
        readonly property real minimum: -100
        readonly property real maximum: 100
        readonly property real step: 1
        readonly property int precision: 0
        readonly property string suffix: ""
        function beginDrag() {}
        function updateDrag(v) { editValue(v) }
        function finishDrag() {}
        function editValue(v) {
            if (section.model)
                valid = section.model.setSaturation(v)
        }
        function commitImmediately() {}
        function setInvalid(message) { valid = false }
        function reset() {
            valid = true
            if (section.model)
                section.model.resetRow("saturation")
        }
    }

    QtObject {
        id: toneCurveControl
        property string label: ""
        property bool enabled: section.model !== null
        readonly property var entries: section.model ? section.model.toneCurvePresets : []
        readonly property int currentIndex: section.model
                                            ? section.indexOfValue(entries, section.model.toneCurvePreset)
                                            : -1
        function selectIndex(index) {
            if (section.model && index >= 0 && index < entries.length)
                section.model.setToneCurvePreset(entries[index].value)
        }
    }

    QtObject {
        id: drtMethodControl
        property string label: ""
        property bool enabled: section.model !== null
        readonly property var entries: section.model ? section.model.drtMethods : []
        readonly property int currentIndex: section.model
                                            ? section.indexOfValue(entries, section.model.drtMethod)
                                            : -1
        function selectIndex(index) {
            if (section.model && index >= 0 && index < entries.length)
                section.model.setDrtMethod(entries[index].value)
        }
    }

    QtObject {
        id: openDrtLookControl
        property string label: ""
        property bool enabled: section.model !== null
        readonly property var entries: section.model ? section.model.openDrtLookPresets : []
        readonly property int currentIndex: section.model
                                            ? section.indexOfValue(entries, section.model.openDrtLookPreset)
                                            : -1
        function selectIndex(index) {
            if (section.model && index >= 0 && index < entries.length)
                section.model.setOpenDrtLookPreset(entries[index].value)
        }
    }

    QtObject {
        id: lensCorrectionControl
        property string label: ""
        property bool enabled: section.model !== null
        readonly property bool value: section.model ? section.model.lensCorrection : false
        function commitValue(checked) {
            if (section.model)
                section.model.setLensCorrection(checked)
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12

        LookRow {
            objectName: "rawImportExposureRow"
            label: qsTr("Exposure")
            labelWidth: section.labelWidth
            textColor: section.textColor
            onResetClicked: section.model.resetRow("exposureEv")
            modified: section.model ? section.model.exposureEvModified : false

            AdjustmentSlider {
                objectName: "rawImportExposureSlider"
                Layout.fillWidth: true
                model: exposureControl
            }
        }

        LookRow {
            objectName: "rawImportSaturationRow"
            label: qsTr("Saturation")
            labelWidth: section.labelWidth
            textColor: section.textColor
            onResetClicked: section.model.resetRow("saturation")
            modified: section.model ? section.model.saturationModified : false

            AdjustmentSlider {
                objectName: "rawImportSaturationSlider"
                Layout.fillWidth: true
                model: saturationControl
            }
        }

        LookRow {
            objectName: "rawImportToneCurveRow"
            label: qsTr("Tone Curve")
            labelWidth: section.labelWidth
            textColor: section.textColor
            onResetClicked: section.model.resetRow("toneCurvePreset")
            modified: section.model ? section.model.toneCurvePresetModified : false

            AdjustmentCombo {
                controlObjectName: "rawImportToneCurveCombo"
                Layout.fillWidth: true
                controlHeight: 36
                showResetButton: false
                groupLabelRole: "group"
                model: toneCurveControl
            }
        }

        // Display only: the RGB curve of the selected preset on the EV grid of the Tone Curve
        // section. The pane is disabled, so it takes no pointer, wheel, or key input.
        Item {
            id: previewFrame
            objectName: "rawImportToneCurvePreview"
            readonly property int plotSize: 220
            Layout.leftMargin: section.labelWidth + 16
            Layout.preferredWidth: plotSize + 2 * section.labelSideInset
            Layout.preferredHeight: plotSize + section.labelInset

            PlotPane {
                id: previewPane
                objectName: "rawImportToneCurvePlotPane"
                anchors.fill: parent
                enabled: false
                limits: Qt.rect(0, 0, 1, 1)
                maximumZoom: 1
                insetLeft: section.labelSideInset
                insetRight: section.labelSideInset
                insetBottom: section.labelInset
                backgroundColor: appTheme.cardSurfaceColor
                surfaceColor: appTheme.plotSurfaceColor

                PlotGridLayer {
                    xTicks: section.evTicks(true)
                    yTicks: section.evTicks(false)
                    color: appTheme.plotGridColor
                    strongColor: appTheme.plotGridStrongColor
                    labelColor: appTheme.plotLabelColor
                    labelFont.family: appTheme.dataFontFamily
                    labelFont.pixelSize: appTheme.fontSizeMicro
                    lineWidth: appTheme.plotGridLineWidth
                }
                PlotPolylineLayer {
                    objectName: "rawImportToneCurveLine"
                    points: section.model ? section.model.toneCurveLine : []
                    color: appTheme.plotLineColor
                    width: appTheme.plotLineWidth
                }
            }
        }

        LookRow {
            objectName: "rawImportDrtMethodRow"
            label: qsTr("Display Transform")
            labelWidth: section.labelWidth
            textColor: section.textColor
            onResetClicked: section.model.resetRow("drtMethod")
            modified: section.model ? section.model.drtMethodModified : false

            AdjustmentCombo {
                controlObjectName: "rawImportDrtMethodCombo"
                Layout.fillWidth: true
                controlHeight: 36
                showResetButton: false
                model: drtMethodControl
            }
        }

        // The value stays while ACES 2.0 is selected; the import stores it for both methods.
        LookRow {
            objectName: "rawImportOpenDrtLookRow"
            visible: section.openDrtSelected
            label: qsTr("OpenDRT Look")
            labelWidth: section.labelWidth
            textColor: section.textColor
            onResetClicked: section.model.resetRow("openDrtLookPreset")
            modified: section.model ? section.model.openDrtLookPresetModified : false

            AdjustmentCombo {
                controlObjectName: "rawImportOpenDrtLookCombo"
                Layout.fillWidth: true
                controlHeight: 36
                showResetButton: false
                model: openDrtLookControl
            }
        }

        LookRow {
            objectName: "rawImportLensCorrectionRow"
            label: qsTr("Lens Correction")
            labelWidth: section.labelWidth
            textColor: section.textColor
            onResetClicked: section.model.resetRow("lensCorrection")
            modified: section.model ? section.model.lensCorrectionModified : false

            AdjustmentToggle {
                objectName: "rawImportLensCorrectionToggle"
                Layout.fillWidth: true
                model: lensCorrectionControl
            }
        }

        Label {
            objectName: "rawImportLookError"
            Layout.fillWidth: true
            Layout.leftMargin: section.labelWidth + 16
            visible: text.length > 0
            text: section.model ? section.model.errorMessage : ""
            color: section.dangerColor
            font.pixelSize: appTheme.fontSizeBody
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: section.labelWidth + 16
            Layout.topMargin: 4

            DialogActionButton {
                objectName: "rawImportRestoreDefaultsButton"
                kind: "normal"
                buttonWidth: Math.max(140, implicitContentWidth + 36)
                buttonHeight: 32
                text: qsTr("Restore Defaults")
                enabled: section.model ? section.model.anyModified : false
                onClicked: section.model.resetAll()
            }

            Item {
                Layout.fillWidth: true
            }
        }
    }

    // One row: the name, the control (the default property), and the reset of the row in a slot
    // of fixed width, so the control keeps its width when the reset shows. The use site binds the
    // label width, the colors, and the reset: an inline component has no access to the ids of
    // this file.
    component LookRow: RowLayout {
        id: lookRow

        property string label: ""
        property bool modified: false
        property int labelWidth: 160
        property color textColor: appTheme.textColor
        default property alias control: controlSlot.data

        signal resetClicked()

        Layout.fillWidth: true
        spacing: 16

        Label {
            Layout.preferredWidth: lookRow.labelWidth
            Layout.alignment: Qt.AlignVCenter
            text: lookRow.label
            color: lookRow.textColor
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightStrong
            wrapMode: Text.Wrap
        }

        RowLayout {
            id: controlSlot
            Layout.fillWidth: true
            spacing: 0
        }

        Item {
            Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
            Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
            Layout.alignment: Qt.AlignVCenter

            IconActionButton {
                objectName: lookRow.objectName + "Reset"
                anchors.centerIn: parent
                compact: true
                visible: lookRow.modified
                iconSrc: "qrc:/panel_icons/reset.svg"
                actionName: qsTr("Reset %1").arg(lookRow.label)
                onClicked: lookRow.resetClicked()
            }
        }
    }
}
