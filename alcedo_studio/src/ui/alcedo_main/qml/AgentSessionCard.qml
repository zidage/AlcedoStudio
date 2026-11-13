pragma ComponentBehavior: Bound

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts

// One agent session, pinned above the other background tasks. Shows who
// controls the session, what the agent does now, and its activity: each
// state-changing command with the note the agent gave and its outcome.
//
// Stop (a white disc with a black square) cancels the agent task: the user has
// control at once and the agent is told to stop. The button then turns gray;
// the session continues when the user tells the agent to continue. The archive
// button below Stop dismisses a finished card. While the agent works, a
// gradient turns around the outline of the card.
Rectangle {
    id: root
    objectName: "agentSessionCard"

    required property var task
    property var controller: null

    readonly property string taskState: task ? String(task.state || "") : ""
    readonly property bool active: taskState === "running" || taskState === "queued"
                                   || taskState === "canceling"
    readonly property bool canStop: !!task && task.cancelable === true
                                    && (taskState === "running" || taskState === "queued")
    readonly property bool finished: taskState === "succeeded" || taskState === "failed"
                                     || taskState === "canceled"
    readonly property var activity: task && task.activity ? task.activity : []
    readonly property real activityMaxHeight: appTheme.editorSidePanelWidthMin * 0.75

    function stateLabel() {
        if (taskState === "canceling")
            return qsTr("Stopping")
        if (taskState === "canceled")
            return qsTr("Stopped")
        if (taskState === "failed")
            return qsTr("Failed")
        if (taskState === "succeeded")
            return qsTr("Finished")
        return qsTr("Working")
    }

    function stateColor(state) {
        if (state === "failed")
            return appTheme.backgroundTaskFailedColor
        if (state === "running" || state === "queued" || state === "canceling")
            return appTheme.backgroundTaskWorkingColor
        return appTheme.backgroundTaskFinishedColor
    }

    implicitHeight: content.implicitHeight + appTheme.spaceMd * 2
    radius: appTheme.controlRadiusSmall
    color: appTheme.cardSurfaceColor
    border.width: active ? 0 : 1
    border.color: appTheme.cardBorderColor

    RotatingTaskBorder {
        anchors.fill: parent
        radius: root.radius
        visible: root.active
        running: visible
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: appTheme.spaceMd
        spacing: appTheme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Rectangle {
                Layout.preferredWidth: appTheme.spaceSm
                Layout.preferredHeight: appTheme.spaceSm
                Layout.alignment: Qt.AlignVCenter
                radius: width / 2
                color: root.stateColor(root.taskState)
                Accessible.ignored: true
            }

            ColorImage {
                Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                Layout.alignment: Qt.AlignVCenter
                source: "qrc:/panel_icons/sparkles.svg"
                sourceSize.width: appTheme.iconSourceSizeCompact
                sourceSize.height: appTheme.iconSourceSizeCompact
                color: appTheme.backgroundTaskWorkingColor
                fillMode: Image.PreserveAspectFit
                Accessible.ignored: true
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    text: (root.task && root.task.title) || qsTr("Agent")
                    color: appTheme.textColor
                    elide: Text.ElideRight
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeTitle
                    font.weight: appTheme.fontWeightStrong
                }

                Label {
                    objectName: "agentSessionStateLabel"
                    Layout.fillWidth: true
                    text: qsTr("Agent · %1").arg(root.stateLabel())
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightStrong
                }
            }

            ColumnLayout {
                Layout.alignment: Qt.AlignTop
                spacing: appTheme.spaceXs

                Button {
                    id: stopButton
                    objectName: "agentSessionStopButton"
                    readonly property real side: appTheme.iconOpticalSize + appTheme.spaceXs
                    Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
                    Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                    Layout.alignment: Qt.AlignHCenter
                    enabled: root.canStop
                    hoverEnabled: true
                    padding: 0
                    Accessible.name: root.canStop ? qsTr("Stop the agent")
                                                  : qsTr("Stopped. Tell the agent to continue.")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: Accessible.name
                    onClicked: root.controller.CancelTask(root.task.id)

                    // A white disc with a black square; gray after the stop.
                    background: Item {
                        Rectangle {
                            anchors.centerIn: parent
                            width: stopButton.side
                            height: stopButton.side
                            radius: width / 2
                            color: stopButton.enabled ? "#FFFFFF" : appTheme.textMutedColor
                            opacity: stopButton.enabled ? (stopButton.down ? 0.72
                                                           : (stopButton.hovered ? 0.88 : 1))
                                                        : 0.45
                        }
                    }
                    contentItem: Item {
                        Rectangle {
                            anchors.centerIn: parent
                            width: Math.round(stopButton.side * 0.36)
                            height: width
                            radius: 2
                            color: "#000000"
                            opacity: stopButton.enabled ? 1 : 0.6
                        }
                    }
                }

                IconActionButton {
                    objectName: "agentSessionArchiveButton"
                    Layout.alignment: Qt.AlignHCenter
                    compact: true
                    enabled: root.finished
                    iconSrc: "qrc:/panel_icons/archive.svg"
                    fillIdle: appTheme.cardSurfaceColor
                    actionName: qsTr("Dismiss")
                    onClicked: root.controller.DismissTask(root.task.id)
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: (root.task && root.task.detail) || ""
            color: root.taskState === "failed" ? appTheme.backgroundTaskFailedColor
                                               : appTheme.textMutedColor
            wrapMode: Text.Wrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(root.activityMaxHeight,
                                             activityList.contentHeight + appTheme.spaceSm * 2)
            visible: root.activity.length > 0
            radius: appTheme.controlRadiusSmall
            color: appTheme.bgBaseColor
            border.width: 1
            border.color: appTheme.cardBorderColor

            ListView {
                id: activityList
                objectName: "agentSessionActivityList"
                anchors.fill: parent
                anchors.margins: appTheme.spaceSm
                clip: true
                spacing: appTheme.spaceXs
                model: root.activity
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    policy: activityList.contentHeight > activityList.height
                            ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                }
                // The newest entry is the last one; keep it in view.
                onCountChanged: Qt.callLater(activityList.positionViewAtEnd)
                onModelChanged: Qt.callLater(activityList.positionViewAtEnd)

                delegate: RowLayout {
                    id: entryRow
                    required property var modelData

                    width: ListView.view ? ListView.view.width : 0
                    spacing: appTheme.spaceSm

                    Label {
                        Layout.alignment: Qt.AlignTop
                        text: Qt.formatTime(new Date(Number(entryRow.modelData.startedAtMs)),
                                            "HH:mm:ss")
                        color: appTheme.textMutedColor
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: appTheme.spaceXs
                        Layout.preferredWidth: appTheme.spaceXs + 2
                        Layout.preferredHeight: appTheme.spaceXs + 2
                        radius: width / 2
                        color: root.stateColor(entryRow.modelData.state)
                        Accessible.ignored: true
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Label {
                            Layout.fillWidth: true
                            text: String(entryRow.modelData.text || "")
                            color: entryRow.modelData.state === "failed"
                                   ? appTheme.backgroundTaskFailedColor : appTheme.textColor
                            wrapMode: Text.WrapAnywhere
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: String(entryRow.modelData.note || "")
                            color: appTheme.textMutedColor
                            wrapMode: Text.Wrap
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.italic: true
                        }
                    }
                }
            }
        }
    }
}
