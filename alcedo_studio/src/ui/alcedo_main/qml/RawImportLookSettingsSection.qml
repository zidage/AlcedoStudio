import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Alcedo.Main 1.0

// RAW Import section of Settings > Default Behavior (#920): the look that new RAW imports write
// into the history root of each image. The rows edit the pending values of
// RawImportLookSettingsModel; the Done button of the dialog writes them (model.apply()). A
// double-click on the name of a row (or on a slider rail) sets the row to its app default;
// "Restore Defaults" sets every row.
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

    function resetRow(field) {
        if (section.model)
            section.model.resetRow(field)
    }

    // Adapters: the shared combo and toggle read their value and entries from a model object.
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

        // The rail covers +-5 EV; typing and a drag on the value reach +-10 EV.
        LookRow {
            objectName: "rawImportExposureRow"
            label: qsTr("Exposure")
            labelWidth: section.labelWidth
            textColor: section.textColor
            onResetRequested: section.resetRow("exposureEv")

            AlcedoSlider {
                objectName: "rawImportExposureSlider"
                Layout.fillWidth: true
                inlineHeight: 36
                valuePixelSize: appTheme.fontSizeBody
                enabled: section.model !== null
                from: -10
                to: 10
                softFrom: -5
                softTo: 5
                stepSize: 0.01
                precision: 2
                defaultValue: section.model ? section.model.exposureEvDefault : 0
                value: section.model ? section.model.exposureEv : 0
                onValueEdited: function (v) { section.model.setExposureEv(v) }
                onValueEntered: function (v) { section.model.setExposureEv(v) }
                onResetRequested: section.resetRow("exposureEv")
            }
        }

        LookRow {
            objectName: "rawImportSaturationRow"
            label: qsTr("Saturation")
            labelWidth: section.labelWidth
            textColor: section.textColor
            onResetRequested: section.resetRow("saturation")

            AlcedoSlider {
                objectName: "rawImportSaturationSlider"
                Layout.fillWidth: true
                inlineHeight: 36
                valuePixelSize: appTheme.fontSizeBody
                enabled: section.model !== null
                from: -100
                to: 100
                stepSize: 1
                precision: 0
                defaultValue: section.model ? section.model.saturationDefault : 0
                value: section.model ? section.model.saturation : 0
                onValueEdited: function (v) { section.model.setSaturation(v) }
                onValueEntered: function (v) { section.model.setSaturation(v) }
                onResetRequested: section.resetRow("saturation")
            }
        }

        LookRow {
            objectName: "rawImportToneCurveRow"
            label: qsTr("Tone Curve")
            labelWidth: section.labelWidth
            textColor: section.textColor
            onResetRequested: section.resetRow("toneCurvePreset")

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
            onResetRequested: section.resetRow("drtMethod")

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
            onResetRequested: section.resetRow("openDrtLookPreset")

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
            onResetRequested: section.resetRow("lensCorrection")

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

    // One row: the name and the control (the default property). A double-click on the name resets
    // the row. The use site binds the label width, the colors, and the reset: an inline component
    // has no access to the ids of this file.
    component LookRow: RowLayout {
        id: lookRow

        property string label: ""
        property int labelWidth: 160
        property color textColor: appTheme.textColor
        default property alias control: controlSlot.data

        signal resetRequested()

        Layout.fillWidth: true
        spacing: 16

        Label {
            objectName: lookRow.objectName + "Label"
            Layout.preferredWidth: lookRow.labelWidth
            Layout.alignment: Qt.AlignVCenter
            text: lookRow.label
            color: lookRow.textColor
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightStrong
            wrapMode: Text.Wrap

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onDoubleClicked: lookRow.resetRequested()
            }
        }

        RowLayout {
            id: controlSlot
            Layout.fillWidth: true
            spacing: 0
        }
    }
}
