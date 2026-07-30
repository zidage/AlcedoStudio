import QtQuick
import QtQml

// Frameless-window minimize / maximize-restore / restore-fade animations and
// the visibility-driven restore trigger. `contentTarget` is the Main content
// item the animations mutate; `host` is the ApplicationWindow (for showMinimized
// / showMaximized / showNormal and the windowMaximized/windowRestoring flags).
Item {
    id: root
    property var host: null
    property Item contentTarget: null

    readonly property bool maximizeRunning: maximizeTransition.running

    // ── Minimize / restore content animations ──
    SequentialAnimation {
        id: minimizeAnimation
        ParallelAnimation {
            NumberAnimation { target: root.contentTarget; property: "scale"; from: 1.0; to: 0.96; duration: 130; easing.type: Easing.InCubic }
            NumberAnimation { target: root.contentTarget; property: "opacity"; to: 0.0; duration: 110; easing.type: Easing.InCubic }
        }
        ScriptAction { script: if (root.host) root.host.showMinimized() }
        ScriptAction { script: { if (root.contentTarget) root.contentTarget.scale = 1.0 } }
    }

    // Fade-through: hide content fully → switch visibility (OS animates window) → wait for the
    // OS resize to finish → fade back in. Hiding during the resize avoids the visible Layout
    // re-flow ("twitch") that happens while the window dimensions are interpolating.
    SequentialAnimation {
        id: maximizeTransition
        property bool targetMaximize: false

        NumberAnimation { target: root.contentTarget; property: "opacity"; to: 0.0; duration: 110; easing.type: Easing.OutCubic }
        ScriptAction {
            script: {
                if (!root.host) return
                if (maximizeTransition.targetMaximize) {
                    root.host.showMaximized()
                } else {
                    root.host.showNormal()
                }
            }
        }
        // Wait long enough for the Win11 maximize/restore animation to finish.
        PauseAnimation { duration: 240 }
        NumberAnimation { target: root.contentTarget; property: "opacity"; to: 1.0; duration: 180; easing.type: Easing.OutCubic }
    }

    NumberAnimation {
        id: restoreAnimation
        target: root.contentTarget
        property: "opacity"
        from: 0.0
        to: 1.0
        duration: 200
        easing.type: Easing.OutCubic
    }

    Connections {
        target: root.host
        ignoreUnknownSignals: true
        function onVisibilityChanged() {
            if (root.host && root.host.windowRestoring
                    && root.contentTarget && root.contentTarget.opacity < 1.0
                    && !restoreAnimation.running) {
                restoreAnimation.start()
            }
        }
    }

    function minimize() {
        minimizeAnimation.start()
    }

    function toggleMaximize() {
        if (maximizeTransition.running) {
            return
        }
        maximizeTransition.targetMaximize = root.host ? !root.host.windowMaximized : false
        maximizeTransition.start()
    }

    // Force-maximize (used when a project opens successfully).
    function maximize() {
        if (maximizeTransition.running) {
            return
        }
        maximizeTransition.targetMaximize = true
        maximizeTransition.start()
    }
}