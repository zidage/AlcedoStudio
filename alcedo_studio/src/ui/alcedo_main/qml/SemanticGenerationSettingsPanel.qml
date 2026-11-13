pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "DataFormat.js" as DataFormat

// Settings > Local Content Recognition, laid out as bento tiles like Settings > LUTs (#762).
// The page fits the Settings dialog without scrolling:
// - Row 1: the AI labels tile and the Photos count. Idle, the tile shows the label coverage
//   (legend and SplitBar) with "Regenerate all" and "Generate"; while labels are made, the same
//   place shows the task progress (ThemedProgressBar), "Move task to background", and "Cancel".
// - Row 2: the model of the project ("Manage" opens Settings > AI Models), how long the model
//   stays loaded, and what happens to the labels of imported photos.
// The page has no explanatory sentences and no accent color: buttons are neutral, and the
// main action is the light DialogActionButton.
ColumnLayout {
    id: page
    objectName: "semanticSettingsPanel"

    property var semanticController: null
    // The interaction-policy controller; the generate buttons bind to its cached properties.
    property var interactionPolicy: null
    property bool projectOpen: false
    property string importPreference: "ask"
    // The effective UI language, for the short form of large counts (102K, 10.2万).
    property string languageCode: "en"

    signal importPreferenceRequested(string preference)
    signal backgroundRequested()
    // "Manage" opens Settings > AI Models, where the models are downloaded and activated.
    signal aiModelsPageRequested()

    readonly property bool hasController: semanticController !== null
    readonly property int albumTotalCount: hasController ? semanticController.albumTotalCount : 0
    readonly property int albumLabeledCount: hasController ? semanticController.albumLabeledCount : 0
    readonly property int albumUnlabeledCount: hasController ? semanticController.albumUnlabeledCount : 0
    readonly property bool generationRunning: hasController && semanticController.running
    readonly property bool hasActiveModel: hasController && projectOpen
                                           && semanticController.activeModelKey.length > 0
    readonly property bool policyAllowsGeneration: !interactionPolicy
                                                   || interactionPolicy.canRunSemanticGeneration

    readonly property int progressTotal: hasController ? semanticController.total : 0
    readonly property int progressCompleted: hasController
                                           ? semanticController.embedded
                                             + semanticController.skipped
                                             + semanticController.failed
                                             + semanticController.canceled
                                           : 0
    // Seconds until the generation finishes, from the controller's rate of the last minute; -1
    // while no estimate exists.
    readonly property int remainingSeconds: hasController ? semanticController.remainingSeconds : -1

    readonly property color labeledColor: appTheme.textColor
    readonly property color unlabeledColor: Qt.rgba(appTheme.textMutedColor.r,
                                                    appTheme.textMutedColor.g,
                                                    appTheme.textMutedColor.b, 0.45)

    spacing: appTheme.spaceMd

    function importPreferenceIndex(value) {
        if (value === "always")
            return 0
        if (value === "never")
            return 2
        return 1
    }

    function preferenceForIndex(index) {
        if (index === 0)
            return "always"
        if (index === 2)
            return "never"
        return "ask"
    }

    function modelText() {
        if (!projectOpen)
            return qsTr("No open project")
        if (!hasActiveModel)
            return qsTr("No model")
        return semanticController.activeModelDisplayName
    }

    QtObject {
        id: idleStopComboModel
        property string label: ""
        property bool enabled: page.hasController
        readonly property var entries: [
            { value: 0, label: qsTr("Unload after use") },
            { value: 30, label: qsTr("30 seconds") },
            { value: 60, label: qsTr("1 minute") },
            { value: 300, label: qsTr("5 minutes") },
            { value: 900, label: qsTr("15 minutes") },
            { value: -1, label: qsTr("While the project is open") }
        ]
        property int currentIndex: {
            const seconds = page.hasController ? page.semanticController.sidecarIdleStopSeconds : 30
            for (let i = 0; i < entries.length; ++i) {
                if (entries[i].value === seconds)
                    return i
            }
            return 1
        }
        function selectIndex(index) {
            if (page.hasController && index >= 0 && index < entries.length)
                page.semanticController.sidecarIdleStopSeconds = entries[index].value
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
        property int currentIndex: page.importPreferenceIndex(page.importPreference)
        function selectIndex(index) {
            page.importPreferenceRequested(page.preferenceForIndex(index))
        }
    }

    Component.onCompleted: {
        if (page.hasController)
            page.semanticController.RefreshAlbumSummary()
    }

    onSemanticControllerChanged: {
        if (page.hasController)
            page.semanticController.RefreshAlbumSummary()
    }

    // ── Bento row 1: AI labels + photo count ──────────────────────────────
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 26
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        spacing: appTheme.spaceMd

        BentoTile {
            objectName: "semanticLabelsTile"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: labelsColumn.implicitHeight

            ColumnLayout {
                id: labelsColumn
                anchors.fill: parent
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceXl

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("AI labels")
                        color: appTheme.textColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: appTheme.fontSizeSection
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    LegendItem {
                        objectName: "semanticLabeledLegend"
                        visible: !page.generationRunning
                        swatch: page.labeledColor
                        label: qsTr("With labels")
                        count: page.albumLabeledCount
                    }
                    LegendItem {
                        objectName: "semanticUnlabeledLegend"
                        visible: !page.generationRunning
                        swatch: page.unlabeledColor
                        label: qsTr("Need labels")
                        count: page.albumUnlabeledCount
                    }
                    RowLayout {
                        visible: page.generationRunning
                        spacing: appTheme.spaceSm

                        Label {
                            text: qsTr("Labeling")
                            color: appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeBody
                        }
                        Label {
                            objectName: "semanticRunningCount"
                            text: DataFormat.grouped(page.progressTotal, Qt.locale())
                            color: appTheme.textColor
                            font.family: appTheme.headlineFontFamily
                            font.pixelSize: appTheme.fontSizeSection
                            font.weight: Font.DemiBold
                            font.features: { "tnum": 1 }
                        }
                        Label {
                            text: qsTr("photos")
                            color: appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeBody
                        }
                    }
                }

                Item {
                    Layout.fillHeight: true
                    Layout.minimumHeight: appTheme.spaceXl
                }

                // The idle body and the running body share one place; it has the height of the
                // taller one, so the tile keeps its height when a generation starts or ends.
                Item {
                    Layout.fillWidth: true
                    implicitHeight: Math.max(idleBody.implicitHeight, runningBody.implicitHeight)

                    ColumnLayout {
                        id: idleBody
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        visible: !page.generationRunning
                        spacing: appTheme.spaceLg

                        SplitBar {
                            objectName: "semanticCoverageBar"
                            Layout.fillWidth: true
                            firstCount: page.albumLabeledCount
                            secondCount: page.albumUnlabeledCount
                            firstColor: page.labeledColor
                            secondColor: page.unlabeledColor
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignRight
                            spacing: appTheme.spaceSm

                            DialogActionButton {
                                id: regenerateButton
                                objectName: "semanticRegenerateButton"
                                buttonWidth: Math.max(112, implicitContentWidth + 36)
                                buttonHeight: 34
                                text: qsTr("Regenerate all")
                                enabled: page.hasController && page.albumTotalCount > 0
                                         && page.policyAllowsGeneration
                                onClicked: page.semanticController.StartAlbumGeneration(true)
                            }
                            DialogActionButton {
                                id: generateButton
                                objectName: "semanticGenerateButton"
                                kind: "light"
                                buttonWidth: Math.max(112, implicitContentWidth + 36)
                                buttonHeight: 34
                                text: qsTr("Generate")
                                enabled: page.hasController && page.albumUnlabeledCount > 0
                                         && page.policyAllowsGeneration
                                onClicked: page.semanticController.StartAlbumGeneration(false)
                            }
                        }
                    }

                    ColumnLayout {
                        id: runningBody
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        visible: page.generationRunning
                        spacing: appTheme.spaceMd

                        ThemedProgressBar {
                            objectName: "semanticProgressBar"
                            Layout.fillWidth: true
                            showDetails: true
                            active: page.generationRunning
                            indeterminate: page.progressTotal <= 0
                            progressValue: page.progressTotal > 0
                                           ? page.progressCompleted * 100 / page.progressTotal : 0
                            fillColor: appTheme.textColor
                            leadingText: page.progressTotal > 0
                                         ? qsTr("%1 / %2").arg(DataFormat.grouped(page.progressCompleted, Qt.locale()))
                                                          .arg(DataFormat.grouped(page.progressTotal, Qt.locale()))
                                         : ""
                            etaText: DataFormat.remaining(page.remainingSeconds)
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignRight
                            spacing: appTheme.spaceSm

                            IconButton {
                                objectName: "semanticBackgroundButton"
                                buttonSize: 34
                                buttonRadius: appTheme.controlRadius
                                iconSize: 17
                                iconSrc: "qrc:/panel_icons/to_bg.svg"
                                normalColor: appTheme.controlFillColor
                                borderColor: appTheme.controlBorderColor
                                tooltipText: qsTr("Move task to background")
                                enabled: page.generationRunning
                                onClicked: page.backgroundRequested()
                            }
                            DialogActionButton {
                                objectName: "semanticCancelButton"
                                buttonWidth: Math.max(96, implicitContentWidth + 36)
                                buttonHeight: 34
                                text: qsTr("Cancel")
                                enabled: page.generationRunning
                                onClicked: page.semanticController.CancelGeneration()
                            }
                        }
                    }
                }
            }
        }

        CountTile {
            objectName: "semanticPhotosTile"
            Layout.fillHeight: true
            label: qsTr("Photos")
            count: page.albumTotalCount
            compactLargeCounts: true
            languageCode: page.languageCode
            countLabel.objectName: "semanticPhotoCount"
            exactCountLabel.objectName: "semanticExactPhotoCount"
        }
    }

    // ── Bento row 2: model and behavior ───────────────────────────────────
    BentoTile {
        objectName: "semanticBehaviorTile"
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.bottomMargin: 26
        contentHeight: behaviorColumn.implicitHeight

        ColumnLayout {
            id: behaviorColumn
            anchors.fill: parent
            spacing: 0

            SettingRow {
                label: qsTr("Model")

                Label {
                    objectName: "semanticActiveModelName"
                    Layout.maximumWidth: 280
                    text: page.modelText()
                    color: page.hasActiveModel ? appTheme.textColor : appTheme.textMutedColor
                    font.family: appTheme.headlineFontFamily
                    font.pixelSize: appTheme.fontSizeTitle
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                DialogActionButton {
                    objectName: "semanticOpenAiModelsButton"
                    buttonWidth: Math.max(96, implicitContentWidth + 36)
                    buttonHeight: 34
                    text: qsTr("Manage")
                    onClicked: page.aiModelsPageRequested()
                }
            }

            Divider {}

            SettingRow {
                label: qsTr("Keep the model loaded")

                AdjustmentCombo {
                    objectName: "sidecarIdleStopControl"
                    controlObjectName: "sidecarIdleStopCombo"
                    Layout.fillWidth: false
                    Layout.preferredWidth: 220
                    controlHeight: 34
                    showResetButton: false
                    model: idleStopComboModel
                }
            }

            Divider {}

            SettingRow {
                label: qsTr("Labels for imported photos")

                AdjustmentCombo {
                    objectName: "semanticImportControl"
                    controlObjectName: "semanticImportCombo"
                    Layout.fillWidth: false
                    Layout.preferredWidth: 220
                    controlHeight: 34
                    showResetButton: false
                    model: importComboModel
                }
            }
        }
    }

    Item {
        Layout.fillHeight: true
    }

    // A row of the behavior tile: the name at the left, the value and control at the right.
    component SettingRow: RowLayout {
        id: settingRow

        property string label: ""

        Layout.fillWidth: true
        Layout.topMargin: appTheme.spaceSm
        Layout.bottomMargin: appTheme.spaceSm
        spacing: appTheme.spaceLg

        Label {
            Layout.fillWidth: true
            text: settingRow.label
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightStrong
            elide: Text.ElideRight
        }
    }

    component Divider: Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: appTheme.dividerColor
    }
}
