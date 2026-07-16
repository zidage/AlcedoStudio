import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Effects

Dialog {
    id: root

    modal: true
    focus: visible
    padding: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    x: 0
    y: 0
    closePolicy: running ? Popup.NoAutoClose
                         : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    font.family: appTheme.uiFontFamily

    property Item blurSource: null
    property var analysisController: null
    property var profileController: null
    property var imageController: null
    property var selectionTargets: []
    property bool backendInteractive: false
    // Phase 2: the interaction-policy controller. The Start button binds to its
    // canRunAnalysis Q_PROPERTY so a running analysis on the selected images
    // blocks starting another, with a reason shown in the footer.
    property var interactionPolicy: null
    property real cornerRadius: 0

    // Phase 2: push the dialog's selected targets into the policy controller so
    // canRunAnalysis re-evaluates on PolicyChanged.
    Binding {
        target: root.interactionPolicy
        property: "pendingAnalysisTargets"
        value: root.selectionTargets
    }

    property color panelColor: appTheme.toneGraphite
    property color canvasColor: appTheme.bgDeepColor
    property color sectionColor: appTheme.bgBaseColor
    property color summaryCardColor: Qt.rgba(1, 1, 1, 0.04)
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color accentColor: appTheme.accentColor
    property color secondaryAccentColor: appTheme.accentSecondaryColor
    property color dangerColor: appTheme.dangerColor
    property color hoverColor: appTheme.hoverColor
    property color dividerColor: appTheme.dividerColor
    property color overlayColor: appTheme.overlayColor
    property string headlineFontFamily: appTheme.headlineFontFamily
    readonly property string dataFontFamily: appTheme.dataFontFamily

    readonly property bool running: analysisController && analysisController.running
    readonly property bool analysisLockedByPolicy: root.running && root.interactionPolicy
                                                 && !root.interactionPolicy.canRunAnalysis
    readonly property bool canRunInBackground: root.analysisLockedByPolicy
    readonly property int selectedImageCount: selectionTargets ? selectionTargets.length : 0
    readonly property bool ratingReasonSelected: ratingTask.checked && ratingReasonTask.checked
    readonly property int selectedTaskCount: (descriptionTask.checked ? 1 : 0)
                                           + (ratingTask.checked ? 1 : 0)
    readonly property int totalUnits: selectedTaskCount > 0 ? selectedImageCount : 0
    readonly property int controllerDone: analysisController
                                          ? Number(analysisController.analyzed)
                                            + Number(analysisController.failed)
                                            + Number(analysisController.canceled)
                                          : 0
    readonly property int completedUnits: Math.min(totalUnits, completedBefore + controllerDone)
    readonly property real progressValue: totalUnits > 0 ? completedUnits / totalUnits : 0
    readonly property string providerDisplay: profileController && profileController.activeDisplayName.length > 0
                                              ? profileController.activeDisplayName
                                              : qsTr("No provider selected")
    readonly property string modelDisplay: profileController && profileController.activeModelDisplayName.length > 0
                                           ? profileController.activeModelDisplayName
                                           : qsTr("No model selected")
    readonly property string outputLanguageDisplay: {
        const value = profileController ? String(profileController.outputLanguage) : "follow"
        if (value === "zh") {
            return qsTr("Chinese")
        }
        if (value === "en") {
            return qsTr("English")
        }
        return qsTr("Follow app language")
    }
    // 评价严苛程度 — bound to the controller's persisted Q_PROPERTY so the
    // segmented slider reflects (and writes) the rating severity persona.
    readonly property string severityCode: analysisController
                                           ? String(analysisController.ratingSeverity || "normal")
                                           : "normal"
    // UI language drives which degree name + flavor text is shown (中文界面显示中文
    // 程度，英文界面显示英文程度), mirroring the app's effective language, not the AI
    // output language. `languageManager` is a root-context property (main.cpp).
    readonly property bool uiIsChinese: {
        if (typeof languageManager === "undefined" || languageManager === null) {
            return false
        }
        return String(languageManager.effectiveLanguageCode || "").toLowerCase().indexOf("zh") === 0
    }
    readonly property string severityTitle: root.uiIsChinese
                                            ? qsTr("评价严苛程度") : qsTr("Rating strictness")
    readonly property var severityModel: [
        { code: "lite",   en: "Lite",   zh: "水",
          selectedColor: root.accentColor,
          flavorEn: "Generous scoring — ordinary photos default to 3–4 stars with mild reasons.",
          flavorZh: "宽容打分——普通照片默认 3–4 星，理由温和。" },
        { code: "normal", en: "Normal", zh: "普通",
          selectedColor: root.accentColor,
          flavorEn: "Balanced 1–5 star rating with a short rationale.",
          flavorZh: "正常评分，平衡的 1–5 星，简短理由。" },
        { code: "high",   en: "High",   zh: "大师",
          selectedColor: root.accentColor,
          flavorEn: "Strict but guiding — reads meaning, composition, narrative, expression, and completeness before technical trivia.",
          flavorZh: "严格但引导式——先看寓意、构图、叙事、表达和完整性，不拿曝光/模糊小题大做。" },
        { code: "xhigh",  en: "xHigh",  zh: "老法师",
          selectedColor: Qt.rgba(0.89, 0.72, 0.30, 1.0),
          flavorEn: "Old-school gear-and-parameter scrutiny with heavy taste policing and blunt practical advice.",
          flavorZh: "老法师标准——器材、参数、对比度、饱和度、虚化和“经验”都要拿出来说道说道。" },
        { code: "max",    en: "Max",    zh: "懂哥",
          selectedColor: Qt.rgba(0.65, 0.55, 0.98, 1.0),
          flavorEn: "Maximum gatekeeping — harsh, cynical, and impossible to please.",
          flavorZh: "懂哥模式——眼光挑剔，分数从严，评语里少不了那套居高临下的行话。" }
    ]
    readonly property string severityFlavor: {
        for (let i = 0; i < severityModel.length; ++i) {
            if (severityModel[i].code === severityCode) {
                return root.uiIsChinese ? severityModel[i].flavorZh : severityModel[i].flavorEn
            }
        }
        const fallback = severityModel[1]
        return root.uiIsChinese ? fallback.flavorZh : fallback.flavorEn
    }

    property int completedBefore: 0
    property int phaseIndex: -1
    property bool startedOnce: false
    property bool finalReady: false
    property bool cancelRequested: false
    property string finalSummary: ""
    property string finalFailureDetails: ""
    property string localError: ""
    property var phaseQueue: []
    property var phaseTargets: ({})
    property int skippedUnits: 0
    property real preservedContentY: 0
    property bool preservingContentY: false

    signal messageRequested(string message)

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    function analysisFlickable() {
        return analysisScroll && analysisScroll.contentItem
                && analysisScroll.contentItem.contentY !== undefined
                ? analysisScroll.contentItem : null
    }

    function beginContentYPreserve() {
        const flickable = analysisFlickable()
        if (!flickable) {
            return
        }
        preservedContentY = flickable.contentY
        preservingContentY = true
    }

    function restoreContentY() {
        const flickable = analysisFlickable()
        if (!flickable) {
            preservingContentY = false
            return
        }
        const maxY = Math.max(0, flickable.contentHeight - flickable.height)
        flickable.contentY = Math.max(0, Math.min(root.preservedContentY, maxY))
        preservingContentY = false
    }

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: root.cornerRadius
            color: "white"
            visible: false
            layer.enabled: true
            layer.smooth: true
        }

        Item {
            anchors.fill: parent
            layer.enabled: true
            layer.smooth: true
            layer.effect: MultiEffect {
                maskEnabled: root.cornerRadius > 0
                maskSource: backdropMask
            }

            MultiEffect {
                anchors.fill: parent
                source: root.blurSource
                blurEnabled: root.blurSource !== null
                blur: 0.64
                blurMax: 68
                saturation: -0.22
                brightness: -0.08
            }

            Rectangle {
                anchors.fill: parent
                color: root.overlayColor
            }
        }

        MouseArea { anchors.fill: parent; hoverEnabled: true }
    }

    function openWithTargets(targets) {
        if (root.running) {
            startedOnce = true
            open()
            return
        }
        selectionTargets = targets ? targets : []
        resetSession()
        open()
        Qt.callLater(function() {
            const flickable = root.analysisFlickable()
            if (flickable) {
                flickable.contentY = 0
                root.preservedContentY = 0
            }
        })
    }

    function openTaskDetails(task) {
        if (task && task.affectedTargets && task.affectedTargets.length > 0) {
            selectionTargets = task.affectedTargets
        }
        if (root.running) {
            startedOnce = true
        }
        open()
    }

    function resetSession() {
        completedBefore = skippedUnits
        phaseIndex = -1
        startedOnce = false
        finalReady = false
        cancelRequested = false
        finalSummary = ""
        finalFailureDetails = ""
        localError = ""
        phaseQueue = []
        phaseTargets = ({})
        skippedUnits = 0
    }

    function buildPhaseQueue() {
        const phases = []
        if (descriptionTask.checked && ratingTask.checked) {
            phases.push("analyze")
            return phases
        }
        if (descriptionTask.checked) {
            phases.push("describe")
        }
        if (ratingTask.checked) {
            phases.push("score")
        }
        return phases
    }

    function hasExistingDescription(target) {
        if (!imageController || !target) {
            return false
        }
        const result = imageController.GetImageDescription(Number(target.elementId))
        return result && result.hasDescription === true
    }

    function hasExistingRating(target) {
        if (!imageController || !target) {
            return false
        }
        const result = imageController.GetImageRating(Number(target.elementId), Number(target.imageId))
        return result && result.success === true && Number(result.rating) > 0
    }

    function hasExistingReason(target) {
        if (!imageController || !target) {
            return false
        }
        const result = imageController.GetImageRatingReasons(Number(target.elementId))
        return result && result.hasReasons === true
    }

    function filteredTargetsForPhase(phase) {
        const out = []
        const source = selectionTargets ? selectionTargets : []
        for (let i = 0; i < source.length; ++i) {
            const target = source[i]
            if ((phase === "describe" || phase === "analyze") && descriptionTask.checked
                    && !overwriteDescription.checked
                    && hasExistingDescription(target)) {
                continue
            }
            if (phase === "score" || phase === "analyze") {
                const skipRating = ratingTask.checked && !overwriteRating.checked
                                   && hasExistingRating(target)
                const skipReason = root.ratingReasonSelected && !overwriteReason.checked
                                   && hasExistingReason(target)
                if (skipRating || skipReason) {
                    continue
                }
            }
            out.push(target)
        }
        return out
    }

    function taskLabel(task) {
        if (task === "analyze") {
            return qsTr("Analysis")
        }
        return task === "describe" ? qsTr("Description") : qsTr("Rating")
    }

    function controllerError() {
        return analysisController ? String(analysisController.lastError || "") : ""
    }

    function failedResultDetail(row) {
        const error = String(row.error || "")
        if (error.length > 0) {
            return error
        }
        const details = []
        const providerStatus = Number(row.providerStatus || 0)
        const providerErrorCode = Number(row.providerErrorCode || 0)
        if (providerStatus !== 0) {
            details.push(qsTr("provider status %1").arg(providerStatus))
        }
        if (providerErrorCode !== 0) {
            details.push(qsTr("error code %1").arg(providerErrorCode))
        }
        const provider = String(row.provider || "")
        const modelId = String(row.modelId || "")
        const providerRequestId = String(row.providerRequestId || "")
        if (provider.length > 0) {
            details.push(qsTr("provider %1").arg(provider))
        }
        if (modelId.length > 0) {
            details.push(qsTr("model %1").arg(modelId))
        }
        if (providerRequestId.length > 0) {
            details.push(qsTr("request %1").arg(providerRequestId))
        }
        return details.length > 0 ? details.join(", ") : qsTr("No provider error message was returned.")
    }

    function failedResultSummary() {
        if (!analysisController || !analysisController.lastResults) {
            return ""
        }
        const parts = []
        const results = analysisController.lastResults
        for (let i = 0; i < results.length; ++i) {
            const row = results[i]
            const status = String(row.status || "")
            if (status === "analyzed") {
                continue
            }
            const label = row.fileName && String(row.fileName).length > 0
                    ? String(row.fileName)
                    : qsTr("Image %1").arg(Number(row.imageId || row.elementId || i + 1))
            parts.push(qsTr("%1: %2").arg(label).arg(failedResultDetail(row)))
        }
        return parts.join("\n")
    }

    function firstFailureLine(details) {
        const lines = String(details || "").split("\n")
        for (let i = 0; i < lines.length; ++i) {
            const line = lines[i].trim()
            if (line.length > 0) {
                return line
            }
        }
        return ""
    }

    function startAnalysis() {
        if (!analysisController || running) {
            return
        }
        if (!backendInteractive) {
            localError = qsTr("Open a project before running remote analysis.")
            return
        }
        if (selectedImageCount <= 0) {
            localError = qsTr("Select at least one image to analyze.")
            return
        }
        phaseQueue = buildPhaseQueue()
        if (phaseQueue.length === 0) {
            localError = qsTr("Choose at least one analysis task.")
            return
        }
        const nextPhaseTargets = ({})
        let nextSkippedUnits = 0
        for (let i = 0; i < phaseQueue.length; ++i) {
            const phase = phaseQueue[i]
            const targets = filteredTargetsForPhase(phase)
            nextPhaseTargets[phase] = targets
            nextSkippedUnits += Math.max(0, selectedImageCount - targets.length)
        }
        phaseTargets = nextPhaseTargets
        skippedUnits = nextSkippedUnits
        localError = ""
        startedOnce = true
        finalReady = false
        cancelRequested = false
        finalFailureDetails = ""
        completedBefore = 0
        phaseIndex = 0
        startCurrentPhase()
    }

    function startCurrentPhase() {
        if (!analysisController || phaseIndex < 0 || phaseIndex >= phaseQueue.length) {
            finishAllPhases()
            return
        }
        const phase = phaseQueue[phaseIndex]
        const targets = phaseTargets[phase] ? phaseTargets[phase] : []
        if (targets.length === 0) {
            advanceAfterControllerStopped()
            return
        }
        if (phase === "analyze") {
            analysisController.StartAnalyzeForTargets(targets, root.ratingReasonSelected)
        } else if (phase === "describe") {
            analysisController.StartDescribeForTargets(targets)
        } else {
            analysisController.StartScoreForTargets(targets, root.ratingReasonSelected)
        }
    }

    function advanceAfterControllerStopped() {
        if (!startedOnce || finalReady || phaseIndex < 0) {
            return
        }
        const error = controllerError()
        const canceled = analysisController && Number(analysisController.canceled) > 0
        if (cancelRequested || canceled || error.length > 0) {
            finishAllPhases()
            return
        }
        completedBefore += selectedImageCount
        if (phaseIndex + 1 < phaseQueue.length) {
            phaseIndex += 1
            startCurrentPhase()
        } else {
            finishAllPhases()
        }
    }

    function finishAllPhases() {
        beginContentYPreserve()
        finalReady = true
        const ok = analysisController ? Number(analysisController.analyzed) : 0
        const failed = analysisController ? Number(analysisController.failed) : 0
        const canceled = analysisController ? Number(analysisController.canceled) : 0
        const error = controllerError()
        const failureDetails = failedResultSummary()
        finalFailureDetails = ""
        if (cancelRequested || canceled > 0) {
            finalFailureDetails = failureDetails
            finalSummary = qsTr("Canceled. Successful results already saved remain in place.")
        } else if (error.length > 0) {
            finalFailureDetails = failureDetails.length > 0 ? failureDetails : error
            finalSummary = error
        } else if (failed > 0) {
            finalFailureDetails = failureDetails
            const firstFailure = firstFailureLine(failureDetails)
            finalSummary = firstFailure.length > 0
                    ? qsTr("Finished with %1 successful item(s) and %2 failed item(s). First failure: %3")
                        .arg(ok).arg(failed).arg(firstFailure)
                    : qsTr("Finished with %1 successful item(s) and %2 failed item(s).").arg(ok).arg(failed)
        } else if (skippedUnits > 0) {
            finalSummary = qsTr("Analysis complete. Skipped %1 existing image(s).").arg(skippedUnits)
        } else {
            finalSummary = qsTr("Analysis complete.")
        }
        phaseIndex = -1
        Qt.callLater(root.restoreContentY)
    }

    function cancelAnalysis() {
        cancelRequested = true
        if (analysisController && analysisController.running) {
            analysisController.CancelAnalysis()
        }
    }

    function runInBackground() {
        if (!canRunInBackground) {
            return
        }
        root.close()
    }

    onRunningChanged: {
        if (!running && startedOnce && !finalReady) {
            beginContentYPreserve()
            Qt.callLater(function() {
                root.advanceAfterControllerStopped()
                root.restoreContentY()
            })
        }
    }

    onOpened: {
        if (analysisController) {
            analysisController.RefreshCredentialState()
        }
    }

    Connections {
        target: root.analysisController
        ignoreUnknownSignals: true

        function onStateChanged() {
            if (root.startedOnce && !root.running) {
                Qt.callLater(root.advanceAfterControllerStopped)
            }
        }
    }

    background: Rectangle {
        radius: 0
        color: "transparent"
    }

    contentItem: Item {
        implicitWidth: root.width
        implicitHeight: root.height

        Rectangle {
            id: shell
            anchors.centerIn: parent
            width: Math.min(parent.width - 56, 860)
            height: Math.min(parent.height - 72, 680)
            radius: 14
            color: root.panelColor
            border.width: 0
            clip: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 96
                    color: root.withAlpha(root.canvasColor, 0.36)

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 28
                        anchors.rightMargin: 22
                        spacing: 16

                        Rectangle {
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            radius: 10
                            color: root.withAlpha(root.accentColor, 0.12)
                            border.width: 0

                            Image {
                                anchors.centerIn: parent
                                width: 24
                                height: 24
                                source: "qrc:/panel_icons/flask.svg"
                                sourceSize.width: 24
                                sourceSize.height: 24
                                opacity: 0.9
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Advanced Content Analysis")
                                color: root.textColor
                                font.family: root.headlineFontFamily
                                font.pixelSize: 28
                                font.weight: 800
                                elide: Text.ElideRight
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("%1 selected image(s)").arg(root.selectedImageCount)
                                color: root.mutedTextColor
                                font.family: root.dataFontFamily
                                font.pixelSize: 13
                                font.weight: 600
                                elide: Text.ElideRight
                            }
                        }

                        ToolButton {
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: 40
                            enabled: !root.running
                            visible: !root.running
                            text: "\u00d7"
                            font.pixelSize: 28
                            font.weight: 300
                            onClicked: root.close()
                            contentItem: Label {
                                text: parent.text
                                color: parent.enabled ? root.withAlpha(root.textColor, 0.82)
                                                      : root.withAlpha(root.mutedTextColor, 0.48)
                                font: parent.font
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 8
                                color: parent.down
                                       ? root.withAlpha(root.textColor, 0.08)
                                       : (parent.hovered ? root.hoverColor : "transparent")
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: root.dividerColor
                }

                ScrollView {
                    id: analysisScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true

                    Connections {
                        target: analysisScroll.contentItem
                        ignoreUnknownSignals: true
                        function onContentYChanged() {
                            if (!root.preservingContentY) {
                                root.preservedContentY = analysisScroll.contentItem.contentY
                            }
                        }
                    }

                    ColumnLayout {
                        width: analysisScroll.availableWidth
                        spacing: 18

                        GridLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 24
                            Layout.leftMargin: 28
                            Layout.rightMargin: 28
                            columns: width > 560 ? 3 : 1
                            columnSpacing: 12
                            rowSpacing: 12

                            SummaryTile { label: qsTr("Provider"); value: root.providerDisplay }
                            SummaryTile { label: qsTr("Model"); value: root.modelDisplay }
                            SummaryTile { label: qsTr("Output language"); value: root.outputLanguageDisplay }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.leftMargin: 28
                            Layout.rightMargin: 28
                            Layout.preferredHeight: taskColumn.implicitHeight + 28
                            radius: 8
                            color: root.sectionColor
                            border.width: 0

                            ColumnLayout {
                                id: taskColumn
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 12

                                Label {
                                    text: qsTr("Tasks")
                                    color: root.textColor
                                    font.pixelSize: 15
                                    font.weight: 800
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 14

                                    CheckBox { id: descriptionTask; text: qsTr("Description"); checked: true; enabled: !root.running; Material.foreground: root.textColor; Material.accent: root.accentColor }
                                    CheckBox {
                                        id: ratingTask
                                        text: qsTr("Rating")
                                        checked: true
                                        enabled: !root.running
                                        Material.foreground: root.textColor
                                        Material.accent: root.accentColor
                                        onCheckedChanged: {
                                            if (!checked) {
                                                ratingReasonTask.checked = false
                                            }
                                        }
                                    }
                                    CheckBox {
                                        id: ratingReasonTask
                                        text: qsTr("Rating reason")
                                        checked: true
                                        enabled: !root.running && ratingTask.checked
                                        Material.foreground: root.textColor
                                        Material.accent: root.accentColor
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.dividerColor }

                                Label {
                                    text: qsTr("Overwrite")
                                    color: root.mutedTextColor
                                    font.pixelSize: 12
                                    font.weight: 700
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 14

                                    CheckBox { id: overwriteRating; text: qsTr("Overwrite photo rating"); checked: true; enabled: !root.running; Material.foreground: root.textColor; Material.accent: root.accentColor }
                                    CheckBox { id: overwriteReason; text: qsTr("Overwrite rating reason"); checked: true; enabled: !root.running && root.ratingReasonSelected; Material.foreground: root.textColor; Material.accent: root.accentColor }
                                    CheckBox { id: overwriteDescription; text: qsTr("Overwrite image description"); checked: true; enabled: !root.running; Material.foreground: root.textColor; Material.accent: root.accentColor }
                                }
                            }
                        }

                        Rectangle {
                            // 评价严苛程度 — only relevant when a rating task is
                            // selected. Hidden while running (the persona is fixed
                            // for the in-flight job).
                            id: severityCard
                            Layout.fillWidth: true
                            Layout.leftMargin: 28
                            Layout.rightMargin: 28
                            visible: ratingTask.checked && !root.running && !root.startedOnce
                            Layout.preferredHeight: severityColumn.implicitHeight + 28
                            radius: 8
                            color: root.sectionColor
                            border.width: 0

                            ColumnLayout {
                                id: severityColumn
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 10

                                Label {
                                    text: root.severityTitle
                                    color: root.textColor
                                    font.pixelSize: 15
                                    font.weight: 800
                                }

                                SeveritySegmentedSlider {
                                    Layout.fillWidth: true
                                    options: root.severityModel
                                    currentCode: root.severityCode
                                    useChineseLabels: root.uiIsChinese
                                    textColor: root.textColor
                                    mutedTextColor: root.mutedTextColor
                                    accentColor: root.accentColor
                                    trackColor: root.withAlpha(root.textColor, 0.30)
                                    hoverColor: root.withAlpha(root.textColor, 0.07)
                                    dividerColor: root.withAlpha(root.textColor, 0.16)
                                    enabled: root.analysisController !== null && !root.running
                                    onSelected: function(code) {
                                        if (root.analysisController) {
                                            root.analysisController.SetRatingSeverity(code)
                                        }
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: root.severityFlavor
                                    color: root.mutedTextColor
                                    font.pixelSize: 12
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.leftMargin: 28
                            Layout.rightMargin: 28
                            Layout.preferredHeight: 190
                            radius: 8
                            color: root.sectionColor
                            border.width: 0

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 22

                                ImportProgressRing {
                                    Layout.preferredWidth: 118
                                    Layout.preferredHeight: 118
                                    ringWidth: 11
                                    progress: root.progressValue
                                    indeterminate: root.running && root.totalUnits <= 0
                                    fillColor: root.accentColor
                                    trackColor: root.hoverColor
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Label {
                                        Layout.fillWidth: true
                                        text: root.running && root.phaseIndex >= 0
                                              ? qsTr("Running %1").arg(root.taskLabel(root.phaseQueue[root.phaseIndex]))
                                              : (root.finalReady ? qsTr("Finished") : qsTr("Ready"))
                                        color: root.textColor
                                        font.pixelSize: 20
                                        font.weight: 800
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("%1 / %2 image(s)").arg(root.completedUnits).arg(root.totalUnits)
                                        color: root.mutedTextColor
                                        font.family: root.dataFontFamily
                                        font.pixelSize: 14
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: root.localError.length > 0
                                              ? root.localError
                                              : (root.finalReady
                                                 ? root.finalSummary
                                                 : (root.analysisController ? root.analysisController.statusText : ""))
                                        color: root.localError.length > 0 || root.controllerError().length > 0
                                               || root.finalFailureDetails.length > 0
                                               ? root.dangerColor
                                               : root.mutedTextColor
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: 13
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: root.analysisController && Number(root.analysisController.lastUsage.totalTokens || 0) > 0
                                        text: qsTr("Usage: %1 token(s)").arg(root.analysisController
                                                                             ? Number(root.analysisController.lastUsage.totalTokens || 0)
                                                                             : 0)
                                        color: root.mutedTextColor
                                        font.pixelSize: 12
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.leftMargin: 28
                            Layout.rightMargin: 28
                            Layout.preferredHeight: Math.min(failureContent.implicitHeight + 28, 220)
                            visible: root.finalReady && root.finalFailureDetails.length > 0
                            radius: 8
                            color: root.withAlpha(root.dangerColor, 0.10)
                            border.width: 1
                            border.color: root.withAlpha(root.dangerColor, 0.26)
                            clip: true

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 8

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Failure details")
                                    color: root.dangerColor
                                    font.pixelSize: 13
                                    font.weight: 800
                                    elide: Text.ElideRight
                                }

                                ScrollView {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    clip: true
                                    contentWidth: availableWidth

                                    Label {
                                        id: failureContent
                                        width: parent.width
                                        text: root.finalFailureDetails
                                        color: root.textColor
                                        font.family: root.dataFontFamily
                                        font.pixelSize: 12
                                        lineHeight: 1.18
                                        wrapMode: Text.WrapAnywhere
                                    }
                                }
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.leftMargin: 28
                            Layout.rightMargin: 28
                            Layout.preferredHeight: hintText.implicitHeight + 24
                            radius: 8
                            color: root.summaryCardColor
                            border.width: 0

                            Label {
                                id: hintText
                                anchors.fill: parent
                                anchors.margins: 12
                                text: qsTr("Results refresh the focused photo's Image inspector. Open the Image page to review and edit description, rating, and reasons.")
                                color: root.textColor
                                wrapMode: Text.WordWrap
                                font.pixelSize: 13
                            }
                        }

                        Item { Layout.preferredHeight: 8 }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 82
                    color: root.withAlpha(root.canvasColor, 0.34)
                    border.width: 0

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 28
                        anchors.rightMargin: 28
                        spacing: 12

                        Label {
                            Layout.fillWidth: true
                            text: root.running
                                  ? (root.canRunInBackground
                                     ? qsTr("Protected by interaction locks; safe to continue in the background.")
                                     : qsTr("Remote provider calls may incur cost."))
                                  : (root.interactionPolicy && !root.interactionPolicy.canRunAnalysis
                                       ? root.interactionPolicy.runAnalysisReason : "")
                            color: root.mutedTextColor
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }

                        IconButton {
                            kind: "normal"
                            buttonWidth: 52
                            buttonHeight: 46
                            buttonRadius: 10
                            iconSize: 18
                            bordered: false
                            iconSrc: "qrc:/panel_icons/to_bg.svg"
                            tooltipText: qsTr("Move task to background")
                            enabled: root.canRunInBackground
                            onClicked: root.runInBackground()
                        }

                        DialogActionButton {
                            kind: "accent"
                            text: qsTr("Analyze Selected")
                            visible: !root.running && !root.finalReady
                            enabled: root.backendInteractive && root.selectedImageCount > 0
                                      && (!root.interactionPolicy
                                          || root.interactionPolicy.canRunAnalysis)
                            onClicked: root.startAnalysis()
                        }

                        DialogActionButton {
                            kind: "warning"
                            text: qsTr("Cancel")
                            visible: root.running
                            enabled: root.running
                            onClicked: root.cancelAnalysis()
                        }

                        DialogActionButton {
                            kind: "normal"
                            text: qsTr("Done")
                            visible: !root.running && root.finalReady
                            onClicked: root.close()
                        }
                    }
                }
            }
        }
    }

    component SummaryTile: Rectangle {
        property string label: ""
        property string value: ""

        Layout.fillWidth: true
        Layout.preferredHeight: 76
        radius: 8
        color: root.summaryCardColor
        border.width: 0

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: label
                color: root.mutedTextColor
                font.pixelSize: 11
                font.weight: 700
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                text: value
                color: root.textColor
                font.pixelSize: 15
                font.weight: 700
                elide: Text.ElideRight
            }
        }
    }
}
