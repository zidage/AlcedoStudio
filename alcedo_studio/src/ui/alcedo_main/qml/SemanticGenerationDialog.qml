import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

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
    width: Math.min(parent ? parent.width - 56 : 560, 560)
    height: contentColumn.implicitHeight + 48
    padding: 0

    property bool promptVisible: false
    property bool generationRunning: false
    property int pendingCount: 0
    property int total: 0
    property int embedded: 0
    property int skipped: 0
    property int failed: 0
    property int canceled: 0
    property string statusText: ""
    property Item backgroundSource: null
    property bool startTransitionPending: false
    property bool backgrounded: false
    property real elapsedSecs: 0
    property var generationStart: null

    signal startRequested(bool rememberChoice)
    signal skipRequested(bool rememberChoice)
    signal cancelRequested()

    readonly property int completed: embedded + skipped + failed + canceled
    readonly property real progressValue: total > 0 ? completed / total : 0
    readonly property bool showingGeneration: startTransitionPending
                                              || (generationRunning && !backgrounded)
    // Rough ETA: batch size is fixed (64 or 4), so once the first batch lands the
    // completion rate stabilizes and remaining time = remaining / rate.
    readonly property real processingRate: (elapsedSecs > 0 && completed > 0)
                                           ? completed / elapsedSecs : 0
    readonly property real remainingSecs: processingRate > 0
                                          ? (total - completed) / processingRate : 0
    readonly property color panelColor: appTheme.toneGraphite
    readonly property color sectionColor: appTheme.bgBaseColor
    readonly property color textColor: appTheme.textColor
    readonly property color mutedTextColor: appTheme.textMutedColor
    readonly property color accentColor: appTheme.accentColor
    readonly property string dataFontFamily: appTheme.dataFontFamily

    onGenerationRunningChanged: {
        if (generationRunning || !promptVisible) {
            startTransitionPending = false
        }
        if (generationRunning) {
            backgrounded = false
            generationStart = Date.now()
            elapsedSecs = 0
            etaTimer.start()
        } else {
            backgrounded = false
            etaTimer.stop()
            generationStart = null
        }
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
            if (root.generationStart !== null) {
                root.elapsedSecs = Math.max(0, (Date.now() - root.generationStart) / 1000)
            }
        }
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
        color: root.panelColor
        border.width: 0
    }

    contentItem: ColumnLayout {
        id: contentColumn
        width: root.width
        spacing: 18

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 24
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: root.showingGeneration
                      ? qsTr("Generating semantic labels")
                      : qsTr("Use AI to analyze image content?")
                color: root.textColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: 24
                font.weight: 700
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: root.showingGeneration
                text: root.showingGeneration
                      ? (root.statusText.length > 0
                         ? root.statusText
                         : qsTr("Starting semantic generation..."))
                      : ""
                color: root.mutedTextColor
                font.pixelSize: 13
                font.weight: 500
                lineHeight: 1.25
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            visible: root.showingGeneration
            spacing: 12

            ImportProgressRing {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 132
                Layout.preferredHeight: 132
                ringWidth: 11
                trackColor: Qt.rgba(1, 1, 1, 0.07)
                fillColor: root.accentColor
                progress: root.progressValue
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: root.total > 0
                      ? qsTr("%1 / %2").arg(root.completed).arg(root.total)
                      : qsTr("Preparing")
                color: root.textColor
                font.family: root.dataFontFamily
                font.pixelSize: 22
                font.weight: 700
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                visible: root.generationRunning && root.generationStart !== null
                text: {
                    var elapsed = root.formatDuration(root.elapsedSecs)
                    if (root.completed > 0 && root.processingRate > 0) {
                        var rem = root.formatDuration(root.remainingSecs)
                        return qsTr("Elapsed %1 · ~%2 remaining").arg(elapsed).arg(rem)
                    }
                    return qsTr("Elapsed %1 · estimating…").arg(elapsed)
                }
                color: root.mutedTextColor
                font.pixelSize: 12
                font.weight: 500
            }
        }

        CheckBox {
            id: rememberChoice
            visible: !root.showingGeneration
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.preferredHeight: 30
            text: qsTr("Remember My Choice")
            checked: false
            spacing: 10
            indicator: Rectangle {
                implicitWidth: 22
                implicitHeight: 22
                x: 0
                y: Math.round((parent.height - height) / 2)
                radius: 5
                color: rememberChoice.checked
                       ? Qt.rgba(root.accentColor.r, root.accentColor.g, root.accentColor.b, 0.22)
                       : Qt.rgba(1, 1, 1, 0.04)
                border.width: 2
                border.color: rememberChoice.checked
                              ? root.accentColor
                              : Qt.rgba(root.textColor.r, root.textColor.g, root.textColor.b, 0.62)

                Rectangle {
                    visible: rememberChoice.checked
                    anchors.centerIn: parent
                    width: 10
                    height: 10
                    radius: 3
                    color: root.accentColor
                }
            }
            contentItem: Label {
                text: rememberChoice.text
                leftPadding: rememberChoice.indicator.width + rememberChoice.spacing
                verticalAlignment: Text.AlignVCenter
                color: root.mutedTextColor
                font.pixelSize: 12
                font.weight: 600
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.bottomMargin: 24
            spacing: 12

            Item { Layout.fillWidth: true }

            AiButton {
                objectName: "semanticGenerationDialogSkipButton"
                Layout.preferredWidth: 142
                text: root.showingGeneration ? qsTr("Cancel") : qsTr("Skip")
                primary: false
                onClicked: {
                    if (root.showingGeneration) {
                        root.startTransitionPending = false
                        root.cancelRequested()
                    } else {
                        root.skipRequested(rememberChoice.checked)
                    }
                }
            }

            IconButton {
                visible: root.showingGeneration
                buttonWidth: 54
                buttonHeight: 48
                buttonRadius: 10
                iconSize: 18
                kind: "accent"
                accentColor: root.accentColor
                bordered: true
                borderColor: Qt.rgba(root.accentColor.r,
                                     root.accentColor.g,
                                     root.accentColor.b,
                                     0.20)
                iconSrc: "qrc:/panel_icons/to_bg.svg"
                tooltipText: qsTr("Move task to background")
                enabled: root.generationRunning
                onClicked: root.runInBackground()
            }

            AiButton {
                visible: !root.showingGeneration
                Layout.preferredWidth: 168
                text: qsTr("Generate")
                primary: true
                enabled: root.pendingCount > 0
                onClicked: {
                    root.startTransitionPending = true
                    root.startRequested(rememberChoice.checked)
                }
            }
        }
    }

    component AiButton: Button {
        property bool primary: false

        Layout.preferredHeight: 48
        topInset: 0
        bottomInset: 0
        leftInset: 0
        rightInset: 0
        font.pixelSize: 15
        font.weight: 800
        hoverEnabled: true
        contentItem: Label {
            text: parent.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: root.textColor
            font.pixelSize: parent.font.pixelSize
            font.weight: parent.font.weight
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 10
            color: parent.primary
                   ? (parent.down
                      ? Qt.darker(root.accentColor, 1.16)
                      : (parent.hovered ? Qt.lighter(root.accentColor, 1.06)
                                        : root.accentColor))
                   : (parent.down
                      ? Qt.rgba(1, 1, 1, 0.07)
                      : (parent.hovered ? Qt.rgba(1, 1, 1, 0.14)
                                        : Qt.rgba(1, 1, 1, 0.10)))
            border.width: 1
            border.color: parent.primary
                          ? Qt.rgba(root.accentColor.r, root.accentColor.g, root.accentColor.b, 0.20)
                          : Qt.rgba(1, 1, 1, 0.10)
            opacity: parent.enabled ? 1.0 : 0.45
        }
    }
}
