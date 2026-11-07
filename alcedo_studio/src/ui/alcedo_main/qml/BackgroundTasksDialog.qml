pragma ComponentBehavior: Bound

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts

// Centered modal listing background work. Layout matches the merge dialog
// family: card surface, square X close, sunken list well, status lamp dots, and
// quiet row actions — not full-height DialogActionButton chrome.
//
// Pinned tasks (agent sessions) lead the scrolling list as AgentSessionCard and
// stay until the user dismisses them. The other tasks follow them in the same
// list; the registry keeps only the newest finished ones.
Dialog {
    id: root
    objectName: "backgroundTasksDialog"

    property var controller: null
    property Item blurSource: null
    property real cornerRadius: 0

    readonly property int taskCount: taskModel.count + agentModel.count
    readonly property int runningCount: controller ? controller.runningCount : 0
    // Group captions only help when both kinds share the list.
    readonly property bool showSectionLabels: agentModel.count > 0 && taskModel.count > 0
    // Medium dialog: wider than a side panel, far narrower than the merge resolver.
    readonly property int preferredWidth: appTheme.editorSidePanelWidth * 2
    readonly property int preferredHeight: appTheme.editorSidePanelWidthMax
                                           + appTheme.editorSidePanelWidthMin / 2

    parent: Overlay.overlay
    modal: true
    focus: true
    title: ""
    padding: 0
    closePolicy: Popup.CloseOnEscape
    width: Math.min(parent ? parent.width - appTheme.spaceXl * 2 : preferredWidth,
                    preferredWidth)
    height: Math.min(parent ? parent.height - appTheme.spaceXl * 2 : preferredHeight,
                     preferredHeight)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    footer: Item {
        width: 1
        height: 0
    }

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    function taskIndex(model, taskId, startIndex) {
        for (let index = startIndex; index < model.count; ++index) {
            const entry = model.get(index)
            if (entry.task && String(entry.task.id) === taskId)
                return index
        }
        return -1
    }

    function syncModel(model, source) {
        for (let targetIndex = 0; targetIndex < source.length; ++targetIndex) {
            const sourceTask = source[targetIndex]
            const sourceId = String(sourceTask.id || "")
            const existingIndex = taskIndex(model, sourceId, targetIndex)
            if (existingIndex < 0) {
                model.insert(targetIndex, { task: sourceTask })
            } else if (existingIndex !== targetIndex) {
                model.move(existingIndex, targetIndex, 1)
            }
            model.setProperty(targetIndex, "task", sourceTask)
        }
        if (model.count > source.length)
            model.remove(source.length, model.count - source.length)
    }

    function syncTasks() {
        const source = controller ? controller.tasks : []
        const pinned = []
        const others = []
        for (let index = 0; index < source.length; ++index) {
            if (source[index].pinned === true)
                pinned.push(source[index])
            else
                others.push(source[index])
        }
        // Newest agent session first.
        pinned.reverse()
        syncModel(agentModel, pinned)
        syncModel(taskModel, others)
    }

    function statusLabel(state) {
        if (state === "failed")
            return qsTr("Failed")
        if (state === "running" || state === "queued" || state === "canceling")
            return qsTr("Working")
        return qsTr("Finished")
    }

    function statusColor(state) {
        if (state === "failed")
            return appTheme.backgroundTaskFailedColor
        if (state === "running" || state === "queued" || state === "canceling")
            return appTheme.backgroundTaskWorkingColor
        return appTheme.backgroundTaskFinishedColor
    }

    function kindLabel(kind) {
        if (kind === "semanticGeneration") return qsTr("Semantic Labels")
        if (kind === "modelActivation") return qsTr("Model Activation")
        if (kind === "modelDownload") return qsTr("Model Download")
        if (kind === "editorSave") return qsTr("Editor Save")
        if (kind === "import") return qsTr("Import")
        if (kind === "export") return qsTr("Export")
        if (kind === "adjustmentPaste") return qsTr("Paste Adjustments")
        if (kind === "ratingUpdate") return qsTr("Ratings")
        if (kind === "imageDelete") return qsTr("Delete Images")
        if (kind === "agentControl") return qsTr("Agent")
        return qsTr("Task")
    }

    function summaryText() {
        if (taskCount === 0)
            return qsTr("No active work")
        if (runningCount > 0)
            return qsTr("%1 total · %2 working").arg(taskCount).arg(runningCount)
        return qsTr("%1 total").arg(taskCount)
    }

    function progressText(task) {
        if (!task)
            return ""
        const percent = task.progressPercent
        if (percent === undefined || percent < 0)
            return ""
        return qsTr("%1%").arg(Math.round(percent))
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.cardBorderColor
    }

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: root.cornerRadius
            color: appTheme.textColor
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
                objectName: "backgroundTasksDialogBackdrop"
                anchors.fill: parent
                source: root.blurSource
                blurEnabled: root.blurSource !== null
                blur: 0.72
                blurMax: 72
                saturation: -0.24
                brightness: -0.08
            }

            Rectangle {
                anchors.fill: parent
                color: appTheme.overlayColor
            }
        }
    }

    ListModel {
        id: agentModel
    }

    ListModel {
        id: taskModel
    }

    Connections {
        target: root.controller
        ignoreUnknownSignals: true

        function onTasksChanged() {
            root.syncTasks()
        }
    }

    onControllerChanged: syncTasks()
    Component.onCompleted: syncTasks()

    contentItem: ColumnLayout {
        spacing: 0

        // Title + summary + ghost Close on one center axis.
        RowLayout {
            id: headerRow
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.topMargin: appTheme.spaceLg
            Layout.bottomMargin: appTheme.spaceMd
            spacing: appTheme.spaceMd

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    objectName: "backgroundTasksDialogTitle"
                    Layout.fillWidth: true
                    text: qsTr("Background Tasks")
                    color: appTheme.textColor
                    wrapMode: Text.Wrap
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeHeadline
                    font.weight: appTheme.fontWeightHeading
                }

                Label {
                    Layout.fillWidth: true
                    text: root.summaryText()
                    color: appTheme.textMutedColor
                    wrapMode: Text.Wrap
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightRegular
                }
            }

            IconActionButton {
                id: closeButton
                objectName: "backgroundTasksDialogCloseButton"
                Layout.alignment: Qt.AlignVCenter
                compact: true
                iconSrc: "qrc:/panel_icons/close.svg"
                iconColorDefault: appTheme.iconColor
                iconColorMuted: appTheme.textMutedColor
                fillIdle: appTheme.cardSurfaceColor
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                actionName: qsTr("Close")
                onClicked: root.close()
            }
        }

        // Sunken list well — rows sit inside the track, not as floating cards.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.bottomMargin: appTheme.spaceLg
            radius: appTheme.controlRadiusSmall
            color: appTheme.bgBaseColor
            border.width: 1
            border.color: appTheme.cardBorderColor
            clip: true

            // One scroll area for agent cards and task rows, so any number of
            // agent sessions stays inside the dialog.
            Flickable {
                id: taskList
                objectName: "backgroundTasksList"
                anchors.fill: parent
                anchors.margins: appTheme.spaceSm
                clip: true
                contentWidth: width
                contentHeight: taskColumn.height
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    policy: taskList.contentHeight > taskList.height
                            ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                    contentItem: Rectangle {
                        implicitWidth: appTheme.spaceXs + 1
                        radius: appTheme.badgeRadius
                        color: root.withAlpha(appTheme.textMutedColor, 0.45)
                    }
                    background: Item {}
                }

                Column {
                    id: taskColumn
                    width: taskList.width
                    spacing: appTheme.spaceSm

                    Label {
                        objectName: "backgroundTasksAgentSectionLabel"
                        width: taskColumn.width
                        visible: root.showSectionLabels
                        text: qsTr("Agent Sessions")
                        color: appTheme.textMutedColor
                        elide: Text.ElideRight
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    Repeater {
                        objectName: "backgroundTasksPinnedAgents"
                        model: agentModel

                        delegate: AgentSessionCard {
                            width: taskColumn.width
                            controller: root.controller
                        }
                    }

                    Label {
                        objectName: "backgroundTasksOtherSectionLabel"
                        width: taskColumn.width
                        visible: root.showSectionLabels
                        topPadding: appTheme.spaceSm
                        text: qsTr("Other Tasks")
                        color: appTheme.textMutedColor
                        elide: Text.ElideRight
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    Repeater {
                        model: taskModel

                        delegate: Rectangle {
                            id: taskRow
                            required property var task

                            readonly property color rowStatusColor: root.statusColor(task ? task.state : "")
                            readonly property bool rowFailed: !!task && task.state === "failed"
                            readonly property bool rowActive: !!task
                                                              && (task.state === "running"
                                                                  || task.state === "queued"
                                                                  || task.state === "canceling")
                            // A known percentage shows as a bar; the turning outline shows work in progress.
                            readonly property bool showProgress: rowActive
                                                                 && task.progressPercent !== undefined
                                                                 && task.progressPercent >= 0
                            readonly property bool showCancel: !!task && task.cancelable === true
                                                               && (task.state === "running"
                                                                   || task.state === "queued")
                            readonly property string progressLabel: root.progressText(task)

                            width: taskColumn.width
                            height: taskContent.implicitHeight + appTheme.spaceMd * 2
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.cardSurfaceColor
                            border.width: rowActive ? 0 : 1
                            border.color: appTheme.cardBorderColor

                            RotatingTaskBorder {
                                anchors.fill: parent
                                radius: taskRow.radius
                                visible: taskRow.rowActive
                                running: visible
                                color: taskRow.rowStatusColor
                            }

                            RowLayout {
                                id: taskContent
                                anchors.fill: parent
                                anchors.leftMargin: appTheme.spaceMd
                                anchors.rightMargin: appTheme.spaceMd
                                anchors.topMargin: appTheme.spaceMd
                                anchors.bottomMargin: appTheme.spaceMd
                                spacing: appTheme.spaceMd

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: appTheme.spaceSm

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: appTheme.spaceSm

                                        // Status lamp only — no pill chrome (matches BackgroundTaskBar).
                                        Rectangle {
                                            Layout.alignment: Qt.AlignVCenter
                                            Layout.preferredWidth: appTheme.spaceMd
                                            Layout.preferredHeight: appTheme.spaceMd
                                            radius: appTheme.badgeRadius
                                            color: taskRow.rowStatusColor
                                            Accessible.ignored: true
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: (taskRow.task && taskRow.task.title)
                                                  || root.kindLabel(taskRow.task ? taskRow.task.kind : "")
                                            color: appTheme.textColor
                                            wrapMode: Text.Wrap
                                            font.family: appTheme.uiFontFamily
                                            font.pixelSize: appTheme.fontSizeTitle
                                            font.weight: appTheme.fontWeightStrong
                                        }

                                        Label {
                                            objectName: "backgroundTaskStatusLabel"
                                            text: root.statusLabel(taskRow.task ? taskRow.task.state : "")
                                            color: appTheme.textMutedColor
                                            font.family: appTheme.uiFontFamily
                                            font.pixelSize: appTheme.fontSizeCaption
                                            font.weight: appTheme.fontWeightStrong
                                        }

                                        Label {
                                            visible: taskRow.progressLabel.length > 0
                                                     && taskRow.showProgress
                                            text: taskRow.progressLabel
                                            color: appTheme.textMutedColor
                                            font.family: appTheme.dataFontFamily
                                            font.pixelSize: appTheme.fontSizeCaption
                                            font.weight: appTheme.fontWeightStrong
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: root.kindLabel(taskRow.task ? taskRow.task.kind : "")
                                        color: appTheme.textMutedColor
                                        wrapMode: Text.Wrap
                                        font.family: appTheme.uiFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                        font.weight: appTheme.fontWeightRegular
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: Boolean(taskRow.task && taskRow.task.detail)
                                        text: (taskRow.task && taskRow.task.detail) || ""
                                        color: taskRow.rowFailed
                                               ? appTheme.backgroundTaskFailedColor
                                               : appTheme.textMutedColor
                                        wrapMode: Text.Wrap
                                        font.family: appTheme.uiFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                        font.weight: appTheme.fontWeightRegular
                                    }

                                    ThemedProgressBar {
                                        Layout.fillWidth: true
                                        visible: taskRow.showProgress
                                        active: taskRow.showProgress
                                        fillColor: taskRow.rowStatusColor
                                        progressValue: {
                                            if (!taskRow.task || taskRow.task.progressPercent === undefined)
                                                return 0
                                            return taskRow.task.progressPercent
                                        }
                                    }
                                }

                                // Quiet row actions — outline chips, not 46 px dialog primaries.
                                ColumnLayout {
                                    Layout.alignment: Qt.AlignVCenter
                                    spacing: appTheme.spaceXs
                                    visible: taskRow.showCancel

                                    Button {
                                        id: cancelButton
                                        objectName: "backgroundTaskCancelButton"
                                        visible: taskRow.showCancel
                                        Layout.preferredWidth: Math.max(implicitWidth,
                                                                        appTheme.iconButtonHitSize * 2)
                                        Layout.preferredHeight: appTheme.spaceXl + appTheme.spaceSm
                                        text: qsTr("Cancel")
                                        hoverEnabled: true
                                        activeFocusOnTab: true
                                        Accessible.name: text
                                        onClicked: root.controller.CancelTask(taskRow.task.id)

                                        contentItem: Label {
                                            text: cancelButton.text
                                            color: appTheme.dangerColor
                                            font.family: appTheme.uiFontFamily
                                            font.pixelSize: appTheme.fontSizeCaption
                                            font.weight: appTheme.fontWeightStrong
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                            leftPadding: appTheme.spaceSm
                                            rightPadding: appTheme.spaceSm
                                        }

                                        background: Rectangle {
                                            radius: appTheme.controlRadiusSmall
                                            color: cancelButton.down
                                                   ? appTheme.buttonPressedFillColor
                                                   : (cancelButton.hovered
                                                      ? appTheme.dangerTintColor
                                                      : appTheme.bgBaseColor)
                                            border.width: 1
                                            border.color: root.withAlpha(appTheme.dangerColor, 0.55)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                spacing: appTheme.spaceSm
                visible: root.taskCount === 0
                width: parent.width - appTheme.spaceXl * 2

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("No background tasks yet")
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: appTheme.fontWeightStrong
                }

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: qsTr("Imports, exports, model work, and AI jobs will appear here.")
                    color: root.withAlpha(appTheme.textMutedColor, 0.72)
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightRegular
                }
            }
        }
    }
}
