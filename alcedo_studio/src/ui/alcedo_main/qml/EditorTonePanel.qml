import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Phase 6B Tone panel: exposure / contrast / highlights / shadows / whites /
// blacks + tone-curve editor. Values and curve points submit through the
// EditorSessionController submitter seam (operator-shaped params JSON). Models
// own the gesture lifecycle (one settled transaction per completed gesture).
// No pipeline/scheduler calls from this file.
Item {
    id: root
    objectName: "editorAdjustmentPanel_tone"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colBase: theme ? theme.colBgBase : appTheme.bgBaseColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor

    // Operator-shaped params builders (Phase 6A default is {"value": v}).
    function numericParams(key, v) {
        var o = {}
        o[key] = v
        return JSON.stringify(o)
    }

    function wireEnabled() {
        const on = root.controlsEnabled
        exposureModel.enabled = on
        contrastModel.enabled = on
        highlightsModel.enabled = on
        shadowsModel.enabled = on
        whitesModel.enabled = on
        blacksModel.enabled = on
        curveModel.enabled = on
    }

    onControlsEnabledChanged: wireEnabled()
    Component.onCompleted: {
        // Clean baseline defaults match pipeline_defaults / ToneAdjustmentState.
        exposureModel.value = exposureModel.defaultValue
        contrastModel.value = contrastModel.defaultValue
        highlightsModel.value = highlightsModel.defaultValue
        shadowsModel.value = shadowsModel.defaultValue
        whitesModel.value = whitesModel.defaultValue
        blacksModel.value = blacksModel.defaultValue
        wireEnabled()
    }

    EditorAdjustmentValueModel {
        id: exposureModel
        objectName: "toneExposureModel"
        fieldKey: "exposure"
        label: qsTr("Exposure")
        minimum: -10
        maximum: 10
        defaultValue: 1.5
        step: 0.01
        precision: 2
        submitter: root.editorSession
        paramsBuilder: function (v) { return root.numericParams("exposure", v) }
    }
    EditorAdjustmentValueModel {
        id: contrastModel
        objectName: "toneContrastModel"
        fieldKey: "contrast"
        label: qsTr("Contrast")
        minimum: -100
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
        paramsBuilder: function (v) { return root.numericParams("contrast", v) }
    }
    EditorAdjustmentValueModel {
        id: highlightsModel
        objectName: "toneHighlightsModel"
        fieldKey: "highlights"
        label: qsTr("Highlights")
        minimum: -100
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
        paramsBuilder: function (v) { return root.numericParams("highlights", v) }
    }
    EditorAdjustmentValueModel {
        id: shadowsModel
        objectName: "toneShadowsModel"
        fieldKey: "shadows"
        label: qsTr("Shadows")
        minimum: -100
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
        paramsBuilder: function (v) { return root.numericParams("shadows", v) }
    }
    EditorAdjustmentValueModel {
        id: whitesModel
        objectName: "toneWhitesModel"
        fieldKey: "white"
        label: qsTr("Whites")
        minimum: -100
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
        paramsBuilder: function (v) { return root.numericParams("white", v) }
    }
    EditorAdjustmentValueModel {
        id: blacksModel
        objectName: "toneBlacksModel"
        fieldKey: "black"
        label: qsTr("Blacks")
        minimum: -100
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
        paramsBuilder: function (v) { return root.numericParams("black", v) }
    }
    EditorToneCurveModel {
        id: curveModel
        objectName: "toneCurveModel"
        fieldKey: "curve"
        label: qsTr("Tone Curve")
        submitter: root.editorSession
    }

    readonly property var toneModels: [
        exposureModel, contrastModel, highlightsModel,
        shadowsModel, whitesModel, blacksModel
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Label {
            Layout.fillWidth: true
            text: qsTr("Tone")
            color: root.colText
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightHeading
        }

        // Fold reference objectName preserved for WorkspaceShellTests.
        CollapsibleSection {
            id: toneGroup
            objectName: "editorAdjustmentGroupShell_tone"
            Layout.fillWidth: true
            title: qsTr("Tone")
            expanded: true
            controlsEnabled: root.controlsEnabled
            surfaceColor: root.colCardSurface
            disabledSurfaceColor: root.colCardSurface
            borderColor: root.colCardBorder
            textColor: root.colText
            mutedColor: root.colMuted
            hoverColor: root.colHover
            accentColor: root.colAccent
            bodyContentHeight: toneControls.implicitHeight + appTheme.spaceSm

            ColumnLayout {
                id: toneControls
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: appTheme.spaceXs
                spacing: appTheme.spaceSm

                AdjustmentSlider {
                    objectName: "toneExposureSlider"
                    Layout.fillWidth: true
                    model: exposureModel
                }
                AdjustmentSlider {
                    objectName: "toneContrastSlider"
                    Layout.fillWidth: true
                    model: contrastModel
                }
                AdjustmentSlider {
                    objectName: "toneHighlightsSlider"
                    Layout.fillWidth: true
                    model: highlightsModel
                }
                AdjustmentSlider {
                    objectName: "toneShadowsSlider"
                    Layout.fillWidth: true
                    model: shadowsModel
                }
                AdjustmentSlider {
                    objectName: "toneWhitesSlider"
                    Layout.fillWidth: true
                    model: whitesModel
                }
                AdjustmentSlider {
                    objectName: "toneBlacksSlider"
                    Layout.fillWidth: true
                    model: blacksModel
                }
            }
        }

        CollapsibleSection {
            id: curveGroup
            objectName: "editorToneCurveGroup"
            Layout.fillWidth: true
            title: qsTr("Tone Curve")
            expanded: true
            controlsEnabled: root.controlsEnabled
            surfaceColor: root.colCardSurface
            disabledSurfaceColor: root.colCardSurface
            borderColor: root.colCardBorder
            textColor: root.colText
            mutedColor: root.colMuted
            hoverColor: root.colHover
            accentColor: root.colAccent
            bodyContentHeight: curveBody.implicitHeight + appTheme.spaceSm

            ColumnLayout {
                id: curveBody
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: appTheme.spaceXs
                spacing: appTheme.spaceXs

                EditorToneCurveItem {
                    id: curveItem
                    objectName: "editorToneCurveItem"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 220
                    model: curveModel
                    backgroundColor: root.colCardSurface
                    plotColor: root.colBase
                    gridColor: root.colCardBorder
                    diagonalColor: root.colMuted
                    curveColor: root.colAccent
                    handleColor: root.colText
                    handleActiveColor: root.colAccent
                    handleOutlineColor: root.colAccent
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceSm

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Left click/drag to shape. Right click a point to remove. Double click to reset.")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    AdjustmentResetButton {
                        objectName: "toneCurveResetButton"
                        model: curveModel
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
