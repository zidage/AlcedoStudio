//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Transient summary of the primary background task. A task state transition
// reveals the bar for three seconds; the bar then folds down out of the main
// layout. Detailed task history lives in BackgroundTasksDialog.
Item {
    id: root
    objectName: "backgroundTaskBar"

    readonly property var controller: appModules ? appModules.backgroundTasks : null
    readonly property var primary: controller && controller.primaryTask
                                   ? controller.primaryTask : ({})
    readonly property int runningCount: controller ? controller.runningCount : 0
    readonly property bool hasTasks: controller && controller.tasks.length > 0
    readonly property int barHeight: appTheme.iconButtonHitSizeCompact

    property real foldProgress: 0
    property bool layoutActive: false
    property string statusFingerprint: ""
    property bool motionArmed: false

    visible: layoutActive
    Layout.fillWidth: true
    Layout.preferredHeight: layoutActive ? barHeight : 0

    function taskStateFingerprint() {
        const tasks = controller ? controller.tasks : []
        const states = []
        for (let i = 0; i < tasks.length; ++i) {
            const task = tasks[i]
            states.push(String(task.id || "") + ":" + String(task.state || ""))
        }
        return states.join("|")
    }

    function revealTemporarily() {
        collapseCompletionTimer.stop()
        if (!hasTasks) {
            autoCollapseTimer.stop()
            foldProgress = 0
            collapseCompletionTimer.restart()
            return
        }
        layoutActive = true
        foldProgress = 1
        autoCollapseTimer.restart()
    }

    function stateColor(state) {
        if (state === "failed")
            return appTheme.backgroundTaskFailedColor
        if (state === "succeeded" || state === "canceled")
            return appTheme.backgroundTaskFinishedColor
        return appTheme.backgroundTaskWorkingColor
    }

    function kindLabel(kind) {
        if (kind === "imageAnalysis") return qsTr("AI Analysis")
        if (kind === "semanticGeneration") return qsTr("Semantic Labels")
        if (kind === "modelActivation") return qsTr("Model Activation")
        if (kind === "modelDownload") return qsTr("Model Download")
        if (kind === "editorSave") return qsTr("Editor Save")
        if (kind === "import") return qsTr("Import")
        if (kind === "export") return qsTr("Export")
        return qsTr("Background Tasks")
    }

    function primaryLabel() {
        if (!(primary && primary.title))
            return qsTr("Background Tasks")
        const kind = kindLabel(primary.kind)
        if (primary.state === "failed" && primary.detail)
            return qsTr("%1 · %2").arg(kind).arg(primary.detail)
        return qsTr("%1 · %2").arg(kind).arg(primary.title)
    }

    Connections {
        target: root.controller
        ignoreUnknownSignals: true

        function onTasksChanged() {
            const nextFingerprint = root.taskStateFingerprint()
            if (nextFingerprint !== root.statusFingerprint) {
                root.statusFingerprint = nextFingerprint
                root.revealTemporarily()
            }
        }
    }

    Timer {
        id: autoCollapseTimer
        interval: appTheme.backgroundTaskAutoCollapseMs
        onTriggered: {
            root.foldProgress = 0
            collapseCompletionTimer.restart()
        }
    }

    Timer {
        id: collapseCompletionTimer
        interval: appTheme.reduceMotion ? 0 : appTheme.motionFoldOpenMs
        onTriggered: {
            if (root.foldProgress < 0.001)
                root.layoutActive = false
        }
    }

    Behavior on foldProgress {
        enabled: root.motionArmed
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : appTheme.motionFoldOpenMs
            easing.type: Easing.OutCubic
        }
    }

    Component.onCompleted: {
        statusFingerprint = taskStateFingerprint()
        motionArmed = true
        if (hasTasks)
            revealTemporarily()
    }

    Item {
        anchors.fill: parent
        clip: true

        Rectangle {
            objectName: "backgroundTaskBarSurface"
            width: parent.width
            height: root.barHeight
            y: (1.0 - root.foldProgress) * root.barHeight
            radius: appTheme.panelRadius
            color: appTheme.cardSurfaceColor
            border.width: 1
            border.color: appTheme.cardBorderColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceMd
                anchors.rightMargin: appTheme.spaceSm
                spacing: appTheme.spaceSm

                Rectangle {
                    Layout.preferredWidth: appTheme.spaceMd
                    Layout.preferredHeight: appTheme.spaceMd
                    radius: appTheme.badgeRadius
                    color: root.stateColor(root.primary.state || "")
                    Accessible.ignored: true
                }

                Label {
                    Layout.fillWidth: true
                    text: root.primaryLabel()
                    color: appTheme.textColor
                    elide: Text.ElideRight
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
                }

                ProgressBar {
                    visible: root.primary
                             && (root.primary.state === "running"
                                 || root.primary.state === "canceling")
                    Layout.preferredWidth: appTheme.editorSidePanelWidthMin / 2
                    Layout.preferredHeight: appTheme.spaceSm
                    from: 0
                    to: 100
                    value: root.primary && root.primary.progressPercent !== undefined
                           ? root.primary.progressPercent : 0
                    indeterminate: root.primary && root.primary.progressPercent < 0
                }

                Label {
                    visible: root.runningCount > 1
                    text: qsTr("+%1").arg(root.runningCount - 1)
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }
        }
    }
}
