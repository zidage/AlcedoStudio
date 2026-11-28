import QtQuick
import QtQuick.Controls

// The steps of a render of the open photo that reads the RAW file or runs the AI Denoise model
// (AI Denoise plan, DN14; DESIGN.md "Viewport render progress"). `progress` is
// EditorSessionController.renderProgress: {steps, step, completed, total, group, gradeIndex,
// gradeCount}, empty when no such render runs. A pill at the bottom left names the step and its
// count; a bar at the bottom edge has one segment for each step. When the render ends, the bar
// fills and both fade out.
Item {
    id: root
    objectName: "editorRenderProgress"

    property var progress: ({})

    readonly property bool running: progress && progress.steps !== undefined
                                    && progress.steps.length > 0
    // The values that the pill and the bar show; kept while they fade out after the render.
    property var shown: ({})
    property bool finished: false
    readonly property var shownSteps: shown && shown.steps !== undefined ? shown.steps : []
    readonly property int currentIndex: shownSteps.indexOf(shown.step || "")
    readonly property bool showing: opacity > 0

    function stepName(step) {
        switch (step) {
        case "read_raw":
            return qsTr("Reading RAW")
        case "ai_denoise":
            return qsTr("AI Denoise")
        default:
            return qsTr("Developing")
        }
    }

    function countText() {
        const completed = Number(shown.completed || 0)
        const total = Number(shown.total || 0)
        if (shown.step === "ai_denoise")
            return total > 0 ? qsTr("%1 / %2 tiles").arg(completed).arg(total) : ""
        if (shown.step !== "develop")
            return ""
        switch (shown.group) {
        case "sensor":
            return qsTr("Sensor")
        case "lens_and_geometry":
            return qsTr("Lens and geometry")
        case "color":
            return qsTr("Color")
        case "color_grade":
            return qsTr("Color Grade %1 of %2").arg(Number(shown.gradeIndex || 0) + 1)
                                               .arg(Number(shown.gradeCount || 0))
        case "display":
            return qsTr("Display")
        default:
            return ""
        }
    }

    // The fill of segment @p index: 1 for a done step, the count of the current step, 0 after it.
    function segmentFill(index) {
        if (root.finished || index < root.currentIndex)
            return 1
        if (index > root.currentIndex)
            return 0
        const total = Number(shown.total || 0)
        return total > 0 ? Math.min(1, Number(shown.completed || 0) / total) : 0
    }

    onProgressChanged: {
        if (root.running) {
            root.shown = root.progress
            root.finished = false
        } else if (root.shownSteps.length > 0) {
            root.finished = true
        }
    }

    opacity: root.running ? 1 : 0
    visible: root.showing
    Behavior on opacity {
        NumberAnimation { duration: appTheme.viewportProgressFadeMs; easing.type: Easing.OutCubic }
    }
    onOpacityChanged: {
        if (opacity === 0 && !root.running) {
            root.shown = ({})
            root.finished = false
        }
    }

    Accessible.role: Accessible.ProgressBar
    Accessible.name: root.stepName(root.shown.step || "")
    Accessible.description: root.countText()

    Rectangle {
        id: pill
        objectName: "editorRenderProgressPill"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: appTheme.spaceMd
        anchors.bottomMargin: appTheme.spaceMd
        width: pillRow.implicitWidth + 2 * appTheme.spaceSm + 2
        height: appTheme.viewportProgressPillHeight
        radius: height / 2
        color: appTheme.viewportProgressPillColor
        border.width: 1
        border.color: appTheme.glassStrokeColor

        Row {
            id: pillRow
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: appTheme.spaceSm + 1
            spacing: appTheme.spaceSm

            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: appTheme.viewportProgressDotSize - 2

                Repeater {
                    model: root.shownSteps.length
                    delegate: Rectangle {
                        required property int index
                        anchors.verticalCenter: parent.verticalCenter
                        width: appTheme.viewportProgressDotSize
                        height: width
                        radius: width / 2
                        color: root.finished || index < root.currentIndex
                               ? appTheme.viewportProgressFillColor
                               : index === root.currentIndex
                                 ? appTheme.textColor
                                 : appTheme.viewportProgressTrackColor
                    }
                }
            }

            Label {
                objectName: "editorRenderProgressStep"
                anchors.verticalCenter: parent.verticalCenter
                text: root.stepName(root.shown.step || "")
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightHeading
            }

            Label {
                objectName: "editorRenderProgressCount"
                anchors.verticalCenter: parent.verticalCenter
                visible: text.length > 0
                text: root.countText()
                color: appTheme.viewportAlertSecondaryTextColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.features: { "tnum": 1 }
            }
        }
    }

    Row {
        id: bar
        objectName: "editorRenderProgressBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: appTheme.viewportProgressBarHeight
        spacing: appTheme.viewportProgressSegmentGap

        Repeater {
            model: root.shownSteps.length
            delegate: Rectangle {
                id: segment
                required property int index
                readonly property bool sweeping: !root.finished && index === root.currentIndex
                                                 && Number(root.shown.total || 0) === 0
                width: (bar.width - bar.spacing * Math.max(0, root.shownSteps.length - 1))
                       / Math.max(1, root.shownSteps.length)
                height: bar.height
                color: appTheme.viewportProgressTrackColor
                clip: true

                Rectangle {
                    visible: !segment.sweeping
                    height: parent.height
                    width: parent.width * root.segmentFill(segment.index)
                    color: appTheme.viewportProgressFillColor
                }

                // A step without a count (Reading RAW, a model run before its tiles are planned).
                Rectangle {
                    id: sweep
                    visible: segment.sweeping
                    height: parent.height
                    width: parent.width * 0.3
                    color: appTheme.viewportProgressFillColor

                    NumberAnimation on x {
                        running: segment.sweeping && root.showing
                        loops: Animation.Infinite
                        from: -sweep.width
                        to: segment.width
                        duration: 1100
                        easing.type: Easing.InOutSine
                    }
                }
            }
        }
    }
}
