import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

ColumnLayout {
    id: panel

    property var semanticController: null
    property var downloadController: null
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

    readonly property bool hasController: semanticController !== null
    readonly property bool hasDownloadController: downloadController !== null
    readonly property int albumTotalCount: hasController ? semanticController.albumTotalCount : 0
    readonly property int albumLabeledCount: hasController ? semanticController.albumLabeledCount : 0
    readonly property int albumUnlabeledCount: hasController ? semanticController.albumUnlabeledCount : 0
    readonly property bool generationRunning: hasController ? semanticController.running : false
    readonly property bool modelTaskRunning: (hasDownloadController && downloadController.modelDownloadRunning)
                                             || (hasController && semanticController.modelActivationRunning)
    readonly property bool modelDownloadRunning: hasDownloadController ? downloadController.modelDownloadRunning : false
    readonly property bool modelActivationRunning: hasController ? semanticController.modelActivationRunning : false
    readonly property string activeModelName: hasController ? semanticController.activeModelDisplayName : qsTr("No active model")
    readonly property string activeModelKey: hasController ? semanticController.activeModelKey : ""
    readonly property string selectedModelSizeLabel: hasDownloadController ? downloadController.selectedModelSizeLabel : ""
    readonly property bool selectedModelInstalled: hasDownloadController ? downloadController.selectedModelInstalled : false
    readonly property bool selectedModelActive: hasController ? semanticController.selectedModelActive : false
    readonly property string modelDownloadPhase: hasDownloadController ? downloadController.modelDownloadPhase : ""
    readonly property int modelDownloadProgress: hasDownloadController ? downloadController.modelDownloadProgress : 0
    readonly property real modelDownloadBytesDone: hasDownloadController ? downloadController.modelDownloadBytesDone : 0
    readonly property real modelDownloadBytesTotal: hasDownloadController ? downloadController.modelDownloadBytesTotal : 0
    readonly property int modelDownloadFilesDone: hasDownloadController ? downloadController.modelDownloadFilesDone : 0
    readonly property int modelDownloadFilesTotal: hasDownloadController ? downloadController.modelDownloadFilesTotal : 0
    readonly property string modelDownloadCurrentFile: hasDownloadController ? downloadController.modelDownloadCurrentFile : ""
    readonly property string modelDownloadBytesLabel: hasDownloadController ? downloadController.modelDownloadBytesLabel : ""
    readonly property string modelDownloadSpeedLabel: hasDownloadController ? downloadController.modelDownloadSpeedLabel : ""
    readonly property string modelDownloadEtaLabel: hasDownloadController ? downloadController.modelDownloadEtaLabel : ""
    // The controller's status text (errors, paths, completion messages). Shown
    // as a muted detail line inside the status card only when nothing is busy,
    // so the card stays the single source of model status.
    readonly property string modelDownloadStatusDetail: hasDownloadController ? downloadController.modelDownloadStatusText : ""
    readonly property string selectedModelDisplayName: {
        if (!panel.hasDownloadController) {
            return ""
        }
        const options = panel.downloadController.modelProfileOptions
        const id = panel.downloadController.selectedModelProfileId
        for (let i = 0; i < options.length; ++i) {
            if (options[i].profileId === id) {
                return options[i].label
            }
        }
        return ""
    }
    // True while the worker is in a phase that hasn't started moving bytes yet
    // (or is finalizing), so the progress bar shows indeterminate motion.
    readonly property bool modelDownloadIndeterminate: panel.modelTaskRunning
        && (panel.modelActivationRunning
            || panel.modelDownloadPhase === "preparing"
            || panel.modelDownloadPhase === "promoting"
            || panel.modelDownloadPhase === "cancelled"
            || panel.modelDownloadProgress <= 0)

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

    function modelProfileIndex(profileId) {
        if (!panel.hasDownloadController) {
            return 0
        }
        const options = panel.downloadController.modelProfileOptions
        for (let i = 0; i < options.length; ++i) {
            if (options[i].profileId === profileId) {
                return i
            }
        }
        return 0
    }

    function endpointPresetIndex(preset) {
        if (preset === "huggingface") {
            return 1
        }
        if (preset === "sufy") {
            return 2
        }
        if (preset === "custom") {
            return 3
        }
        return 0
    }

    function endpointPresetForIndex(index) {
        if (index === 1) {
            return "huggingface"
        }
        if (index === 2) {
            return "sufy"
        }
        if (index === 3) {
            return "custom"
        }
        return "mirror"
    }

    FolderDialog {
        id: modelFolderDialog
        title: qsTr("Select Model Download Folder")
        onAccepted: {
            if (panel.hasDownloadController) {
                panel.downloadController.SetModelDownloadDirectory(selectedFolder.toString())
            }
        }
    }

    Component.onCompleted: {
        if (panel.hasController) {
            panel.semanticController.RefreshAlbumSummary()
        }
        if (panel.hasDownloadController) {
            panel.downloadController.RefreshSelectedModelStatus()
        }
    }

    onSemanticControllerChanged: {
        if (panel.hasController) {
            panel.semanticController.RefreshAlbumSummary()
        }
        if (panel.hasDownloadController) {
            panel.downloadController.RefreshSelectedModelStatus()
        }
    }

    onDownloadControllerChanged: {
        if (panel.hasDownloadController) {
            panel.downloadController.RefreshSelectedModelStatus()
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
                text: panel.generationRunning ? qsTr("Cancel") : qsTr("Generate")
                enabled: panel.hasController
                         && (panel.generationRunning || panel.albumUnlabeledCount > 0)
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
                text: qsTr("Regenerate")
                enabled: panel.hasController && panel.albumTotalCount > 0
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
        }
    }

    SettingsSection {
        Layout.fillWidth: true
        title: qsTr("Model")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Label {
                Layout.preferredWidth: 180
                text: qsTr("Model")
                color: panel.textColor
                font.pixelSize: 15
                font.weight: 600
            }

            ComboBox {
                id: modelBox
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                enabled: panel.hasDownloadController && !panel.modelTaskRunning
                model: panel.hasDownloadController ? panel.downloadController.modelProfileOptions : []
                textRole: "label"
                currentIndex: panel.hasDownloadController
                              ? panel.modelProfileIndex(panel.downloadController.selectedModelProfileId)
                              : 0
                onActivated: function(index) {
                    const item = model[index]
                    if (item && panel.hasDownloadController) {
                        panel.downloadController.SetSelectedModelProfileId(item.profileId)
                        panel.downloadController.RefreshSelectedModelStatus()
                    }
                }
            }
        }

        ModelStatusCard {
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: qsTr("Download directory")
                    color: panel.textColor
                    font.pixelSize: 15
                    font.weight: 600
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 50
                    radius: 8
                    color: Qt.rgba(1, 1, 1, 0.10)
                    border.width: 1
                    border.color: Qt.rgba(panel.textColor.r, panel.textColor.g, panel.textColor.b, 0.12)

                    Label {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        text: panel.hasDownloadController ? panel.downloadController.modelDownloadDirectory : ""
                        elide: Text.ElideMiddle
                        verticalAlignment: Text.AlignVCenter
                        color: panel.textColor
                        font.family: panel.dataFontFamily
                        font.pixelSize: 14
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 50
                Layout.preferredHeight: 50
                Layout.alignment: Qt.AlignBottom
                radius: 8
                color: modelBrowseMouse.pressed
                       ? Qt.rgba(1, 1, 1, 0.06)
                       : (modelBrowseMouse.containsMouse
                          ? Qt.rgba(1, 1, 1, 0.12)
                          : Qt.rgba(1, 1, 1, 0.07))
                border.width: 1
                border.color: Qt.rgba(panel.textColor.r, panel.textColor.g, panel.textColor.b, 0.14)
                opacity: panel.hasController && !panel.modelTaskRunning ? 1 : 0.45

                Image {
                    anchors.centerIn: parent
                    width: 23
                    height: 23
                    source: "qrc:/panel_icons/folder-open.svg"
                    sourceSize.width: 23
                    sourceSize.height: 23
                    asynchronous: true
                }

                MouseArea {
                    id: modelBrowseMouse
                    anchors.fill: parent
                    enabled: panel.hasController && !panel.modelTaskRunning
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: modelFolderDialog.open()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Label {
                Layout.preferredWidth: 180
                text: qsTr("Source")
                color: panel.textColor
                font.pixelSize: 15
                font.weight: 600
            }

            ComboBox {
                id: endpointBox
                Layout.preferredWidth: 210
                Layout.preferredHeight: 44
                enabled: panel.hasDownloadController && !panel.modelTaskRunning
                model: [
                    qsTr("HF Mirror"),
                    qsTr("Hugging Face"),
                    qsTr("Sufy CDN"),
                    qsTr("Custom")
                ]
                currentIndex: panel.hasDownloadController
                              ? panel.endpointPresetIndex(panel.downloadController.modelEndpointPreset)
                              : 0
                onActivated: function(index) {
                    if (panel.hasDownloadController) {
                        panel.downloadController.SetModelEndpointPreset(panel.endpointPresetForIndex(index))
                    }
                }
            }

            TextField {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                visible: panel.hasDownloadController && panel.downloadController.modelEndpointPreset === "custom"
                enabled: panel.hasDownloadController && !panel.modelTaskRunning
                text: panel.hasDownloadController ? panel.downloadController.customModelEndpoint : ""
                placeholderText: qsTr("https://example.com")
                color: panel.textColor
                onEditingFinished: {
                    if (panel.hasDownloadController) {
                        panel.downloadController.SetCustomModelEndpoint(text)
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Button {
                Layout.preferredHeight: 42
                text: qsTr("Check")
                enabled: panel.hasDownloadController && !panel.modelTaskRunning
                Material.foreground: panel.textColor
                onClicked: panel.downloadController.RefreshSelectedModelStatus()
            }

            Button {
                Layout.preferredHeight: 42
                text: panel.modelDownloadRunning
                      ? qsTr("Cancel")
                      : qsTr("Download")
                enabled: panel.hasDownloadController && !panel.modelActivationRunning
                Material.foreground: panel.textColor
                onClicked: {
                    if (panel.modelDownloadRunning) {
                        panel.downloadController.CancelSelectedModelDownload()
                    } else {
                        panel.downloadController.StartSelectedModelDownload()
                    }
                }
            }

            Button {
                Layout.preferredHeight: 42
                text: qsTr("Delete")
                enabled: panel.hasDownloadController && !panel.modelTaskRunning
                Material.foreground: panel.dangerColor
                onClicked: panel.downloadController.DeleteSelectedModel()
            }

            Button {
                Layout.preferredHeight: 42
                text: qsTr("Activate")
                enabled: panel.hasController
                         && !panel.modelDownloadRunning
                         && !panel.modelActivationRunning
                Material.foreground: panel.textColor
                onClicked: panel.semanticController.ActivateSelectedModel()
            }

            Item { Layout.fillWidth: true }
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

            ComboBox {
                id: importBehaviorBox
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                model: [
                    qsTr("Always"),
                    qsTr("Always Ask"),
                    qsTr("Always Skip")
                ]
                currentIndex: panel.importPreferenceIndex(panel.importPreference)
                onActivated: function(index) {
                    panel.importPreferenceRequested(panel.preferenceForIndex(index))
                }
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

        implicitHeight: 84
        radius: 8
        color: Qt.rgba(panel.canvasColor.r, panel.canvasColor.g, panel.canvasColor.b, 0.62)

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: label
                color: panel.mutedTextColor
                font.pixelSize: 12
                font.weight: 700
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                text: value
                color: panel.textColor
                font.family: panel.dataFontFamily
                font.pixelSize: 18
                font.weight: 700
                elide: Text.ElideRight
            }
        }
    }

    component ModelStatusCard: Rectangle {
        id: card

        readonly property bool downloading: panel.modelDownloadRunning
        readonly property bool activating: panel.modelActivationRunning
        readonly property bool busy: card.downloading || card.activating
        readonly property bool isActive: panel.selectedModelActive
        readonly property bool isInstalled: panel.selectedModelInstalled
        // Green when the selected model is the active one; muted when merely
        // downloaded; danger red when not present locally. Accent while busy.
        readonly property color statusColor: card.busy
                                             ? panel.primaryAccent
                                             : (card.isActive
                                                ? "#5BB37A"
                                                : (card.isInstalled
                                                   ? panel.mutedTextColor
                                                   : panel.dangerColor))
        readonly property string statusText: {
            if (card.activating) {
                return qsTr("Activating…")
            }
            if (card.downloading) {
                return qsTr("Downloading %1%").arg(panel.modelDownloadProgress)
            }
            if (card.isActive) {
                return qsTr("Active")
            }
            if (card.isInstalled) {
                return qsTr("Installed · not active")
            }
            return qsTr("Not downloaded")
        }
        // Current file basename, shown while downloading so the card tells the
        // user *what* is coming down — replaces the old standalone status line.
        readonly property string currentFileName: {
            const f = panel.modelDownloadCurrentFile
            if (f.length === 0) {
                return ""
            }
            const slash = f.lastIndexOf("/")
            return slash >= 0 ? f.substring(slash + 1) : f
        }

        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 28
        radius: 10
        color: Qt.rgba(1, 1, 1, 0.08)
        border.width: 1
        border.color: Qt.rgba(card.statusColor.r, card.statusColor.g, card.statusColor.b, 0.35)

        ColumnLayout {
            id: inner
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 14
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.preferredWidth: 9
                    Layout.preferredHeight: 9
                    radius: 5
                    color: card.statusColor
                    opacity: card.busy ? dotPulse.opacity : 1.0
                }

                Label {
                    text: card.statusText
                    color: panel.textColor
                    font.pixelSize: 14
                    font.weight: 800
                }

                Item { Layout.fillWidth: true }

                // Total size on the right. Hidden while busy because the
                // progress row below already shows "done / total".
                Label {
                    visible: !card.busy && panel.selectedModelSizeLabel.length > 0
                    text: panel.selectedModelSizeLabel
                    color: panel.mutedTextColor
                    font.family: panel.dataFontFamily
                    font.pixelSize: 13
                    font.weight: 700
                }
            }

            Label {
                Layout.fillWidth: true
                visible: panel.selectedModelDisplayName.length > 0
                text: panel.selectedModelDisplayName
                color: panel.textColor
                font.pixelSize: 15
                font.weight: 800
                elide: Text.ElideRight
            }

            // Live progress: bar + byte count + current file. Shown only while
            // a download or activation is running.
            ColumnLayout {
                Layout.fillWidth: true
                visible: card.busy
                spacing: 5

                ProgressBar {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 8
                    from: 0
                    to: 100
                    value: panel.modelDownloadProgress
                    indeterminate: panel.modelDownloadIndeterminate
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: {
                            if (panel.modelActivationRunning) {
                                return qsTr("Preparing model runtime…")
                            }
                            return panel.modelDownloadBytesLabel
                        }
                        color: panel.mutedTextColor
                        font.family: panel.dataFontFamily
                        font.pixelSize: 12
                        font.weight: 600
                    }

                    Item { Layout.fillWidth: true }

                    Label {
                        visible: panel.modelDownloadFilesTotal > 0
                                 && !panel.modelActivationRunning
                        text: qsTr("File %1 / %2").arg(panel.modelDownloadFilesDone)
                              .arg(panel.modelDownloadFilesTotal)
                        color: panel.mutedTextColor
                        font.family: panel.dataFontFamily
                        font.pixelSize: 12
                        font.weight: 500
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    // Speed + ETA, shown only while actually downloading (not
                    // during activation). Both are pre-formatted in C++.
                    Label {
                        visible: card.downloading
                                 && panel.modelDownloadSpeedLabel.length > 0
                        text: panel.modelDownloadSpeedLabel
                        color: panel.mutedTextColor
                        font.family: panel.dataFontFamily
                        font.pixelSize: 12
                        font.weight: 600
                    }

                    Label {
                        visible: card.downloading
                                 && panel.modelDownloadSpeedLabel.length > 0
                                 && panel.modelDownloadEtaLabel.length > 0
                        text: "·"
                        color: panel.mutedTextColor
                        font.pixelSize: 12
                        font.weight: 500
                    }

                    Label {
                        visible: card.downloading
                                 && panel.modelDownloadEtaLabel.length > 0
                        text: panel.modelDownloadEtaLabel
                        color: panel.mutedTextColor
                        font.family: panel.dataFontFamily
                        font.pixelSize: 12
                        font.weight: 500
                    }

                    Item { Layout.fillWidth: true }
                }

                Label {
                    Layout.fillWidth: true
                    visible: card.downloading && card.currentFileName.length > 0
                    text: qsTr("↓ %1").arg(card.currentFileName)
                    color: panel.mutedTextColor
                    font.family: panel.dataFontFamily
                    font.pixelSize: 12
                    font.weight: 500
                    elide: Text.ElideMiddle
                }
            }

            // When idle, surface the controller's status text (errors, install
            // path, "not checked", etc.) as a single muted detail line so the
            // card remains the one place model state is reported.
            Label {
                Layout.fillWidth: true
                visible: !card.busy && panel.modelDownloadStatusDetail.length > 0
                text: panel.modelDownloadStatusDetail
                color: panel.mutedTextColor
                font.pixelSize: 12
                font.weight: 500
                wrapMode: Text.WordWrap
                lineHeight: 1.25
                elide: Text.ElideRight
            }
        }

        // Subtle pulse on the status dot while busy, so the card reads as
        // "live" even when a mirror stalls for a second or two.
        QtObject {
            id: dotPulse
            property real opacity: 1.0
            SequentialAnimation on opacity {
                running: card.busy
                loops: Animation.Infinite
                NumberAnimation { to: 0.35; duration: 700; easing.type: Easing.InOutSine }
                NumberAnimation { to: 1.0; duration: 700; easing.type: Easing.InOutSine }
            }
        }
    }
}
