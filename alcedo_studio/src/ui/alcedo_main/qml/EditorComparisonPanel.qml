import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Compare page of the adjustment stack.
//
// Shows the A and B source selectors, the display mode, the divider orientation, Swap, Close,
// the fixed-sensor-settings info hint, and the loading, error, Retry, and HDR-unavailable states.
// A comparison opens as unadjusted against current; the user picks other sources in A and B.
//
// The panel owns no comparison state. Every control reports a request signal; the owner
// changes its state and binds the result back. Selections that need new images are disabled
// while a pair renders. Display mode, orientation, and Swap only change the view of a ready
// pair, so they stay available. Close is always available.
Item {
    id: root
    objectName: "editorComparisonPanel"

    // Source choices of the open image: [{ value, label }, ...].
    property var sourceOptions: []
    property string aSourceValue: ""
    property string bSourceValue: ""
    // "complete" | "divider"
    property string displayMode: "divider"
    // "horizontal" (left/right) | "vertical" (top/bottom)
    property string orientation: "horizontal"
    // "idle" | "loading" | "ready" | "failed"
    property string status: "idle"
    property string errorText: ""
    // True when HDR output makes comparison unavailable; hdrReason states why.
    property bool hdrUnavailable: false
    property string hdrReason: ""

    readonly property bool rendering: status === "loading"
    readonly property bool selectionEnabled: !rendering && !hdrUnavailable

    signal aSourceRequested(string value)
    signal bSourceRequested(string value)
    signal displayModeRequested(string value)
    signal orientationRequested(string value)
    signal swapRequested()
    signal retryRequested()
    signal closeRequested()

    function indexOfValue(value) {
        for (let i = 0; i < root.sourceOptions.length; ++i) {
            if (String(root.sourceOptions[i].value) === value)
                return i
        }
        return -1
    }

    implicitHeight: content.implicitHeight + 2 * appTheme.spaceMd

    // AdjustmentCombo reads a model object; these adapters forward the user's choice as a
    // request and follow the bound selection.
    QtObject {
        id: aSourceModel
        readonly property string label: qsTr("A")
        readonly property var entries: root.sourceOptions
        readonly property int currentIndex: root.indexOfValue(root.aSourceValue)
        readonly property bool enabled: root.selectionEnabled
        function selectIndex(index) {
            if (index >= 0 && index < root.sourceOptions.length)
                root.aSourceRequested(String(root.sourceOptions[index].value))
        }
    }
    QtObject {
        id: bSourceModel
        readonly property string label: qsTr("B")
        readonly property var entries: root.sourceOptions
        readonly property int currentIndex: root.indexOfValue(root.bSourceValue)
        readonly property bool enabled: root.selectionEnabled
        function selectIndex(index) {
            if (index >= 0 && index < root.sourceOptions.length)
                root.bSourceRequested(String(root.sourceOptions[index].value))
        }
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: appTheme.spaceMd
        spacing: appTheme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: qsTr("Compare")
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightHeading
                wrapMode: Text.Wrap
            }

            EditorInfoHint {
                objectName: "editorComparisonSensorInfoHint"
                Layout.alignment: Qt.AlignVCenter
                text: qsTr("Both images use the current demosaic, highlight reconstruction, and lens settings. Each image keeps its own white balance and adjustments. Unadjusted is the imported image with these settings.")
            }
        }

        AdjustmentCombo {
            objectName: "editorComparisonSourceA"
            controlObjectName: "editorComparisonSourceACombo"
            Layout.fillWidth: true
            model: aSourceModel
            showResetButton: false
        }

        AdjustmentCombo {
            objectName: "editorComparisonSourceB"
            controlObjectName: "editorComparisonSourceBCombo"
            Layout.fillWidth: true
            model: bSourceModel
            showResetButton: false
        }

        SegmentedCardSwitcher {
            objectName: "editorComparisonDisplayModeSwitcher"
            Layout.fillWidth: true
            currentValue: root.displayMode
            entries: [
                { value: "complete", label: qsTr("Complete images") },
                { value: "divider", label: qsTr("Divider") }
            ]
            onSelected: function (index, value) {
                if (value !== root.displayMode)
                    root.displayModeRequested(value)
            }
        }

        SegmentedCardSwitcher {
            objectName: "editorComparisonOrientationSwitcher"
            Layout.fillWidth: true
            currentValue: root.orientation
            entries: [
                { value: "horizontal", label: qsTr("Left/right") },
                { value: "vertical", label: qsTr("Top/bottom") }
            ]
            onSelected: function (index, value) {
                if (value !== root.orientation)
                    root.orientationRequested(value)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            DialogActionButton {
                objectName: "editorComparisonSwapButton"
                Layout.fillWidth: true
                buttonWidth: 0
                text: qsTr("Swap A and B")
                onClicked: root.swapRequested()
            }
            DialogActionButton {
                objectName: "editorComparisonCloseButton"
                Layout.fillWidth: true
                buttonWidth: 0
                text: qsTr("Close")
                onClicked: root.closeRequested()
            }
        }

        Label {
            objectName: "editorComparisonStatusLabel"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.Wrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
            color: root.status === "failed" ? appTheme.dangerColor : appTheme.textMutedColor
            text: {
                if (root.hdrUnavailable)
                    return root.hdrReason.length > 0
                            ? root.hdrReason
                            : qsTr("Comparison is unavailable for HDR output.")
                if (root.status === "loading")
                    return qsTr("Rendering comparison images")
                if (root.status === "failed")
                    return root.errorText
                return ""
            }
        }

        DialogActionButton {
            objectName: "editorComparisonRetryButton"
            Layout.fillWidth: true
            buttonWidth: 0
            visible: root.status === "failed" && !root.hdrUnavailable
            text: qsTr("Retry")
            onClicked: root.retryRequested()
        }
    }
}
