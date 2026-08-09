import QtQuick
import QtQuick.Layouts

// Shared themed progress track used by BackgroundTasksDialog rows and the
// Export inspector footer. Deterministic fill when progressValue is in [0, 100];
// indeterminate sweeps a short bar when active and reduceMotion is off.
Item {
    id: root
    objectName: "themedProgressBar"

    property real progressValue: 0
    property bool indeterminate: false
    property bool active: true
    property color fillColor: appTheme.backgroundTaskWorkingColor
    property color trackColor: Qt.rgba(appTheme.textMutedColor.r,
                                       appTheme.textMutedColor.g,
                                       appTheme.textMutedColor.b, 0.18)

    Layout.fillWidth: true
    Layout.preferredHeight: appTheme.spaceXs + 2
    implicitHeight: appTheme.spaceXs + 2
    implicitWidth: 120
    Accessible.role: Accessible.ProgressBar
    Accessible.name: qsTr("Progress")
    Accessible.description: indeterminate
                            ? qsTr("Indeterminate progress")
                            : qsTr("%1 percent").arg(Math.round(progressValue))

    readonly property real clampedValue: Math.max(0, Math.min(100, progressValue))

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: root.trackColor
        Accessible.ignored: true
    }

    Rectangle {
        id: progressFill
        height: parent.height
        radius: height / 2
        color: root.fillColor
        width: root.indeterminate
               ? parent.width * 0.28
               : parent.width * (root.clampedValue / 100.0)
        x: root.indeterminate ? indeterminateAnim.phase * (parent.width - width) : 0

        Behavior on width {
            enabled: !root.indeterminate && !appTheme.reduceMotion
            NumberAnimation {
                duration: appTheme.motionFadeMs
                easing.type: Easing.OutCubic
            }
        }
    }

    SequentialAnimation {
        id: indeterminateAnim
        property real phase: 0
        running: root.active && root.indeterminate && !appTheme.reduceMotion
        loops: Animation.Infinite

        NumberAnimation {
            target: indeterminateAnim
            property: "phase"
            from: 0
            to: 1
            duration: appTheme.motionFoldOpenMs * 5 + appTheme.motionFadeMs
            easing.type: Easing.InOutSine
        }
        NumberAnimation {
            target: indeterminateAnim
            property: "phase"
            from: 1
            to: 0
            duration: appTheme.motionFoldOpenMs * 5 + appTheme.motionFadeMs
            easing.type: Easing.InOutSine
        }
    }
}
