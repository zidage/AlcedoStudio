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
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

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
                visible: !root.analyticSelected
                wrapMode: Text.WordWrap
                text: root.maskCreation && root.maskCreation.creating
                      ? qsTr("Draw on the photograph to create the Mask.")
                      : qsTr("Select a Mask in the Node drawer or choose a Mask tool.")
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
            }

            ParameterLabel { visible: root.radialSelected; text: qsTr("Horizontal radius") }
            EditorMonoSlider {
                objectName: "editorMasksMajorRadiusSlider"
                Layout.fillWidth: true
                visible: root.radialSelected
                enabled: root.radialSelected
                from: 0.1; to: 100; stepSize: 0.1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                externalValue: root.maskCreation ? root.maskCreation.majorRadiusPercent : 0
                onBegin: root.maskCreation.beginMajorRadius()
                onUpdate: function (v) { root.maskCreation.updateMajorRadius(v) }
                onFinish: root.maskCreation.finishAnalyticControl()
                onReset: {
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
                from: 0.1; to: 100; stepSize: 0.1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                externalValue: root.maskCreation ? root.maskCreation.minorRadiusPercent : 0
                onBegin: root.maskCreation.beginMinorRadius()
                onUpdate: function (v) { root.maskCreation.updateMinorRadius(v) }
                onFinish: root.maskCreation.finishAnalyticControl()
                onReset: {
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
                from: -180; to: 180; stepSize: 0.5; pointerGain: 1
                rowHeight: 28; handleSize: 18
                externalValue: root.maskCreation ? root.maskCreation.rotationDegrees : 0
                onBegin: root.maskCreation.beginRotation()
                onUpdate: function (v) { root.maskCreation.updateRotation(v) }
                onFinish: root.maskCreation.finishAnalyticControl()
                onReset: {
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
                from: 0; to: 100; stepSize: 1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                externalValue: root.maskCreation ? root.maskCreation.innerFeatherPercent : 0
                onBegin: root.maskCreation.beginInnerFeather()
                onUpdate: function (v) { root.maskCreation.updateInnerFeather(v) }
                onFinish: root.maskCreation.finishAnalyticControl()
                onReset: {
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
                from: 0; to: 400; stepSize: 1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                externalValue: root.maskCreation
                               ? Math.min(400, root.maskCreation.outerFeatherPercent) : 0
                onBegin: root.maskCreation.beginOuterFeather()
                onUpdate: function (v) { root.maskCreation.updateOuterFeather(v) }
                onFinish: root.maskCreation.finishAnalyticControl()
                onReset: {
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
                from: 0.1; to: 200; stepSize: 0.1; pointerGain: 1
                rowHeight: 28; handleSize: 18
                externalValue: root.maskCreation ? root.maskCreation.transitionPercent : 0
                onBegin: root.maskCreation.beginTransition()
                onUpdate: function (v) { root.maskCreation.updateTransition(v) }
                onFinish: root.maskCreation.finishAnalyticControl()
                onReset: {
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
