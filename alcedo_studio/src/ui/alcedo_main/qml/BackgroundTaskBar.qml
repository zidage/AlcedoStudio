//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Compact background-task summary bar for the main-window chrome. Phase 1
// mirrors running image-analysis / semantic-generation / model-download tasks
// from `appModules.backgroundTasks`. It collapses to zero height
// when no tasks are registered, shows the primary running task title + progress
// when one is active, a "+N" count badge when several are active, and a Cancel
// button for the primary task. Click opens `BackgroundTaskPopover`.
Item {
    id: root

    readonly property var controller: appModules ? appModules.backgroundTasks : null
    readonly property var primary: controller && controller.primaryTask ? controller.primaryTask : ({})
    readonly property int runningCount: controller ? controller.runningCount : 0
    readonly property bool hasTasks: controller && controller.tasks.length > 0

    signal taskDetailsRequested(var task)

    visible: hasTasks
    Layout.fillWidth: true
    Layout.preferredHeight: hasTasks ? 38 : 0

    onHasTasksChanged: {
        if (!hasTasks) {
            popover.close()
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: appTheme.panelRadius
        color: appTheme.glassPanelColor
        border.width: 1
        border.color: appTheme.glassStrokeColor

        MouseArea {
            anchors.fill: parent
            onClicked: popover.open()
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 8
            spacing: 10

            Rectangle {
                Layout.preferredWidth: 10
                Layout.preferredHeight: 10
                radius: 5
                color: root.dotColor(root.primary && root.primary.state ? root.primary.state : "")
            }

            Label {
                // Prefix the primary task title with its kind so the bar itself
                // disambiguates concurrent tasks (e.g. "AI Analysis · Analyzing..."
                // vs "Semantic Labels · Generating..."), matching the popover badge.
                text: {
                    if (!(root.primary && root.primary.title)) return qsTr("Background tasks")
                    const kl = root.kindLabel(root.primary.kind)
                    return kl.length > 0 ? (kl + " · " + root.primary.title) : root.primary.title
                }
                color: appTheme.textColor
                elide: Text.ElideRight
                font.family: appTheme.uiFontFamily
                Layout.fillWidth: true
            }

            ProgressBar {
                visible: root.primary && root.primary.state === "running"
                Layout.preferredWidth: 140
                Layout.preferredHeight: 8
                from: 0
                to: 100
                value: root.primary && root.primary.progressPercent !== undefined
                      ? root.primary.progressPercent : 0
                indeterminate: root.primary && root.primary.progressPercent < 0
            }

            Rectangle {
                visible: root.runningCount > 1
                Layout.preferredWidth: countLabel.implicitWidth + 14
                Layout.preferredHeight: 22
                radius: 11
                color: appTheme.hoverColor
                Label {
                    id: countLabel
                    anchors.centerIn: parent
                    text: "+" + (root.runningCount - 1)
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: 11
                }
            }

            Button {
                visible: root.primary.cancelable === true
                        && root.primary.state === "running"
                text: qsTr("Cancel")
                flat: true
                Layout.preferredHeight: 28
                Material.foreground: appTheme.dangerColor
                onClicked: {
                    if (root.primary && root.primary.id) {
                        root.controller.CancelTask(root.primary.id)
                    }
                }
            }
        }
    }

    function dotColor(state) {
        if (state === "running" || state === "canceling") return appTheme.accentColor
        if (state === "failed") return appTheme.dangerColor
        if (state === "succeeded" || state === "canceled") return appTheme.textMutedColor
        return appTheme.hoverColor
    }

    function kindLabel(kind) {
        if (kind === "imageAnalysis") return qsTr("AI Analysis")
        if (kind === "semanticGeneration") return qsTr("Semantic Labels")
        if (kind === "modelActivation") return qsTr("Model Activation")
        if (kind === "modelDownload") return qsTr("Model Download")
        if (kind === "import") return qsTr("Import")
        if (kind === "export") return qsTr("Export")
        return ""
    }

    BackgroundTaskPopover {
        id: popover
        bar: root
        onTaskDetailsRequested: function(task) {
            root.taskDetailsRequested(task)
        }
    }
}
