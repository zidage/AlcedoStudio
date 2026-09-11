import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Parameter editor for the Mask selected in the Node drawer or created from
// the adjustment header. The Node drawer is the only Mask list/management
// surface; this page owns only source-specific controls for the exact MaskId.
Item {
    id: root
    objectName: "editorAdjustmentPanel_masks"

    property var theme: null
    property var editorSession: null
    property var nodeController: null
    property var maskCreation: null
    property bool controlsEnabled: true

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property string selectedMaskId: root.maskCreation
                                               ? String(root.maskCreation.selectedMaskId || "")
                                               : ""
    readonly property string sourceKind: root.maskCreation
                                           ? String(root.maskCreation.toolKind || "")
                                           : ""
    readonly property bool radialSelected: root.controlsEnabled
                                            && root.selectedMaskId.length > 0
                                            && root.sourceKind === "radial"
    readonly property bool gradientSelected: root.controlsEnabled
                                              && root.selectedMaskId.length > 0
                                              && root.sourceKind === "linear"
    readonly property bool analyticSelected: root.radialSelected || root.gradientSelected
    readonly property bool brushSelected: root.controlsEnabled
                                          && root.sourceKind === "brush"
    readonly property bool maskSelected: root.controlsEnabled
                                          && root.selectedMaskId.length > 0

    function loadFromSnapshot(snapshot) {
        void snapshot
    }

    component ParameterLabel: Label {
        Layout.fillWidth: true
        color: root.colText
        font.pixelSize: appTheme.fontSizeCaption
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    component ParameterValue: Label {
        Layout.fillWidth: true
        color: root.colMuted
        font.family: appTheme.dataFontFamily
        font.pixelSize: appTheme.fontSizeCaption
    }

    Flickable {
        id: maskScroll
        objectName: "editorMasksScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds

        function beginInputLock() {
            interactive = false
        }
        function endInputLock() {
            interactive = contentHeight > height
        }

        ColumnLayout {
            id: content
            width: parent.width
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: qsTr("Mask")
                color: root.colText
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightHeading
            }

            Label {
                objectName: "editorMasksContextEmpty"
                Layout.fillWidth: true
                visible: !root.maskSelected
                wrapMode: Text.WordWrap
                text: root.maskCreation && root.maskCreation.creating
                      ? qsTr("Draw on the photograph to create the Mask.")
                      : qsTr("Select a Mask in the Node drawer or choose a Mask tool.")
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
            }

            // Mask fields for the selected Mask. Each edit settles as one
            // typed SetMaskField; opacity drags stay Interactive until release.
            ParameterLabel { visible: root.maskSelected; text: qsTr("Name") }
            TextField {
                id: maskNameField
                objectName: "editorMasksNameField"
                Layout.fillWidth: true
                visible: root.maskSelected
                enabled: root.maskSelected
                color: root.colText
                font.pixelSize: appTheme.fontSizeBody
                selectByMouse: true
                activeFocusOnTab: true
                Accessible.role: Accessible.EditableText
                Accessible.name: qsTr("Mask name")
                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: maskNameField.activeFocus ? appTheme.accentColor
                                                            : appTheme.dividerColor
                }
                Component.onCompleted: {
                    if (root.maskCreation)
                        text = root.maskCreation.maskName
                }
                onEditingFinished: {
                    if (root.maskCreation)
                        root.maskCreation.setMaskName(text)
                }
                onActiveFocusChanged: {
                    if (!activeFocus && root.maskCreation)
                        text = root.maskCreation.maskName
                }
                Connections {
                    target: root.maskCreation
                    enabled: root.maskCreation !== null
                    function onMaskCreationChanged() {
                        if (!maskNameField.activeFocus && root.maskCreation)
                            maskNameField.text = root.maskCreation.maskName
                    }
                }
            }

            ThemeCheckBox {
                objectName: "editorMasksEnabledCheck"
                Layout.fillWidth: true
                visible: root.maskSelected
                enabled: root.maskSelected
                text: qsTr("Enabled")
                checked: root.maskCreation ? root.maskCreation.maskEnabled : true
                onToggled: function (checked) {
                    if (root.maskCreation)
                        root.maskCreation.setMaskEnabled(checked)
                }
            }
            ThemeCheckBox {
                objectName: "editorMasksInvertCheck"
                Layout.fillWidth: true
                visible: root.maskSelected
                enabled: root.maskSelected
                text: qsTr("Invert")
                checked: root.maskCreation ? root.maskCreation.maskInvert : false
                onToggled: function (checked) {
                    if (root.maskCreation)
                        root.maskCreation.setMaskInvert(checked)
                }
            }

            ParameterLabel { visible: root.maskSelected; text: qsTr("Opacity") }
            EditorMonoSlider {
                objectName: "editorMasksOpacitySlider"
                Layout.fillWidth: true
                visible: root.maskSelected
                enabled: root.maskSelected
                accessibleName: qsTr("Mask opacity")
                from: 0; to: 100; stepSize: 1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                flickable: maskScroll
                externalValue: root.maskCreation ? root.maskCreation.maskOpacityPercent : 100
                onBegin: function () { root.maskCreation.beginMaskOpacity() }
                onUpdate: function (v) { root.maskCreation.updateMaskOpacity(v) }
                onFinish: function () { root.maskCreation.finishAnalyticControl() }
                onReset: function () {
                    root.maskCreation.beginMaskOpacity()
                    root.maskCreation.updateMaskOpacity(100)
                    root.maskCreation.finishAnalyticControl()
                }
            }
            ParameterValue {
                objectName: "editorMasksOpacityValue"
                visible: root.maskSelected
                text: qsTr("%1 percent").arg(Math.round(root.maskCreation
                                                        ? root.maskCreation.maskOpacityPercent
                                                        : 100))
            }

            // Position: arrows nudge the focused Mask position through the
            // same queued translate-handle route a pointer drag uses.
            ParameterLabel { visible: nudgeArea.visible; text: qsTr("Position") }
            Item {
                id: nudgeArea
                objectName: "editorMasksNudgeArea"
                Layout.fillWidth: true
                implicitHeight: 28
                visible: root.maskCreation
                         ? root.maskCreation.maskNudgeAvailable : false
                activeFocusOnTab: visible
                Accessible.role: Accessible.Grouping
                Accessible.name: qsTr("Mask position. Arrow keys move; Shift steps faster.")

                property bool nudgeActive: false

                Keys.onPressed: function (event) {
                    if (!root.maskCreation || !visible)
                        return
                    var step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
                    var dx = 0
                    var dy = 0
                    if (event.key === Qt.Key_Left)
                        dx = -step
                    else if (event.key === Qt.Key_Right)
                        dx = step
                    else if (event.key === Qt.Key_Up)
                        dy = -step
                    else if (event.key === Qt.Key_Down)
                        dy = step
                    else
                        return
                    if (!event.isAutoRepeat && !nudgeActive)
                        nudgeActive = root.maskCreation.beginMaskNudge()
                    if (nudgeActive)
                        root.maskCreation.nudgeMaskBy(dx, dy)
                    event.accepted = true
                }
                Keys.onReleased: function (event) {
                    if (!root.maskCreation || !nudgeActive)
                        return
                    if (event.key === Qt.Key_Left || event.key === Qt.Key_Right
                            || event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                        if (!event.isAutoRepeat) {
                            root.maskCreation.finishAnalyticControl()
                            nudgeActive = false
                            event.accepted = true
                        }
                    }
                }

                Rectangle {
                    anchors.fill: parent
                    radius: appTheme.controlRadiusSmall
                    color: nudgeArea.activeFocus ? appTheme.hoverColor
                                                 : appTheme.bgBaseColor
                    border.width: 1
                    border.color: nudgeArea.activeFocus ? appTheme.accentColor
                                                        : appTheme.cardBorderColor
                }
                Label {
                    anchors.centerIn: parent
                    width: parent.width - appTheme.spaceSm * 2
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Arrow keys move · Shift ×10")
                    color: root.colMuted
                    font.pixelSize: appTheme.fontSizeCaption
                    elide: Text.ElideRight
                }
            }

            // Brush tool: Paint/Erase append strokes; Move drags the whole
            // placement. Move needs an existing committed Brush Mask.
            ParameterLabel { visible: root.brushSelected; text: qsTr("Tool") }
            SegmentedCardSwitcher {
                id: brushToolSegments
                objectName: "editorMasksBrushToolSegments"
                Layout.fillWidth: true
                visible: root.brushSelected
                enabled: root.brushSelected
                entries: [
                    { "label": qsTr("Paint"), "value": "paint" },
                    { "label": qsTr("Erase"), "value": "erase",
                      "enabled": root.maskSelected },
                    { "label": qsTr("Move"), "value": "move",
                      "enabled": root.maskSelected }
                ]
                currentValue: root.maskCreation ? String(root.maskCreation.brushTool) : ""
                onSelected: function (index, value) {
                    if (root.maskCreation)
                        root.maskCreation.setBrushTool(value)
                }
            }

            ParameterLabel { visible: root.brushSelected; text: qsTr("Brush size") }
            EditorMonoSlider {
                objectName: "editorMasksBrushSizeSlider"
                Layout.fillWidth: true
                visible: root.brushSelected
                enabled: root.brushSelected
                accessibleName: qsTr("Brush size")
                from: 0.5; to: 50; stepSize: 0.1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                flickable: maskScroll
                externalValue: root.maskCreation ? root.maskCreation.brushDiameterPercent : 0
                onUpdate: function (v) { root.maskCreation.setBrushDiameterPercent(v) }
            }
            ParameterValue {
                objectName: "editorMasksBrushSizeValue"
                visible: root.brushSelected
                text: qsTr("%1 px").arg(Math.round(root.maskCreation
                                                   ? root.maskCreation.brushDiameter : 0))
            }

            ParameterLabel { visible: root.brushSelected; text: qsTr("Brush strength") }
            EditorMonoSlider {
                objectName: "editorMasksBrushStrengthSlider"
                Layout.fillWidth: true
                visible: root.brushSelected
                enabled: root.brushSelected
                accessibleName: qsTr("Brush strength")
                from: 0; to: 100; stepSize: 1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                flickable: maskScroll
                externalValue: root.maskCreation ? root.maskCreation.brushStrengthPercent : 100
                onUpdate: function (v) { root.maskCreation.setBrushStrengthPercent(v) }
            }
            ParameterValue {
                objectName: "editorMasksBrushStrengthValue"
                visible: root.brushSelected
                text: qsTr("%1 percent").arg(Math.round(root.maskCreation
                                                        ? root.maskCreation.brushStrengthPercent
                                                        : 100))
            }

            ParameterLabel { visible: root.brushSelected && root.maskSelected
                             ; text: qsTr("Brush feather") }
            EditorMonoSlider {
                objectName: "editorMasksBrushFeatherSlider"
                Layout.fillWidth: true
                visible: root.brushSelected && root.maskSelected
                enabled: root.brushSelected && root.maskSelected
                accessibleName: qsTr("Brush feather")
                from: 0; to: 20; stepSize: 0.1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                flickable: maskScroll
                externalValue: root.maskCreation ? root.maskCreation.brushFeatherPercent : 0
                onBegin: function () { root.maskCreation.beginBrushFeather() }
                onUpdate: function (v) { root.maskCreation.updateBrushFeatherPercent(v) }
                onFinish: function () { root.maskCreation.finishAnalyticControl() }
                onReset: function () {
                    root.maskCreation.beginBrushFeather()
                    root.maskCreation.updateBrushFeatherPercent(0)
                    root.maskCreation.finishAnalyticControl()
                }
            }
            ParameterValue {
                objectName: "editorMasksBrushFeatherValue"
                visible: root.brushSelected && root.maskSelected
                text: qsTr("%1 percent").arg(Number(root.maskCreation
                                                    ? root.maskCreation.brushFeatherPercent : 0)
                                             .toFixed(1))
            }

            ParameterLabel { visible: root.radialSelected; text: qsTr("Horizontal radius") }
            EditorMonoSlider {
                objectName: "editorMasksMajorRadiusSlider"
                Layout.fillWidth: true
                visible: root.radialSelected
                enabled: root.radialSelected
                accessibleName: qsTr("Mask horizontal radius")
                from: 0.1; to: 100; stepSize: 0.1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                flickable: maskScroll
                externalValue: root.maskCreation ? root.maskCreation.majorRadiusPercent : 0
                onBegin: function () { root.maskCreation.beginMajorRadius() }
                onUpdate: function (v) { root.maskCreation.updateMajorRadius(v) }
                onFinish: function () { root.maskCreation.finishAnalyticControl() }
                onReset: function () {
                    root.maskCreation.beginMajorRadius()
                    root.maskCreation.updateMajorRadius(50)
                    root.maskCreation.finishAnalyticControl()
                }
            }
            ParameterValue {
                objectName: "editorMasksMajorRadiusValue"
                visible: root.radialSelected
                text: qsTr("%1 percent").arg(Number(root.maskCreation
                                                     ? root.maskCreation.majorRadiusPercent : 0)
                                              .toFixed(1))
            }

            ParameterLabel { visible: root.radialSelected; text: qsTr("Vertical radius") }
            EditorMonoSlider {
                objectName: "editorMasksMinorRadiusSlider"
                Layout.fillWidth: true
                visible: root.radialSelected
                enabled: root.radialSelected
                accessibleName: qsTr("Mask vertical radius")
                from: 0.1; to: 100; stepSize: 0.1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                flickable: maskScroll
                externalValue: root.maskCreation ? root.maskCreation.minorRadiusPercent : 0
                onBegin: function () { root.maskCreation.beginMinorRadius() }
                onUpdate: function (v) { root.maskCreation.updateMinorRadius(v) }
                onFinish: function () { root.maskCreation.finishAnalyticControl() }
                onReset: function () {
                    root.maskCreation.beginMinorRadius()
                    root.maskCreation.updateMinorRadius(50)
                    root.maskCreation.finishAnalyticControl()
                }
            }
            ParameterValue {
                objectName: "editorMasksMinorRadiusValue"
                visible: root.radialSelected
                text: qsTr("%1 percent").arg(Number(root.maskCreation
                                                     ? root.maskCreation.minorRadiusPercent : 0)
                                              .toFixed(1))
            }

            ParameterLabel { visible: root.analyticSelected; text: qsTr("Rotation") }
            EditorMonoSlider {
                objectName: "editorMasksRotationSlider"
                Layout.fillWidth: true
                visible: root.analyticSelected
                enabled: root.analyticSelected
                accessibleName: qsTr("Mask rotation")
                from: -180; to: 180; stepSize: 0.5; pointerGain: 1
                rowHeight: 28; handleSize: 18
                flickable: maskScroll
                externalValue: root.maskCreation ? root.maskCreation.rotationDegrees : 0
                onBegin: function () { root.maskCreation.beginRotation() }
                onUpdate: function (v) { root.maskCreation.updateRotation(v) }
                onFinish: function () { root.maskCreation.finishAnalyticControl() }
                onReset: function () {
                    root.maskCreation.beginRotation()
                    root.maskCreation.updateRotation(0)
                    root.maskCreation.finishAnalyticControl()
                }
            }
            ParameterValue {
                objectName: "editorMasksRotationValue"
                visible: root.analyticSelected
                text: qsTr("%1 degrees").arg(Number(root.maskCreation
                                                     ? root.maskCreation.rotationDegrees : 0)
                                              .toFixed(1))
            }

            ParameterLabel { visible: root.radialSelected; text: qsTr("Inner feather") }
            EditorMonoSlider {
                objectName: "editorMasksInnerFeatherSlider"
                Layout.fillWidth: true
                visible: root.radialSelected
                enabled: root.radialSelected
                accessibleName: qsTr("Mask inner feather")
                from: 0; to: 100; stepSize: 1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                flickable: maskScroll
                externalValue: root.maskCreation ? root.maskCreation.innerFeatherPercent : 0
                onBegin: function () { root.maskCreation.beginInnerFeather() }
                onUpdate: function (v) { root.maskCreation.updateInnerFeather(v) }
                onFinish: function () { root.maskCreation.finishAnalyticControl() }
                onReset: function () {
                    root.maskCreation.beginInnerFeather()
                    root.maskCreation.updateInnerFeather(0)
                    root.maskCreation.finishAnalyticControl()
                }
            }
            ParameterValue {
                objectName: "editorMasksInnerFeatherValue"
                visible: root.radialSelected
                text: qsTr("%1 percent").arg(Math.round(root.maskCreation
                                                        ? root.maskCreation.innerFeatherPercent : 0))
            }

            ParameterLabel { visible: root.radialSelected; text: qsTr("Outer feather") }
            EditorMonoSlider {
                objectName: "editorMasksOuterFeatherSlider"
                Layout.fillWidth: true
                visible: root.radialSelected
                enabled: root.radialSelected
                accessibleName: qsTr("Mask outer feather")
                from: 0; to: 400; stepSize: 1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                flickable: maskScroll
                externalValue: root.maskCreation
                               ? Math.min(400, root.maskCreation.outerFeatherPercent) : 0
                onBegin: function () { root.maskCreation.beginOuterFeather() }
                onUpdate: function (v) { root.maskCreation.updateOuterFeather(v) }
                onFinish: function () { root.maskCreation.finishAnalyticControl() }
                onReset: function () {
                    root.maskCreation.beginOuterFeather()
                    root.maskCreation.updateOuterFeather(0)
                    root.maskCreation.finishAnalyticControl()
                }
            }
            ParameterValue {
                objectName: "editorMasksOuterFeatherValue"
                visible: root.radialSelected
                text: qsTr("%1 percent").arg(Math.round(root.maskCreation
                                                        ? root.maskCreation.outerFeatherPercent : 0))
            }

            ParameterLabel { visible: root.gradientSelected; text: qsTr("Transition width") }
            EditorMonoSlider {
                objectName: "editorMasksTransitionSlider"
                Layout.fillWidth: true
                visible: root.gradientSelected
                enabled: root.gradientSelected
                accessibleName: qsTr("Mask transition width")
                from: 0.1; to: 200; stepSize: 0.1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                flickable: maskScroll
                externalValue: root.maskCreation ? root.maskCreation.transitionPercent : 0
                onBegin: function () { root.maskCreation.beginTransition() }
                onUpdate: function (v) { root.maskCreation.updateTransition(v) }
                onFinish: function () { root.maskCreation.finishAnalyticControl() }
                onReset: function () {
                    root.maskCreation.beginTransition()
                    root.maskCreation.updateTransition(20)
                    root.maskCreation.finishAnalyticControl()
                }
            }
            ParameterValue {
                objectName: "editorMasksTransitionValue"
                visible: root.gradientSelected
                text: qsTr("%1 percent").arg(Number(root.maskCreation
                                                     ? root.maskCreation.transitionPercent : 0)
                                              .toFixed(1))
            }

            Item { Layout.fillHeight: true }
        }
    }
}
