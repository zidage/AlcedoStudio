import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ColumnLayout {
    id: panel

    property var semanticController: null
    // Phase 2: the interaction-policy controller. The generate checks below bind
    // to its cached Q_PROPERTYs (re-evaluated on PolicyChanged).
    property var interactionPolicy: null
    property string importPreference: "ask"
    property color primaryAccent: "#457B9D"
    property color secondaryAccent: "#9FC7D8"
    property color textColor: "#F5F1EA"
    property color mutedTextColor: "#B6B0A7"
    property color canvasColor: "#111214"
    property color dividerColor: Qt.rgba(1, 1, 1, 0.08)
    property color dangerColor: "#D96C75"
    property string dataFontFamily: appTheme.dataFontFamily

    signal importPreferenceRequested(string preference)
    signal messageRequested(string message)
    signal backgroundRequested()
    // The model section opens Settings > AI Models, where the models are downloaded and activated.
    signal aiModelsPageRequested()

    readonly property bool hasController: semanticController !== null
    readonly property int albumTotalCount: hasController ? semanticController.albumTotalCount : 0
    readonly property int albumLabeledCount: hasController ? semanticController.albumLabeledCount : 0
    readonly property int albumUnlabeledCount: hasController ? semanticController.albumUnlabeledCount : 0
    readonly property bool generationRunning: hasController ? semanticController.running : false
    readonly property string activeModelName: hasController ? semanticController.activeModelDisplayName : qsTr("No active model")

    readonly property int progressTotal: hasController ? semanticController.total : 0
    readonly property int progressCompleted: hasController
                                           ? semanticController.embedded
                                             + semanticController.skipped
                                             + semanticController.failed
                                             + semanticController.canceled
                                           : 0
    readonly property real progressValue: progressTotal > 0 ? progressCompleted / progressTotal : 0
    property real elapsedSecs: 0
    property var generationStart: null
    // Rough ETA: batch size is fixed (64 or 4), so once the first batch lands the
    // completion rate stabilizes and remaining time = remaining / rate.
    readonly property real processingRate: (elapsedSecs > 0 && progressCompleted > 0)
                                           ? progressCompleted / elapsedSecs : 0
    readonly property real remainingSecs: processingRate > 0
                                          ? (progressTotal - progressCompleted) / processingRate
                                          : 0

    onGenerationRunningChanged: {
        if (generationRunning) {
            generationStart = Date.now()
            elapsedSecs = 0
            etaTimer.start()
        } else {
            etaTimer.stop()
            generationStart = null
        }
    }

    function _pad2(n) {
        n = Math.floor(n)
        return n < 10 ? "0" + n : "" + n
    }

    function formatDuration(secs) {
        var s = Math.max(0, Math.floor(secs))
        var h = Math.floor(s / 3600)
        var m = Math.floor((s % 3600) / 60)
        if (h > 0) {
            return h + ":" + _pad2(m) + ":" + _pad2(s % 60)
        }
        return _pad2(m) + ":" + _pad2(s % 60)
    }

    Timer {
        id: etaTimer
        interval: 1000
        repeat: true
        onTriggered: {
            if (panel.generationStart !== null) {
                panel.elapsedSecs = Math.max(0, (Date.now() - panel.generationStart) / 1000)
            }
        }
    }

    width: parent ? parent.width : implicitWidth
    spacing: 20

    function importPreferenceIndex(value) {
        if (value === "always") {
            return 0
        }
        if (value === "never") {
            return 2
        }
        return 1
    }

    function preferenceForIndex(index) {
        if (index === 0) {
            return "always"
        }
        if (index === 2) {
            return "never"
        }
        return "ask"
    }

    QtObject {
        id: idleStopComboModel
        property string label: ""
        property bool enabled: panel.hasController
        readonly property var entries: [
            { value: 0, label: qsTr("Unload after use") },
            { value: 30, label: qsTr("30 seconds") },
            { value: 60, label: qsTr("1 minute") },
            { value: 300, label: qsTr("5 minutes") },
            { value: 900, label: qsTr("15 minutes") },
            { value: -1, label: qsTr("While the project is open") }
        ]
        property int currentIndex: {
            const seconds = panel.hasController ? panel.semanticController.sidecarIdleStopSeconds
                                                : 30
            for (let i = 0; i < entries.length; ++i) {
                if (entries[i].value === seconds)
                    return i
            }
            return 1
        }
        function selectIndex(index) {
            if (panel.hasController && index >= 0 && index < entries.length)
                panel.semanticController.sidecarIdleStopSeconds = entries[index].value
        }
    }

    QtObject {
        id: importComboModel
        property string label: ""
        property bool enabled: true
        readonly property var entries: [
            { value: "always", label: qsTr("Always") },
            { value: "ask", label: qsTr("Always Ask") },
            { value: "never", label: qsTr("Always Skip") }
        ]
        property int currentIndex: panel.importPreferenceIndex(panel.importPreference)
        function selectIndex(index) {
            panel.importPreferenceRequested(panel.preferenceForIndex(index))
        }
    }

    Component.onCompleted: {
        if (panel.hasController) {
            panel.semanticController.RefreshAlbumSummary()
        }
    }

    onSemanticControllerChanged: {
        if (panel.hasController) {
            panel.semanticController.RefreshAlbumSummary()
        }
    }

    SettingsSection {
        Layout.fillWidth: true
        title: qsTr("AI content recognition")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        GridLayout {
            Layout.fillWidth: true
            columns: 3
            columnSpacing: 12
            rowSpacing: 12

            MetricCard {
                Layout.fillWidth: true
                label: qsTr("Images")
                value: String(panel.albumTotalCount)
            }

            MetricCard {
                Layout.fillWidth: true
                label: qsTr("With labels")
                value: String(panel.albumLabeledCount)
            }

            MetricCard {
                Layout.fillWidth: true
                label: qsTr("Need labels")
                value: String(panel.albumUnlabeledCount)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    text: panel.generationRunning
                          ? (panel.hasController ? panel.semanticController.statusText : "")
                          : qsTr("Generate labels only for images that do not have AI content labels.")
                    color: panel.mutedTextColor
                    font.pixelSize: 13
                    font.weight: 500
                    wrapMode: Text.WordWrap
                    lineHeight: 1.25
                }

                ProgressBar {
                    visible: panel.generationRunning
                    Layout.fillWidth: true
                    Layout.preferredHeight: 8
                    from: 0
                    to: 1
                    value: panel.progressValue
                    indeterminate: panel.progressTotal <= 0
                }

                Label {
                    visible: panel.generationRunning
                    text: qsTr("%1 / %2").arg(panel.progressCompleted).arg(panel.progressTotal)
                    color: panel.textColor
                    font.family: panel.dataFontFamily
                    font.pixelSize: 13
                    font.weight: 700
                }

                Label {
                    visible: panel.generationRunning && panel.generationStart !== null
                    text: {
                        var elapsed = panel.formatDuration(panel.elapsedSecs)
                        if (panel.progressCompleted > 0 && panel.processingRate > 0) {
                            var rem = panel.formatDuration(panel.remainingSecs)
                            return qsTr("Elapsed %1 · ~%2 remaining").arg(elapsed).arg(rem)
                        }
                        return qsTr("Elapsed %1 · estimating…").arg(elapsed)
                    }
                    color: panel.mutedTextColor
                    font.pixelSize: 12
                    font.weight: 500
                }
            }

            Button {
                id: generateButton
                Layout.preferredWidth: 148
                Layout.preferredHeight: 48
                topInset: 0
                bottomInset: 0
                leftInset: 0
                rightInset: 0
                text: panel.generationRunning ? qsTr("Cancel") : qsTr("Generate")
                enabled: panel.hasController
                         && (panel.generationRunning
                             || (panel.albumUnlabeledCount > 0
                                 && (!panel.interactionPolicy
                                     || panel.interactionPolicy.canRunSemanticGeneration)))
                font.pixelSize: 15
                font.weight: 800
                Material.foreground: panel.textColor
                onClicked: {
                    if (!panel.hasController) {
                        return
                    }
                    if (panel.generationRunning) {
                        panel.semanticController.CancelGeneration()
                    } else {
                        panel.semanticController.StartAlbumGeneration(false)
                    }
                }
                background: Rectangle {
                    radius: 10
                    color: generateButton.down
                           ? Qt.darker(panel.primaryAccent, 1.16)
                           : (generateButton.hovered
                              ? Qt.lighter(panel.primaryAccent, 1.06)
                              : panel.primaryAccent)
                    border.width: 1
                    border.color: Qt.rgba(panel.secondaryAccent.r,
                                          panel.secondaryAccent.g,
                                          panel.secondaryAccent.b,
                                          0.18)
                    opacity: generateButton.enabled ? 1.0 : 0.45
                }
            }

            Button {
                id: regenerateButton
                visible: !panel.generationRunning
                Layout.preferredWidth: 148
                Layout.preferredHeight: 48
                topInset: 0
                bottomInset: 0
                leftInset: 0
                rightInset: 0
                text: qsTr("Regenerate")
                enabled: panel.hasController && panel.albumTotalCount > 0
                         && (!panel.interactionPolicy
                             || panel.interactionPolicy.canRunSemanticGeneration)
                font.pixelSize: 15
                font.weight: 800
                Material.foreground: panel.textColor
                onClicked: panel.semanticController.StartAlbumGeneration(true)
                background: Rectangle {
                    radius: 10
                    color: regenerateButton.down
                           ? Qt.rgba(1, 1, 1, 0.08)
                           : (regenerateButton.hovered
                              ? Qt.rgba(1, 1, 1, 0.14)
                              : Qt.rgba(1, 1, 1, 0.09))
                    border.width: 1
                    border.color: Qt.rgba(panel.textColor.r,
                                          panel.textColor.g,
                                          panel.textColor.b,
                                          0.16)
                    opacity: regenerateButton.enabled ? 1.0 : 0.45
                }
            }

            IconButton {
                visible: panel.generationRunning
                buttonWidth: 54
                buttonHeight: 48
                buttonRadius: 10
                iconSize: 18
                kind: "accent"
                accentColor: panel.primaryAccent
                bordered: true
                borderColor: Qt.rgba(panel.secondaryAccent.r,
                                     panel.secondaryAccent.g,
                                     panel.secondaryAccent.b,
                                     0.18)
                iconSrc: "qrc:/panel_icons/to_bg.svg"
                tooltipText: qsTr("Move task to background")
                enabled: panel.generationRunning
                onClicked: panel.backgroundRequested()
            }
        }
    }

    // The models are downloaded and activated in Settings > AI Models (AI Models plan, AM4).
    SettingsSection {
        Layout.fillWidth: true
        title: qsTr("Model")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Label {
                    text: qsTr("Active model of this project")
                    color: panel.mutedTextColor
                    font.pixelSize: 12
                    font.weight: 700
                }
                Label {
                    objectName: "semanticActiveModelName"
                    Layout.fillWidth: true
                    text: panel.activeModelName
                    color: panel.textColor
                    font.family: panel.dataFontFamily
                    font.pixelSize: 15
                    font.weight: 700
                    wrapMode: Text.Wrap
                }
            }

            DialogActionButton {
                objectName: "semanticOpenAiModelsButton"
                kind: "normal"
                buttonWidth: Math.max(148, implicitContentWidth + 32)
                buttonHeight: 42
                text: qsTr("Open AI Models")
                onClicked: panel.aiModelsPageRequested()
            }
        }
    }

    SettingsSection {
        Layout.fillWidth: true
        title: qsTr("Memory")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Label {
                Layout.preferredWidth: 180
                text: qsTr("Keep AI model loaded")
                color: panel.textColor
                font.pixelSize: 15
                font.weight: 600
            }

            AdjustmentCombo {
                objectName: "sidecarIdleStopControl"
                controlObjectName: "sidecarIdleStopCombo"
                Layout.fillWidth: true
                controlHeight: 36
                showResetButton: false
                model: idleStopComboModel
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 196
            text: qsTr("The text model's unload time applies to the sidecar the next time it starts.")
            color: panel.mutedTextColor
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
    }

    SettingsSection {
        Layout.fillWidth: true
        title: qsTr("Import")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Label {
                Layout.preferredWidth: 180
                text: qsTr("是否生成标签")
                color: panel.textColor
                font.pixelSize: 15
                font.weight: 600
            }

            AdjustmentCombo {
                id: importBehaviorBox
                objectName: "semanticImportControl"
                controlObjectName: "semanticImportCombo"
                Layout.fillWidth: true
                controlHeight: 36
                showResetButton: false
                model: importComboModel
            }
        }
    }

    Item { Layout.fillHeight: true }

    component SettingsSection: ColumnLayout {
        property string title: ""
        property color textColor: "white"
        property color mutedTextColor: "#999999"
        property color dividerColor: Qt.rgba(1, 1, 1, 0.08)

        spacing: 14

        Label {
            Layout.fillWidth: true
            text: title
            color: textColor
            font.pixelSize: 18
            font.weight: 800
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: dividerColor
        }
    }

    component MetricCard: Rectangle {
        property string label: ""
        property string value: ""

        implicitHeight: Math.max(84, metricCardColumn.implicitHeight + 28)
        radius: 8
        color: Qt.rgba(panel.canvasColor.r, panel.canvasColor.g, panel.canvasColor.b, 0.62)

        ColumnLayout {
            id: metricCardColumn
            anchors.fill: parent
            anchors.margins: 14
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: label
                color: panel.mutedTextColor
                font.pixelSize: 12
                font.weight: 700
                wrapMode: Text.Wrap
            }

            Label {
                Layout.fillWidth: true
                text: value
                color: panel.textColor
                font.family: panel.dataFontFamily
                font.pixelSize: 18
                font.weight: 700
                wrapMode: Text.Wrap
            }
        }
    }
}
