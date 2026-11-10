pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Effects
import "DataFormat.js" as DataFormat

// The AI label dialog after an import (#762): the prompt (photo count, "Remember my choice",
// "Skip", "Generate") and the progress view (ring, completed / total, remaining time,
// "Move task to background", "Cancel"). Neutral colors as on Settings > Local Content
// Recognition; numbers in Manrope with tabular figures.
Popup {
    id: root
    objectName: "semanticGenerationDialog"
    font.family: appTheme.uiFontFamily
    modal: true
    focus: true
    visible: promptVisible || showingGeneration
    closePolicy: showingGeneration ? Popup.NoAutoClose
                                   : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 56 : 480, 480)
    padding: 24

    property bool promptVisible: false
    property bool generationRunning: false
    property int pendingCount: 0
    property int total: 0
    property int embedded: 0
    property int skipped: 0
    property int failed: 0
    property int canceled: 0
    property Item backgroundSource: null
    property bool startTransitionPending: false
    property bool backgrounded: false
    // Seconds until the generation finishes, from the controller's rate of the last minute; -1
    // while no estimate exists.
    property int remainingSeconds: -1

    signal startRequested(bool rememberChoice)
    signal skipRequested(bool rememberChoice)
    signal cancelRequested()

    readonly property int completed: embedded + skipped + failed + canceled
    readonly property real progressValue: total > 0 ? completed / total : 0
    readonly property bool showingGeneration: startTransitionPending
                                              || (generationRunning && !backgrounded)

    onGenerationRunningChanged: {
        if (generationRunning || !promptVisible) {
            startTransitionPending = false
        }
        backgrounded = false
    }

    onPromptVisibleChanged: {
        if (promptVisible) {
            startTransitionPending = false
            backgrounded = false
        }
    }

    onClosed: startTransitionPending = false

    function runInBackground() {
        if (!generationRunning) {
            return
        }
        backgrounded = true
        close()
    }

    Overlay.modal: Item {
        anchors.fill: parent

        MultiEffect {
            visible: root.backgroundSource !== null
            anchors.fill: parent
            source: root.backgroundSource
            blurEnabled: true
            blur: 0.62
            blurMax: 64
            saturation: -0.24
        }

        Rectangle {
            anchors.fill: parent
            color: appTheme.overlayColor
        }

        MouseArea { anchors.fill: parent; hoverEnabled: true }
    }

    background: Rectangle {
        radius: 14
        color: appTheme.toneGraphite
        border.width: 1
        border.color: appTheme.cardBorderColor
    }

    contentItem: ColumnLayout {
        spacing: appTheme.spaceLg

        Label {
            objectName: "semanticGenerationDialogTitle"
            Layout.fillWidth: true
            text: root.showingGeneration ? qsTr("Generating AI labels")
                                         : qsTr("Generate AI labels for the new photos?")
            color: appTheme.textColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeHeadline
            font.weight: Font.Bold
            wrapMode: Text.WordWrap
        }

        // Prompt: the number of photos that need labels.
        RowLayout {
            visible: !root.showingGeneration
            spacing: appTheme.spaceSm

            Label {
                objectName: "semanticGenerationDialogPendingCount"
                text: DataFormat.grouped(root.pendingCount, Qt.locale())
                color: appTheme.textColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: Font.DemiBold
                font.features: { "tnum": 1 }
            }
            Label {
                text: qsTr("photos need labels")
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
            }
        }

        CheckBox {
            id: rememberChoice
            objectName: "semanticGenerationDialogRememberChoice"
            visible: !root.showingGeneration
            Layout.fillWidth: true
            text: qsTr("Remember my choice")
            checked: false
            spacing: 10
            padding: 0
            indicator: Rectangle {
                implicitWidth: 18
                implicitHeight: 18
                x: 0
                y: Math.round((rememberChoice.height - height) / 2)
                radius: 5
                color: rememberChoice.checked ? appTheme.textColor : "transparent"
                border.width: 2
                border.color: rememberChoice.checked
                              ? appTheme.textColor
                              : Qt.rgba(appTheme.textColor.r, appTheme.textColor.g,
                                        appTheme.textColor.b, 0.62)

                Rectangle {
                    visible: rememberChoice.checked
                    anchors.centerIn: parent
                    width: 8
                    height: 8
                    radius: 2
                    color: appTheme.toneGraphite
                }
            }
            contentItem: Label {
                text: rememberChoice.text
                leftPadding: rememberChoice.indicator.width + rememberChoice.spacing
                verticalAlignment: Text.AlignVCenter
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightStrong
            }
        }

        // Progress: the ring, completed / total, and the remaining time.
        RowLayout {
            visible: root.showingGeneration
            spacing: 22

            ImportProgressRing {
                Layout.preferredWidth: 112
                Layout.preferredHeight: 112
                ringWidth: 10
                trackColor: Qt.rgba(1, 1, 1, 0.07)
                fillColor: appTheme.textColor
                progress: root.progressValue
                indeterminate: root.total <= 0
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    objectName: "semanticGenerationDialogProgressText"
                    Layout.fillWidth: true
                    visible: root.total > 0
                    textFormat: Text.PlainText
                    text: qsTr("%1 / %2").arg(DataFormat.grouped(root.completed, Qt.locale()))
                                          .arg(DataFormat.grouped(root.total, Qt.locale()))
                    color: appTheme.textColor
                    font.family: appTheme.headlineFontFamily
                    font.pixelSize: 34
                    font.weight: Font.Light
                    font.features: { "tnum": 1 }
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 20
                }
                Label {
                    objectName: "semanticGenerationDialogRemainingText"
                    Layout.fillWidth: true
                    text: root.total > 0 ? DataFormat.remaining(root.remainingSeconds)
                                         : qsTr("Preparing")
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    wrapMode: Text.WordWrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: appTheme.spaceXs
            spacing: appTheme.spaceSm

            Item { Layout.fillWidth: true }

            IconButton {
                objectName: "semanticGenerationDialogBackgroundButton"
                visible: root.showingGeneration
                buttonSize: 34
                buttonRadius: appTheme.controlRadius
                iconSize: 17
                iconSrc: "qrc:/panel_icons/to_bg.svg"
                normalColor: appTheme.controlFillColor
                borderColor: appTheme.controlBorderColor
                tooltipText: qsTr("Move task to background")
                enabled: root.generationRunning
                onClicked: root.runInBackground()
            }

            DialogActionButton {
                objectName: "semanticGenerationDialogSkipButton"
                buttonWidth: Math.max(96, implicitContentWidth + 36)
                buttonHeight: 34
                text: root.showingGeneration ? qsTr("Cancel") : qsTr("Skip")
                onClicked: {
                    if (root.showingGeneration) {
                        root.startTransitionPending = false
                        root.cancelRequested()
                    } else {
                        root.skipRequested(rememberChoice.checked)
                    }
                }
            }

            DialogActionButton {
                objectName: "semanticGenerationDialogGenerateButton"
                visible: !root.showingGeneration
                kind: "light"
                buttonWidth: Math.max(112, implicitContentWidth + 36)
                buttonHeight: 34
                text: qsTr("Generate")
                enabled: root.pendingCount > 0
                onClicked: {
                    root.startTransitionPending = true
                    root.startRequested(rememberChoice.checked)
                }
            }
        }
    }
}
