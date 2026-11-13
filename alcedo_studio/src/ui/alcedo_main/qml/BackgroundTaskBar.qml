//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Background-task island on the top toolbar, next to the right-sidebar toggle.
// Detailed task history lives in BackgroundTasksDialog.
//
// At rest the island is a single status lamp that keeps the outcome of the most
// recent task (green done, red failed). It morphs into a pill showing the
// primary task when:
//   - a task keeps running past a short grace delay (quick tasks such as an
//     editor save on image switch never unfold the island);
//   - a task fails; the island folds back after the auto-collapse delay.
// A successful finish folds the island back at once; the lamp pops green.
//
// The agent task is pinned: while an agent works it is the primary task, and
// after it ends the island keeps showing how it ended until the user dismisses
// it in the task dialog, so frequent short tasks such as editor saves do not
// hide it. A sparkles mark tells an agent task from work of the user.
//
// A click on the folded island opens it by hand; a hand-opened island stays
// open (no auto-collapse). A click on the open island opens the task dialog and
// hands the island back to the automatic behavior: it folds after the
// auto-collapse delay unless a task still runs.
//
// Motion: the pill width, the surface opacity, and the text fade all follow one
// `reveal` value. While a task runs, a gradient turns around the whole outline
// of the pill; a known progress shows as a percentage. Hovering an
// automatically opened island keeps it open.
Item {
    id: root
    objectName: "backgroundTaskBar"

    // Main window: openBackgroundTasksDialog().
    property var host: null
    readonly property var controller: appModules ? appModules.backgroundTasks : null
    readonly property var primary: controller && controller.primaryTask
                                   ? controller.primaryTask : ({})
    readonly property int runningCount: controller ? controller.runningCount : 0
    readonly property bool hasTasks: controller && controller.tasks.length > 0
    readonly property bool primaryActive: primary && isActiveState(primary.state)
    readonly property bool progressKnown: primaryActive
                                          && primary.progressPercent !== undefined
                                          && Number(primary.progressPercent) >= 0
    readonly property color lampColor: stateColor(primary.state || "")
    readonly property bool primaryIsAgent: primary && primary.kind === "agentControl"

    readonly property int revealDelayMs: 400
    readonly property real collapsedWidth: appTheme.iconButtonHitSizeCompact
    readonly property real pillHeight: appTheme.iconButtonHitSizeCompact - appTheme.spaceMd
    readonly property real maxExpandedWidth: 360
    readonly property real lampBoxSize: appTheme.spaceMd
    readonly property real contentLeftInset: (collapsedWidth - lampBoxSize) / 2
    readonly property real expandedWidth: Math.max(
        collapsedWidth,
        Math.min(maxExpandedWidth,
                 contentLeftInset + lampBoxSize + islandRow.spacing
                 + detailRow.implicitWidth + appTheme.spaceMd))

    // Target state. `pinned` marks an island the user opened by hand.
    property bool expanded: false
    property bool pinned: false
    // 0 = lamp only, 1 = fully unfolded pill.
    property real reveal: 0
    property string statusFingerprint: ""
    property string lastPrimaryState: ""

    implicitWidth: collapsedWidth + (expandedWidth - collapsedWidth) * reveal
    implicitHeight: appTheme.iconButtonHitSizeCompact
    Layout.preferredWidth: implicitWidth
    Layout.minimumWidth: collapsedWidth
    Layout.preferredHeight: implicitHeight

    Accessible.role: Accessible.Button
    Accessible.name: primaryLabel()
    Accessible.onPressAction: activateByUser()

    function taskStateFingerprint() {
        const tasks = controller ? controller.tasks : []
        const states = []
        for (let i = 0; i < tasks.length; ++i) {
            const task = tasks[i]
            states.push(String(task.id || "") + ":" + String(task.state || ""))
        }
        return states.join("|")
    }

    function animateReveal(target) {
        openAnimation.stop()
        closeAnimation.stop()
        if (appTheme.reduceMotion) {
            reveal = target
            return
        }
        if (reveal === target)
            return
        const animation = target > 0 ? openAnimation : closeAnimation
        animation.restart()
    }

    function expandIsland() {
        expandDelayTimer.stop()
        expanded = true
        animateReveal(1)
    }

    function collapseIsland() {
        expandDelayTimer.stop()
        autoCollapseTimer.stop()
        expanded = false
        pinned = false
        animateReveal(0)
    }

    function activateByUser() {
        if (!expanded) {
            expandIsland()
            autoCollapseTimer.stop()
            pinned = true
            return
        }
        pinned = false
        if (host)
            host.openBackgroundTasksDialog()
        if (!primaryActive)
            scheduleAutoCollapse()
    }

    function scheduleAutoCollapse() {
        if (!pinned && expanded && !islandHover.hovered)
            autoCollapseTimer.restart()
    }

    function isActiveState(state) {
        return state === "queued" || state === "running" || state === "canceling"
    }

    function handleTaskTransition() {
        if (pinned)
            return
        autoCollapseTimer.stop()
        // Read the controller directly: the `primary` binding may not have
        // re-evaluated yet when this handler runs.
        const task = controller && controller.primaryTask ? controller.primaryTask : ({})
        if (isActiveState(task.state)) {
            if (!expanded)
                expandDelayTimer.restart()
            return
        }
        expandDelayTimer.stop()
        if (task.state === "failed") {
            expandIsland()
            scheduleAutoCollapse()
            return
        }
        collapseIsland()
    }

    function stateColor(state) {
        if (state === "failed")
            return appTheme.backgroundTaskFailedColor
        if (isActiveState(state))
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
        if (kind === "libraryMarkUpdate") return qsTr("Labels and Marks")
        if (kind === "imageDelete") return qsTr("Delete Images")
        if (kind === "agentControl") return qsTr("Agent")
        return qsTr("Background Tasks")
    }

    function primaryLabel() {
        if (!hasTasks)
            return qsTr("No recent tasks")
        if (!(primary && primary.title))
            return qsTr("Background Tasks")
        // The agent task names the agent and what it does now, or how it ended.
        if (primary.kind === "agentControl" && primary.detail)
            return qsTr("%1 · %2").arg(primary.title).arg(primary.detail)
        const kind = kindLabel(primary.kind)
        if (primary.state === "failed" && primary.detail)
            return qsTr("%1 · %2").arg(kind).arg(primary.detail)
        return qsTr("%1 · %2").arg(kind).arg(primary.title)
    }

    onPrimaryChanged: {
        const nextState = String(primary && primary.state ? primary.state : "")
        if (nextState !== lastPrimaryState) {
            const finished = nextState === "succeeded" || nextState === "failed"
                             || nextState === "canceled"
            lastPrimaryState = nextState
            if (finished && !appTheme.reduceMotion)
                lampPop.restart()
        }
    }

    Connections {
        target: root.controller
        ignoreUnknownSignals: true

        function onTasksChanged() {
            const nextFingerprint = root.taskStateFingerprint()
            if (nextFingerprint !== root.statusFingerprint) {
                root.statusFingerprint = nextFingerprint
                root.handleTaskTransition()
            }
        }
    }

    // Quick tasks finish inside this window and never unfold the island.
    Timer {
        id: expandDelayTimer
        interval: root.revealDelayMs
        onTriggered: {
            if (root.primaryActive && !root.pinned)
                root.expandIsland()
        }
    }

    Timer {
        id: autoCollapseTimer
        interval: appTheme.backgroundTaskAutoCollapseMs
        onTriggered: {
            if (!root.pinned && !root.primaryActive)
                root.collapseIsland()
        }
    }

    NumberAnimation {
        id: openAnimation
        target: root
        property: "reveal"
        to: 1
        duration: appTheme.motionFoldOpenMs + appTheme.motionFadeMs * 2
        easing.type: Easing.OutBack
        easing.overshoot: 1.2
    }

    NumberAnimation {
        id: closeAnimation
        target: root
        property: "reveal"
        to: 0
        duration: appTheme.motionFoldCloseMs + appTheme.motionFadeMs
        easing.type: Easing.InOutCubic
    }

    Component.onCompleted: {
        statusFingerprint = taskStateFingerprint()
        lastPrimaryState = String(primary && primary.state ? primary.state : "")
        if (primaryActive)
            expandDelayTimer.restart()
    }

    HoverHandler {
        id: islandHover
        cursorShape: Qt.PointingHandCursor
        onHoveredChanged: {
            if (hovered)
                autoCollapseTimer.stop()
            else if (root.primary && root.primary.state === "failed")
                root.scheduleAutoCollapse()
        }
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: root.activateByUser()
    }

    // Hover feedback for the bare lamp; the pill surface takes over as it unfolds.
    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.pillHeight
        radius: height / 2
        color: appTheme.buttonHoveredFillColor
        opacity: islandHover.hovered ? Math.max(0, 1 - root.reveal * 2) : 0

        Behavior on opacity {
            enabled: !appTheme.reduceMotion
            NumberAnimation {
                duration: appTheme.motionFadeMs
            }
        }
    }

    Rectangle {
        id: surface
        objectName: "backgroundTaskBarSurface"
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.pillHeight
        radius: height / 2
        color: appTheme.cardSurfaceColor
        // A running task draws its outline with RotatingTaskBorder instead.
        border.width: root.primaryActive ? 0 : 1
        border.color: Qt.rgba(root.lampColor.r, root.lampColor.g, root.lampColor.b, 0.2)
        opacity: Math.min(1, Math.max(0, root.reveal) * 1.6)

        Behavior on border.color {
            enabled: !appTheme.reduceMotion
            ColorAnimation {
                duration: appTheme.motionFoldOpenMs
            }
        }

        RotatingTaskBorder {
            objectName: "backgroundTaskBarWorkingBorder"
            anchors.fill: parent
            radius: surface.radius
            visible: root.primaryActive && root.reveal > 0
            running: visible
            color: root.lampColor
        }
    }

    Item {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.pillHeight
        clip: true

        RowLayout {
            id: islandRow
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: root.contentLeftInset
            width: Math.max(0, parent.width - root.contentLeftInset - appTheme.spaceMd)
            spacing: appTheme.spaceSm

            // Status lamp: it pops once when the task ends.
            Item {
                Layout.preferredWidth: root.lampBoxSize
                Layout.preferredHeight: root.lampBoxSize
                Layout.alignment: Qt.AlignVCenter
                Accessible.ignored: true

                Rectangle {
                    id: lamp
                    objectName: "backgroundTaskBarLamp"
                    anchors.centerIn: parent
                    width: appTheme.spaceSm
                    height: appTheme.spaceSm
                    radius: width / 2
                    color: root.lampColor

                    Behavior on color {
                        enabled: !appTheme.reduceMotion
                        ColorAnimation {
                            duration: appTheme.motionFoldOpenMs
                        }
                    }
                }

                NumberAnimation {
                    id: lampPop
                    target: lamp
                    property: "scale"
                    from: 1.6
                    to: 1
                    duration: appTheme.motionFoldOpenMs + appTheme.motionFadeMs
                    easing.type: Easing.OutBack
                    easing.overshoot: 2.2
                }
            }

            RowLayout {
                id: detailRow
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: appTheme.spaceSm
                opacity: Math.max(0, Math.min(1, root.reveal * 1.8 - 0.8))
                visible: root.reveal > 0

                ColorImage {
                    objectName: "backgroundTaskBarAgentMark"
                    visible: root.primaryIsAgent
                    Layout.preferredWidth: appTheme.iconOpticalSizeCompact - appTheme.spaceXs
                    Layout.preferredHeight: Layout.preferredWidth
                    Layout.alignment: Qt.AlignVCenter
                    source: "qrc:/panel_icons/sparkles.svg"
                    sourceSize.width: appTheme.iconSourceSizeCompact
                    sourceSize.height: appTheme.iconSourceSizeCompact
                    color: appTheme.backgroundTaskWorkingColor
                    fillMode: Image.PreserveAspectFit
                    Accessible.ignored: true
                }

                Label {
                    Layout.fillWidth: true
                    text: root.primaryLabel()
                    color: appTheme.textColor
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
                }

                Label {
                    visible: root.progressKnown
                    text: qsTr("%1%").arg(Math.round(Number(root.primary.progressPercent)))
                    color: appTheme.textMutedColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
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
