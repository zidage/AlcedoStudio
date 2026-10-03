//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Transient summary of the primary background task. A task state transition
// reveals the bar for three seconds; the bar then folds down out of the main
// layout. Detailed task history lives in BackgroundTasksDialog.
//
// Motion: the reserved layout height, the surface opacity, a short rise, and a
// slight scale all follow one `reveal` value, so the workspace above never jumps.
// While a task runs, the status lamp pulses and a hairline along the bottom edge
// shows the progress (a moving highlight when the progress is unknown). Hovering
// the bar keeps it open.
Item {
    id: root
    objectName: "backgroundTaskBar"

    readonly property var controller: appModules ? appModules.backgroundTasks : null
    readonly property var primary: controller && controller.primaryTask
                                   ? controller.primaryTask : ({})
    readonly property int runningCount: controller ? controller.runningCount : 0
    readonly property bool hasTasks: controller && controller.tasks.length > 0
    readonly property real barHeight: Math.max(appTheme.iconButtonHitSizeCompact,
                                               taskBarRow.implicitHeight + appTheme.spaceXs)
    readonly property bool primaryActive: primary
                                          && (primary.state === "running"
                                              || primary.state === "canceling")
    readonly property bool progressKnown: primaryActive
                                          && primary.progressPercent !== undefined
                                          && Number(primary.progressPercent) >= 0
    readonly property color lampColor: stateColor(primary.state || "")

    // 0 = folded away, 1 = fully shown.
    property real reveal: 0
    property bool layoutActive: false
    property string statusFingerprint: ""
    property string lastPrimaryState: ""

    visible: layoutActive
    clip: true
    Layout.fillWidth: true
    Layout.preferredHeight: layoutActive ? barHeight * reveal : 0

    function taskStateFingerprint() {
        const tasks = controller ? controller.tasks : []
        const states = []
        for (let i = 0; i < tasks.length; ++i) {
            const task = tasks[i]
            states.push(String(task.id || "") + ":" + String(task.state || ""))
        }
        return states.join("|")
    }

    function openBar() {
        closeAnimation.stop()
        layoutActive = true
        if (appTheme.reduceMotion) {
            reveal = 1
            return
        }
        if (reveal < 1)
            openAnimation.restart()
    }

    function closeBar() {
        openAnimation.stop()
        if (!layoutActive || appTheme.reduceMotion || reveal <= 0) {
            reveal = 0
            layoutActive = false
            return
        }
        closeAnimation.restart()
    }

    function revealTemporarily() {
        if (!hasTasks) {
            autoCollapseTimer.stop()
            closeBar()
            return
        }
        openBar()
        if (!surfaceHover.hovered)
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
        if (kind === "adjustmentPaste") return qsTr("Paste Adjustments")
        if (kind === "ratingUpdate") return qsTr("Ratings")
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
                root.revealTemporarily()
            }
        }
    }

    Timer {
        id: autoCollapseTimer
        interval: appTheme.backgroundTaskAutoCollapseMs
        onTriggered: root.closeBar()
    }

    NumberAnimation {
        id: openAnimation
        target: root
        property: "reveal"
        to: 1
        duration: appTheme.motionFoldOpenMs + appTheme.motionFadeMs
        easing.type: Easing.OutQuint
    }

    SequentialAnimation {
        id: closeAnimation
        NumberAnimation {
            target: root
            property: "reveal"
            to: 0
            duration: appTheme.motionFoldCloseMs
            easing.type: Easing.InCubic
        }
        ScriptAction {
            script: root.layoutActive = false
        }
    }

    Component.onCompleted: {
        statusFingerprint = taskStateFingerprint()
        lastPrimaryState = String(primary && primary.state ? primary.state : "")
        if (hasTasks)
            revealTemporarily()
    }

    Rectangle {
        id: surface
        objectName: "backgroundTaskBarSurface"
        width: parent.width
        height: root.barHeight
        anchors.bottom: parent.bottom
        radius: Math.min(height / 2, appTheme.panelRadius * 2)
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: Qt.rgba(root.lampColor.r, root.lampColor.g, root.lampColor.b,
                              root.primaryActive ? 0.28 : 0.18)
        opacity: Math.min(1, root.reveal * 1.4)
        transformOrigin: Item.Bottom
        scale: 0.96 + 0.04 * root.reveal
        transform: Translate {
            y: (1 - root.reveal) * appTheme.spaceMd
        }

        Behavior on border.color {
            enabled: !appTheme.reduceMotion
            ColorAnimation {
                duration: appTheme.motionFoldOpenMs
            }
        }

        HoverHandler {
            id: surfaceHover
            onHoveredChanged: {
                if (hovered)
                    autoCollapseTimer.stop()
                else if (root.layoutActive && root.hasTasks)
                    autoCollapseTimer.restart()
            }
        }

        RowLayout {
            id: taskBarRow
            anchors.fill: parent
            anchors.leftMargin: appTheme.spaceMd
            anchors.rightMargin: appTheme.spaceMd
            spacing: appTheme.spaceSm

            // Status lamp: pulses while the task runs, pops once when it ends.
            Item {
                Layout.preferredWidth: appTheme.spaceMd
                Layout.preferredHeight: appTheme.spaceMd
                Accessible.ignored: true

                Rectangle {
                    id: lampHalo
                    anchors.centerIn: parent
                    width: lamp.width
                    height: lamp.height
                    radius: width / 2
                    color: root.lampColor
                    opacity: 0
                    scale: 1
                }

                ParallelAnimation {
                    running: root.primaryActive && root.layoutActive && !appTheme.reduceMotion
                    loops: Animation.Infinite
                    onStopped: {
                        lampHalo.opacity = 0
                        lampHalo.scale = 1
                    }

                    NumberAnimation {
                        target: lampHalo
                        property: "scale"
                        from: 1
                        to: 2.6
                        duration: appTheme.motionFoldOpenMs * 6
                        easing.type: Easing.OutCubic
                    }
                    NumberAnimation {
                        target: lampHalo
                        property: "opacity"
                        from: 0.45
                        to: 0
                        duration: appTheme.motionFoldOpenMs * 6
                        easing.type: Easing.OutCubic
                    }
                }

                Rectangle {
                    id: lamp
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

            Label {
                Layout.fillWidth: true
                text: root.primaryLabel()
                color: appTheme.textColor
                wrapMode: Text.Wrap
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

        // Hairline progress along the bottom edge, inset so it stays inside the
        // rounded corners.
        Item {
            id: progressTrack
            objectName: "backgroundTaskBarProgress"
            visible: root.primaryActive
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: surface.radius
            anchors.rightMargin: surface.radius
            anchors.bottomMargin: 1
            height: 2
            clip: true
            Accessible.role: Accessible.ProgressBar
            Accessible.name: root.primaryLabel()

            Rectangle {
                anchors.fill: parent
                radius: height / 2
                color: Qt.rgba(root.lampColor.r, root.lampColor.g, root.lampColor.b, 0.16)
            }

            Rectangle {
                visible: root.progressKnown
                height: parent.height
                radius: height / 2
                color: root.lampColor
                width: parent.width * Math.max(0, Math.min(100,
                                    Number(root.primary.progressPercent || 0))) / 100

                Behavior on width {
                    enabled: !appTheme.reduceMotion
                    NumberAnimation {
                        duration: appTheme.motionFoldOpenMs + appTheme.motionFadeMs
                        easing.type: Easing.OutCubic
                    }
                }
            }

            // Unknown progress: a soft highlight sweeps across the track.
            Rectangle {
                id: sweep
                visible: !root.progressKnown
                width: Math.max(appTheme.spaceMd * 4, parent.width * 0.32)
                height: parent.height
                radius: height / 2
                x: appTheme.reduceMotion ? (parent.width - width) / 2 : -width
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 0.5; color: root.lampColor }
                    GradientStop { position: 1.0; color: "transparent" }
                }

                NumberAnimation on x {
                    running: sweep.visible && progressTrack.visible && root.layoutActive
                             && !appTheme.reduceMotion
                    loops: Animation.Infinite
                    from: -sweep.width
                    to: progressTrack.width
                    duration: appTheme.motionFoldOpenMs * 7
                    easing.type: Easing.InOutCubic
                }
            }
        }
    }
}
