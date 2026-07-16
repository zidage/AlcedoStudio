//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Popover listing all active + recent background tasks from
// `appModules.backgroundTasks`. Opened by `BackgroundTaskBar`.
// Each row shows a status dot, title, detail, a progress bar, and a Cancel
// button when the task is still cancelable. Non-modal; closes on
// Escape/outside-click.
Popup {
    id: root

    property Item bar

    readonly property var controller: bar && bar.controller ? bar.controller : null

    signal taskDetailsRequested(var task)

    parent: Overlay.overlay
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: 420
    padding: 0

    // Bottom-right of the window, just above the task bar.
    x: parent ? Math.round(parent.width - root.width - 12) : 12
    y: parent ? Math.round(parent.height - root.height - 56) : 12

    background: Rectangle {
        radius: 10
        color: appTheme.bgDeepColor
        border.width: 1
        border.color: appTheme.glassStrokeColor
    }

    contentItem: ColumnLayout {
        spacing: 0

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 12
            Layout.bottomMargin: 8
            text: qsTr("Background tasks")
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.bold: true
        }

        ListView {
            id: taskList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 420)
            Layout.bottomMargin: 8
            clip: true
            interactive: contentHeight > 420
            model: root.controller ? root.controller.tasks : []
            delegate: ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 10
                    Layout.rightMargin: 10
                    Layout.preferredHeight: 1
                    color: appTheme.dividerColor
                }

                RowLayout {
                    Layout.leftMargin: 14
                    Layout.rightMargin: 14
                    spacing: 8

                    Rectangle {
                        Layout.preferredWidth: 9
                        Layout.preferredHeight: 9
                        radius: 4
                        color: root.dotColor(modelData.state)
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            // Kind badge so concurrent tasks are distinguishable at a
                            // glance — e.g. "AI ANALYSIS" vs "SEMANTIC LABELS" vs "MODEL
                            // ACTIVATION". Without this, two "24 image(s)" tasks in the
                            // popover are easy to mix up, and a failed semantic task gets
                            // misread as a failed AI-analysis task.
                            Layout.fillWidth: true
                            visible: root.kindLabel(modelData.kind).length > 0
                            text: root.kindLabel(modelData.kind)
                            color: appTheme.textMutedColor
                            elide: Text.ElideRight
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: 10
                            font.capitalization: Font.AllUppercase
                            font.bold: true
                        }

                        Label {
                            Layout.fillWidth: true
                            text: modelData.title ? modelData.title : qsTr("Task")
                            color: appTheme.textColor
                            elide: Text.ElideRight
                            font.family: appTheme.uiFontFamily
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: modelData.detail && modelData.detail.length > 0
                            text: modelData.detail
                            color: appTheme.textMutedColor
                            elide: Text.ElideRight
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: 11
                        }
                    }

                    Button {
                        visible: modelData.kind === "imageAnalysis"
                                && (modelData.state === "running" || modelData.state === "canceling")
                        text: qsTr("Details")
                        flat: true
                        Material.foreground: appTheme.textColor
                        onClicked: {
                            root.taskDetailsRequested(modelData)
                            root.close()
                        }
                    }

                    Button {
                        visible: modelData.cancelable === true
                                && (modelData.state === "running" || modelData.state === "queued")
                        text: qsTr("Cancel")
                        flat: true
                        Material.foreground: appTheme.dangerColor
                        onClicked: root.controller.CancelTask(modelData.id)
                    }
                }

                ProgressBar {
                    Layout.fillWidth: true
                    Layout.leftMargin: 14
                    Layout.rightMargin: 14
                    Layout.topMargin: 2
                    Layout.bottomMargin: 8
                    Layout.preferredHeight: 6
                    from: 0
                    to: 100
                    value: modelData.progressPercent !== undefined ? modelData.progressPercent : 0
                    indeterminate: modelData.progressPercent < 0
                    visible: modelData.state === "running" || modelData.state === "canceling"
                }
            }
        }
    }

    function dotColor(state) {
        if (state === "running" || state === "canceling") return appTheme.accentColor
        if (state === "failed") return appTheme.dangerColor
        if (state === "succeeded") return appTheme.accentSecondaryColor
        if (state === "canceled") return appTheme.textMutedColor
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
}
