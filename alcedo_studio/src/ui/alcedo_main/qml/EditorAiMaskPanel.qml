import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// AI Mask tool panel (SM9, issues #513 and #519). Fills the lower part of the Mask panel while
// the AI Mask edit is open; the Range section of the Mask panel stays above it (#564). Every value and action is EditorAiMaskTool's; the panel keeps no session
// state. Controls are icon buttons with tooltips: the four tasks, the People list, the prompt
// tools Point / Box / Path with Add / Subtract, the click level, Feather, Expand, and the
// temporary Undo / Redo. A running operation shows the working border around the whole panel
// and the stage progress at its bottom; there is no busy text. Without a usable installed AI
// Mask model set (AI Models plan, AM5) the panel shows only that state and "Open AI Models".
Rectangle {
    id: root
    objectName: "editorAiMaskPanel"

    property var tool: null
    property var maskCreation: null
    // Opens Settings (openAiModelSettings); null in panel tests without the application.
    property var host: null
    readonly property bool toolReady: !!(tool && tool.ready)
    // The installed AI Mask model set stopped the Open; "" when it did not.
    readonly property string modelSetState: tool && tool.modelSetState ? tool.modelSetState : ""
    readonly property bool modelSetMissing: modelSetState.length > 0
    readonly property bool canStep: toolReady && !tool.busy
    readonly property bool busy: !!(tool && tool.busy)
    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    // Short operations (Undo, an edge step) end before the working border would be noticed;
    // the border and progress show only after this delay.
    readonly property int busyRevealDelayMs: appTheme.motionFadeMs
    property bool busyShown: false

    color: appTheme.cardSurfaceColor
    radius: appTheme.controlRadius
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("AI Mask")

    function stageText(stage) {
        switch (stage) {
        case "openingEdit": return qsTr("Opening the edit")
        case "renderingAnalysis": return qsTr("Rendering the analysis image")
        case "startingSidecar": return qsTr("Starting the AI runtime")
        case "loadingModels": return qsTr("Loading the models")
        case "encodingImage": return qsTr("Reading the image")
        case "runningModel": return qsTr("Running the model")
        case "updating": return qsTr("Updating the Mask")
        case "committing": return qsTr("Saving the Mask")
        }
        return ""
    }

    function modelSetText(state) {
        switch (state) {
        case "notInstalled":
            return qsTr("The AI Mask models are not installed. Download them on the AI Models page in Settings.")
        case "updateRequired":
            return qsTr("The installed AI Mask models are too old for this version of Alcedo Studio. Update them on the AI Models page in Settings.")
        case "needsNewerApplication":
            return qsTr("The AI Mask models need a newer version of Alcedo Studio. Update Alcedo Studio to use AI Mask.")
        case "repairRequired":
            return qsTr("The installed AI Mask models are incomplete. Repair them on the AI Models page in Settings.")
        }
        return ""
    }

    // The tool closes: the Settings page installs the models, and the next Open reads them.
    function openAiModels() {
        if (root.host && root.host.openAiModelSettings)
            root.host.openAiModelSettings()
        if (root.tool)
            root.tool.cancel()
    }

    onBusyChanged: {
        if (root.busy) {
            busyRevealTimer.restart()
        } else {
            busyRevealTimer.stop()
            root.busyShown = false
        }
    }

    Timer {
        id: busyRevealTimer
        interval: root.busyRevealDelayMs
        onTriggered: root.busyShown = root.busy
    }

    component Divider: Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: appTheme.dividerColor
    }

    // Raised control fills, so the actions read apart from the panel surface. A selected
    // action uses the monochrome light well with dark ink.
    component ToolIconButton: IconActionButton {
        compact: true
        iconColorDefault: selected ? appTheme.editorListSelectedInkColor : appTheme.iconColor
        iconColorMuted: root.colMuted
        fillIdle: appTheme.controlFillColor
        fillHover: appTheme.controlHoveredFillColor
        fillPressed: appTheme.controlPressedFillColor
        fillSelected: appTheme.editorListSelectedFillColor
    }

    component ToolNav: SlidingIconNav {
        trackColor: appTheme.controlFillColor
        trackBorderColor: appTheme.controlBorderColor
        idleIconColor: appTheme.iconColor
    }

    // Feather and Expand drive the shared AdjustmentSlider through this value model: a handle
    // drag previews through the coverage display, the release adds one session step.
    component EdgeValueModel: QtObject {
        id: edgeModel
        property string label: ""
        property real sourceValue: 0
        property real minimum: 0
        property real maximum: 1
        property real step: 1
        property int precision: 0
        property string suffix: qsTr(" px")
        property bool enabled: root.canStep
        property bool valid: true
        // The dragged value stays shown until the session step returns.
        property bool holding: false
        property real heldValue: 0
        readonly property real value: holding ? heldValue : sourceValue
        // (value) -> void
        property var preview: function (v) {}
        property var accept: function (v) {}

        function beginDrag() {
            heldValue = sourceValue
            holding = true
        }
        function updateDrag(v) {
            heldValue = v
            preview(v)
        }
        function finishDrag() {
            accept(heldValue)
        }
        function editValue(v) {
            heldValue = Math.max(minimum, Math.min(maximum, v))
            holding = true
            accept(heldValue)
        }
        function commitImmediately() {}
        function reset() {
            editValue(0)
        }
        function setInvalid(invalid) {
            valid = !invalid
        }
        onSourceValueChanged: holding = false
    }

    Connections {
        target: root.tool
        ignoreUnknownSignals: true
        // A step that did not apply keeps the session value; the held drag value returns to it.
        function onBusyChanged() {
            if (root.tool && !root.tool.busy) {
                featherModel.holding = false
                expandModel.holding = false
            }
        }
    }

    EdgeValueModel {
        id: featherModel
        label: qsTr("Feather")
        sourceValue: root.tool ? root.tool.featherPx : 0
        maximum: root.tool ? root.tool.maximumFeatherPx : 50
        step: 0.5
        precision: 1
        preview: function (v) { root.tool.previewEdge(v, expandModel.value) }
        accept: function (v) { root.tool.acceptEdge(v, expandModel.value) }
    }

    EdgeValueModel {
        id: expandModel
        label: qsTr("Expand")
        sourceValue: root.tool ? root.tool.expandPx : 0
        minimum: root.tool ? -root.tool.maximumExpandPx : -50
        maximum: root.tool ? root.tool.maximumExpandPx : 50
        preview: function (v) { root.tool.previewEdge(featherModel.value, Math.round(v)) }
        accept: function (v) { root.tool.acceptEdge(featherModel.value, Math.round(v)) }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appTheme.spaceSm
        spacing: appTheme.spaceSm

        // Header: tool icon, title, Close.
        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            ColorImage {
                Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                source: "qrc:/panel_icons/pencil-ai.svg"
                sourceSize.width: appTheme.iconSourceSizeCompact
                sourceSize.height: appTheme.iconSourceSizeCompact
                color: appTheme.iconColor
                Accessible.ignored: true
            }
            Label {
                Layout.fillWidth: true
                text: root.tool && root.tool.editingStoredMask ? qsTr("Edit AI Mask")
                                                                : qsTr("New AI Mask")
                color: root.colText
                elide: Text.ElideRight
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightHeading
            }
            ToolIconButton {
                objectName: "editorAiMaskClose"
                iconSrc: "qrc:/panel_icons/close.svg"
                actionName: qsTr("Close without saving")
                onClicked: root.tool.cancel()
            }
        }

        // The installed AI Mask model set cannot load: its state and the way to the AI Models page.
        // The tool never downloads (AI Models plan, decision A8).
        ColumnLayout {
            objectName: "editorAiMaskModelSet"
            Layout.fillWidth: true
            visible: root.modelSetMissing
            spacing: appTheme.spaceSm

            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                ColorImage {
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                    Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                    source: "qrc:/panel_icons/info.svg"
                    sourceSize.width: appTheme.iconSourceSizeCompact
                    sourceSize.height: appTheme.iconSourceSizeCompact
                    color: root.colMuted
                    Accessible.ignored: true
                }
                Label {
                    objectName: "editorAiMaskModelSetText"
                    Layout.fillWidth: true
                    text: root.modelSetText(root.modelSetState)
                    color: root.colText
                    wrapMode: Text.Wrap
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }
            DialogActionButton {
                objectName: "editorAiMaskOpenAiModels"
                Layout.fillWidth: true
                buttonHeight: appTheme.iconButtonHitSizeCompact
                font.pixelSize: appTheme.fontSizeBody
                text: qsTr("Open AI Models")
                onClicked: root.openAiModels()
            }
        }
        Item {
            Layout.fillHeight: true
            visible: root.modelSetMissing
        }

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.modelSetMissing
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: scroll.availableWidth
                spacing: appTheme.spaceSm

                // Tasks: each runs once and adds a component with the Add / Subtract mode.
                RowLayout {
                    objectName: "editorAiMaskTasks"
                    Layout.fillWidth: true
                    spacing: appTheme.spaceXs

                    ToolIconButton {
                        objectName: "editorAiMaskSubject"
                        iconSrc: "qrc:/mask_icons/ai-subject.svg"
                        actionName: qsTr("Select Subject")
                        enabled: root.canStep
                        onClicked: root.tool.addTask("subject")
                    }
                    ToolIconButton {
                        objectName: "editorAiMaskBackground"
                        iconSrc: "qrc:/mask_icons/ai-background.svg"
                        actionName: qsTr("Select Background")
                        enabled: root.canStep
                        onClicked: root.tool.addTask("background")
                    }
                    ToolIconButton {
                        objectName: "editorAiMaskSky"
                        iconSrc: "qrc:/mask_icons/ai-sky.svg"
                        actionName: qsTr("Select Sky")
                        enabled: root.canStep
                        onClicked: root.tool.addTask("sky")
                    }
                    ToolIconButton {
                        objectName: "editorAiMaskPeople"
                        iconSrc: "qrc:/mask_icons/ai-people.svg"
                        actionName: qsTr("Select People")
                        selected: !!(root.tool && root.tool.peopleActive)
                        enabled: root.canStep
                        onClicked: root.tool.addTask("people")
                    }
                    Item { Layout.fillWidth: true }
                }

                // Training data notice of the Sky model (#537; AI Models plan, decision A7 and
                // section 4). It stays visible under the tasks, so the user reads it before the
                // first Sky run.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceXs

                    ColorImage {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                        Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                        source: "qrc:/panel_icons/info.svg"
                        sourceSize.width: appTheme.iconSourceSizeCompact
                        sourceSize.height: appTheme.iconSourceSizeCompact
                        color: root.colMuted
                        Accessible.ignored: true
                    }
                    Label {
                        objectName: "editorAiMaskSkyNotice"
                        Layout.fillWidth: true
                        text: qsTr("Sky: the license of the Sky model's training data is not clear. Decide yourself before commercial use; see Settings > AI Models.")
                        color: root.colMuted
                        wrapMode: Text.Wrap
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }

                // People list: a click adds or removes the person; the viewport outlines the
                // union of the selected people.
                ListView {
                    id: peopleList
                    objectName: "editorAiMaskPeopleList"
                    readonly property int rowHeight: appTheme.iconButtonHitSizeCompact - appTheme.spaceXs
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(count, 6) * (rowHeight + spacing)
                                            + 2 * appTheme.spaceXs
                    visible: !!(root.tool && root.tool.peopleActive && count > 0)
                    model: root.tool ? root.tool.people : []
                    clip: true
                    spacing: 2
                    topMargin: appTheme.spaceXs
                    bottomMargin: appTheme.spaceXs
                    leftMargin: appTheme.spaceXs
                    rightMargin: appTheme.spaceXs
                    boundsBehavior: Flickable.StopAtBounds
                    Accessible.role: Accessible.List
                    Accessible.name: qsTr("People")

                    Rectangle {
                        parent: peopleList
                        anchors.fill: parent
                        z: -1
                        radius: appTheme.controlRadiusSmall
                        color: appTheme.bgBaseColor
                        border.width: 1
                        border.color: appTheme.controlBorderColor
                    }

                    delegate: Rectangle {
                        id: personRow
                        required property var modelData
                        required property int index
                        readonly property bool included: !!modelData.included
                        readonly property color ink: included ? appTheme.editorListSelectedInkColor
                                                              : root.colText
                        width: ListView.view.width - ListView.view.leftMargin
                               - ListView.view.rightMargin
                        height: peopleList.rowHeight
                        radius: appTheme.controlRadiusSmall
                        color: included ? appTheme.editorListSelectedFillColor
                                        : (rowHover.hovered ? appTheme.buttonHoveredFillColor
                                                            : "transparent")
                        opacity: root.canStep ? 1.0 : 0.55
                        activeFocusOnTab: true
                        Accessible.role: Accessible.CheckBox
                        Accessible.checkable: true
                        Accessible.checked: included
                        Accessible.name: personLabel.text
                        Accessible.onToggleAction: root.tool.togglePerson(modelData.index)
                        Keys.onSpacePressed: root.tool.togglePerson(modelData.index)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: appTheme.spaceSm
                            anchors.rightMargin: appTheme.spaceSm
                            spacing: appTheme.spaceSm

                            ColorImage {
                                Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                                Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                                source: "qrc:/mask_icons/ai-people.svg"
                                sourceSize.width: appTheme.iconSourceSizeCompact
                                sourceSize.height: appTheme.iconSourceSizeCompact
                                color: personRow.ink
                                Accessible.ignored: true
                            }
                            Label {
                                id: personLabel
                                Layout.fillWidth: true
                                text: qsTr("Person %1").arg(personRow.modelData.index + 1)
                                color: personRow.ink
                                elide: Text.ElideRight
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                            }
                        }

                        HoverHandler {
                            id: rowHover
                            onHoveredChanged: {
                                if (!root.tool)
                                    return
                                root.tool.highlightPerson(hovered ? personRow.modelData.index : -1)
                            }
                        }
                        TapHandler {
                            enabled: root.canStep
                            onTapped: root.tool.togglePerson(personRow.modelData.index)
                        }
                    }
                }

                Divider {}

                // Prompt tools and the Add / Subtract mode.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceSm

                    ToolNav {
                        objectName: "editorAiMaskPromptTools"
                        thumbObjectName: "editorAiMaskPromptToolThumb"
                        controlsEnabled: root.toolReady
                        currentKey: root.tool ? root.tool.promptTool : "point"
                        items: [
                            { key: "point", icon: "qrc:/mask_icons/ai-point.svg",
                              label: qsTr("Point: click to add a point"),
                              itemObjectName: "editorAiMaskPointTool" },
                            { key: "box", icon: "qrc:/mask_icons/ai-box.svg",
                              label: qsTr("Box: drag around an object"),
                              itemObjectName: "editorAiMaskBoxTool" },
                            { key: "path", icon: "qrc:/mask_icons/ai-path.svg",
                              label: qsTr("Path: draw a line over an object"),
                              itemObjectName: "editorAiMaskPathTool" }
                        ]
                        onActivated: function (key) { root.tool.setPromptTool(key) }
                    }
                    Item { Layout.fillWidth: true }
                    ToolNav {
                        objectName: "editorAiMaskMode"
                        thumbObjectName: "editorAiMaskModeThumb"
                        controlsEnabled: root.toolReady
                        currentKey: root.tool && root.tool.subtract ? "subtract" : "add"
                        items: [
                            { key: "add", icon: "qrc:/panel_icons/plus.svg",
                              label: qsTr("Add to the Mask"),
                              itemObjectName: "editorAiMaskAdd" },
                            { key: "subtract", icon: "qrc:/panel_icons/minus.svg",
                              label: qsTr("Subtract from the Mask"),
                              itemObjectName: "editorAiMaskSubtract" }
                        ]
                        onActivated: function (key) { root.tool.setSubtract(key === "subtract") }
                    }
                }

                // Level of a single click: a larger circle selects a larger region.
                ToolNav {
                    objectName: "editorAiMaskLevels"
                    thumbObjectName: "editorAiMaskLevelThumb"
                    visible: !!(root.tool && root.tool.levelCount > 0)
                    controlsEnabled: root.canStep
                    currentKey: root.tool ? String(root.tool.selectedLevel) : "0"
                    items: [
                        { key: "0", icon: "qrc:/mask_icons/ai-level-1.svg",
                          label: qsTr("Small region"), itemObjectName: "editorAiMaskLevel0" },
                        { key: "1", icon: "qrc:/mask_icons/ai-level-2.svg",
                          label: qsTr("Medium region"), itemObjectName: "editorAiMaskLevel1" },
                        { key: "2", icon: "qrc:/mask_icons/ai-level-3.svg",
                          label: qsTr("Large region"), itemObjectName: "editorAiMaskLevel2" }
                    ]
                    onActivated: function (key) { root.tool.selectLevel(Number(key)) }
                }

                Divider {}

                AdjustmentSlider {
                    objectName: "editorAiMaskFeather"
                    Layout.fillWidth: true
                    model: featherModel
                }
                AdjustmentSlider {
                    objectName: "editorAiMaskExpand"
                    Layout.fillWidth: true
                    model: expandModel
                }

                // Temporary history of this edit; never the main history.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceXs

                    ToolIconButton {
                        objectName: "editorAiMaskUndo"
                        iconSrc: "qrc:/panel_icons/reset.svg"
                        actionName: qsTr("Undo AI Mask step")
                        enabled: root.canStep && root.tool.canUndo
                        onClicked: root.tool.undo()
                    }
                    ToolIconButton {
                        objectName: "editorAiMaskRedo"
                        iconSrc: "qrc:/panel_icons/retry.svg"
                        actionName: qsTr("Redo AI Mask step")
                        enabled: root.canStep && root.tool.canRedo
                        onClicked: root.tool.redo()
                    }
                    Item { Layout.fillWidth: true }
                }

                AlertBadge {
                    objectName: "editorAiMaskError"
                    text: root.tool ? root.tool.errorText : ""
                }
                // A failed coverage display keeps the previous coverage; Retry shows it again.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceXs
                    visible: previewErrorBadge.visible

                    AlertBadge {
                        id: previewErrorBadge
                        objectName: "editorAiMaskPreviewError"
                        text: root.tool ? root.tool.previewError : ""
                    }
                    ToolIconButton {
                        objectName: "editorAiMaskRetry"
                        visible: !!(root.tool && root.tool.previewStatus === "Previous")
                        iconSrc: "qrc:/panel_icons/retry.svg"
                        actionName: qsTr("Retry the Mask display")
                        onClicked: root.tool.retryPreview()
                    }
                }
                Label {
                    objectName: "editorAiMaskNotice"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.tool ? root.tool.noticeText : ""
                    color: root.colMuted
                    wrapMode: Text.Wrap
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }
        }

        // One full-width Confirm in the Browse LUTs style. Close in the header cancels.
        DialogActionButton {
            objectName: "editorAiMaskCommit"
            Layout.fillWidth: true
            visible: !root.modelSetMissing
            buttonHeight: appTheme.iconButtonHitSizeCompact
            font.pixelSize: appTheme.fontSizeBody
            text: qsTr("Confirm")
            enabled: root.canStep && root.tool.previewStatus !== "Previous"
            onClicked: root.tool.commit()
        }

        // Stage progress of the running operation: the Open stages advance the bar, a model
        // run without stages moves it as indeterminate.
        ColumnLayout {
            objectName: "editorAiMaskProgress"
            Layout.fillWidth: true
            spacing: appTheme.spaceXs
            visible: root.busyShown

            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm
                Label {
                    objectName: "editorAiMaskStage"
                    Layout.fillWidth: true
                    text: root.tool ? root.stageText(root.tool.busyStage) : ""
                    color: root.colMuted
                    elide: Text.ElideRight
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
                Label {
                    visible: !!(root.tool && root.tool.busyStepCount > 0)
                    text: root.tool ? qsTr("%1/%2").arg(root.tool.busyStep)
                                                    .arg(root.tool.busyStepCount)
                                    : ""
                    color: root.colMuted
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }
            ThemedProgressBar {
                Layout.fillWidth: true
                active: root.busyShown
                indeterminate: !(root.tool && root.tool.busyStepCount > 0)
                progressValue: root.tool && root.tool.busyStepCount > 0
                               ? (root.tool.busyStep - 0.5) / root.tool.busyStepCount * 100
                               : 0
            }
        }
    }

    // Working border around the whole panel while an operation runs.
    RotatingTaskBorder {
        objectName: "editorAiMaskWorkingBorder"
        anchors.fill: parent
        radius: root.radius
        visible: root.busyShown
        running: visible
        z: 1
    }
}
