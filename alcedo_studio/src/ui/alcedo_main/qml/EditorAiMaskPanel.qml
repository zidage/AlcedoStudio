import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// AI Mask tool panel (SM9 MVP, issue #513). Covers the adjustment panel stack while the AI Mask
// edit is open. Every value and action is EditorAiMaskTool's; the panel keeps no session state.
// A left click in the viewport adds a click prompt, a left drag a box prompt.
Rectangle {
    id: root
    objectName: "editorAiMaskPanel"

    property var tool: null
    readonly property bool toolReady: !!(tool && tool.ready)
    readonly property bool canStep: toolReady && !tool.busy
    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor

    color: appTheme.bgBaseColor
    radius: appTheme.controlRadius
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("AI Mask")

    component SectionLabel: Label {
        Layout.fillWidth: true
        color: root.colMuted
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        font.weight: appTheme.fontWeightStrong
        wrapMode: Text.Wrap
    }

    component ToolButton: Button {
        Layout.fillWidth: true
        Layout.preferredWidth: 0
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeBody
        Accessible.name: text
    }

    ScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: appTheme.spaceSm
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: scroll.availableWidth
            spacing: appTheme.spaceSm

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: root.tool && root.tool.editingStoredMask ? qsTr("AI Mask (edit)")
                                                                    : qsTr("AI Mask (new)")
                    color: root.colText
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeTitle
                    font.weight: appTheme.fontWeightStrong
                }
                ToolButton {
                    objectName: "editorAiMaskClose"
                    Layout.fillWidth: false
                    Layout.preferredWidth: implicitWidth
                    text: qsTr("Close")
                    onClicked: root.tool.cancel()
                }
            }

            SectionLabel {
                visible: text.length > 0
                text: root.tool ? root.tool.imageText : ""
            }

            // Busy and error text.
            Label {
                objectName: "editorAiMaskBusy"
                Layout.fillWidth: true
                visible: !!(root.tool && root.tool.busy)
                text: root.tool ? root.tool.busyText : ""
                color: appTheme.accentColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "editorAiMaskError"
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.tool ? root.tool.errorText : ""
                color: appTheme.dangerColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                wrapMode: Text.Wrap
            }

            // Prompt / Tasks
            SectionLabel { text: qsTr("Input") }
            RowLayout {
                Layout.fillWidth: true
                ToolButton {
                    text: qsTr("Prompt")
                    checkable: true
                    checked: !!(root.tool && root.tool.inputMode === "prompt")
                    enabled: root.canStep
                    onClicked: root.tool.setInputMode("prompt")
                }
                ToolButton {
                    text: qsTr("Tasks")
                    checkable: true
                    checked: !!(root.tool && root.tool.inputMode === "task")
                    enabled: root.canStep
                    onClicked: root.tool.setInputMode("task")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                ToolButton {
                    objectName: "editorAiMaskAdd"
                    text: qsTr("Add")
                    checkable: true
                    checked: !!(root.tool && !root.tool.subtract)
                    enabled: root.toolReady
                    onClicked: root.tool.setSubtract(false)
                }
                ToolButton {
                    objectName: "editorAiMaskSubtract"
                    text: qsTr("Subtract")
                    checkable: true
                    checked: !!(root.tool && root.tool.subtract)
                    enabled: root.toolReady
                    onClicked: root.tool.setSubtract(true)
                }
            }
            SectionLabel {
                visible: !!(root.tool && root.tool.inputMode === "prompt")
                text: qsTr("Click the photo for a point, drag for a box.")
            }
            RowLayout {
                Layout.fillWidth: true
                visible: !!(root.tool && root.tool.inputMode === "prompt")
                SectionLabel {
                    Layout.fillWidth: false
                    text: qsTr("Level")
                }
                Repeater {
                    model: 3
                    ToolButton {
                        required property int index
                        text: String(index)
                        enabled: root.canStep
                        onClicked: root.tool.selectLevel(index)
                    }
                }
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                ToolButton {
                    objectName: "editorAiMaskSubject"
                    text: qsTr("Subject")
                    enabled: root.canStep
                    onClicked: root.tool.addTask("subject")
                }
                ToolButton {
                    objectName: "editorAiMaskBackground"
                    text: qsTr("Background")
                    enabled: root.canStep
                    onClicked: root.tool.addTask("background")
                }
                ToolButton {
                    objectName: "editorAiMaskSky"
                    text: qsTr("Sky")
                    enabled: root.canStep
                    onClicked: root.tool.addTask("sky")
                }
                ToolButton {
                    objectName: "editorAiMaskPeople"
                    text: qsTr("People")
                    enabled: root.canStep
                    onClicked: root.tool.addTask("people")
                }
            }
            Flow {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs
                visible: !!(root.tool && root.tool.people.length > 0)
                Repeater {
                    model: root.tool ? root.tool.people : []
                    Button {
                        required property var modelData
                        text: qsTr("Person %1 %2").arg(modelData.index + 1)
                                                  .arg(modelData.included ? "✓" : "")
                        checkable: true
                        checked: modelData.included
                        enabled: root.canStep
                        font.pixelSize: appTheme.fontSizeCaption
                        onClicked: root.tool.togglePerson(modelData.index)
                    }
                }
            }

            // Feather / Expand: the drag previews through the coverage display; the release adds
            // one session step.
            SectionLabel {
                text: qsTr("Feather %1 px").arg(featherSlider.value.toFixed(1))
            }
            Slider {
                id: featherSlider
                objectName: "editorAiMaskFeather"
                Layout.fillWidth: true
                from: 0
                to: root.tool ? root.tool.maximumFeatherPx : 50
                stepSize: 0.5
                enabled: root.canStep
                value: root.tool ? root.tool.featherPx : 0
                Accessible.name: qsTr("Feather")
                onMoved: root.tool.previewEdge(value, expandSlider.value)
                onPressedChanged: {
                    if (!pressed)
                        root.tool.acceptEdge(value, expandSlider.value)
                }
            }
            SectionLabel {
                text: qsTr("Expand %1 px").arg(Math.round(expandSlider.value))
            }
            Slider {
                id: expandSlider
                objectName: "editorAiMaskExpand"
                Layout.fillWidth: true
                from: root.tool ? -root.tool.maximumExpandPx : -50
                to: root.tool ? root.tool.maximumExpandPx : 50
                stepSize: 1
                enabled: root.canStep
                value: root.tool ? root.tool.expandPx : 0
                Accessible.name: qsTr("Expand")
                onMoved: root.tool.previewEdge(featherSlider.value, Math.round(value))
                onPressedChanged: {
                    if (!pressed)
                        root.tool.acceptEdge(featherSlider.value, Math.round(value))
                }
            }

            // Session Undo / Redo (temporary stack, never main history).
            RowLayout {
                Layout.fillWidth: true
                ToolButton {
                    objectName: "editorAiMaskUndo"
                    text: qsTr("Undo")
                    enabled: root.canStep && root.tool.canUndo
                    onClicked: root.tool.undo()
                }
                ToolButton {
                    objectName: "editorAiMaskRedo"
                    text: qsTr("Redo")
                    enabled: root.canStep && root.tool.canRedo
                    onClicked: root.tool.redo()
                }
            }
            SectionLabel {
                text: root.tool
                      ? qsTr("Steps: %1 | Preview: %2").arg(root.tool.stateCount)
                                                     .arg(root.tool.previewStatus)
                      : ""
            }
            SectionLabel {
                visible: text.length > 0
                text: root.tool ? root.tool.lastStepText : ""
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.tool ? root.tool.previewError : ""
                color: appTheme.dangerColor
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
            }
            ToolButton {
                objectName: "editorAiMaskRetry"
                visible: !!(root.tool && root.tool.previewStatus === "Previous")
                text: qsTr("Retry preview")
                onClicked: root.tool.retryPreview()
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: appTheme.spaceSm
                DialogActionButton {
                    objectName: "editorAiMaskCancel"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    buttonHeight: 36
                    text: qsTr("Cancel")
                    onClicked: root.tool.cancel()
                }
                DialogActionButton {
                    objectName: "editorAiMaskCommit"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    buttonHeight: 36
                    kind: "accent"
                    text: qsTr("Commit")
                    enabled: root.canStep && root.tool.previewStatus !== "Previous"
                    onClicked: root.tool.commit()
                }
            }
        }
    }
}
