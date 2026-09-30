import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Post Processing page. Every control writes the document DRT node, whichever node is
// selected: the scene-linear Diffusion filter (before the display transform), then the
// display-referred Clarity, Sharpen, Film Grain, and Halation adjustments.
Item {
    id: root
    objectName: "editorAdjustmentPanel_post"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor

    function wireEnabled() {
        const on = root.controlsEnabled
        diffusionModel.enabled = on
        clarityModel.enabled = on
        sharpenModel.enabled = on
        filmGrainModel.enabled = on
        halationModel.enabled = on
    }

    onControlsEnabledChanged: wireEnabled()
    Component.onCompleted: {
        wireEnabled()
        // Bootstrap when the stack has not yet projected. Settled fan-out still
        // owns ongoing echoes via EditorAdjustmentStack.
        loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }

    function loadFromSnapshot(snapshot) {
        if (snapshot === undefined || snapshot === null)
            return
        loadNestedPercent(diffusionModel, "diffusion", "strength", snapshot)
        loadModelFromSnapshot(clarityModel, "clarity", snapshot)
        loadSharpenFromSnapshot(snapshot)
        loadNestedPercent(filmGrainModel, "film_grain", "strength", snapshot)
        loadNestedPercent(halationModel, "halation", "strength", snapshot)
    }

    function loadModelFromSnapshot(model, fieldKey, snapshot) {
        if (!model || !fieldKey || !snapshot)
            return
        if (model.dragActive)
            return
        const entry = snapshot[fieldKey]
        if (entry === undefined)
            return
        const val = entry[fieldKey] !== undefined ? entry[fieldKey] : entry.value
        if (val === undefined)
            return
        const num = Number(val)
        if (isNaN(num))
            return
        if (Math.abs(model.value - num) > (model.step * 0.1))
            model.value = num
    }

    /// Stored 0..1 strengths show as 0..100 on the slider.
    function loadNestedPercent(model, fieldKey, nestedKey, snapshot) {
        if (!model || !snapshot)
            return
        if (model.dragActive)
            return
        const entry = snapshot[fieldKey]
        if (entry === undefined)
            return
        const nested = entry[fieldKey]
        const val = (nested && nested[nestedKey] !== undefined) ? nested[nestedKey]
                  : (entry[nestedKey] !== undefined ? entry[nestedKey] : undefined)
        if (val === undefined)
            return
        const num = Number(val) * 100.0
        if (isNaN(num))
            return
        if (Math.abs(model.value - num) > (model.step * 0.1))
            model.value = num
    }

    function loadSharpenFromSnapshot(snapshot) {
        if (!snapshot)
            return
        if (sharpenModel.dragActive)
            return
        const entry = snapshot.sharpen
        if (entry === undefined)
            return
        const nested = entry.sharpen
        const val = (nested && nested.offset !== undefined) ? nested.offset
                  : (entry.offset !== undefined ? entry.offset : undefined)
        if (val === undefined)
            return
        const num = Number(val)
        if (isNaN(num))
            return
        if (Math.abs(sharpenModel.value - num) > 0.1)
            sharpenModel.value = num
    }

    EditorAdjustmentValueModel {
        id: diffusionModel
        objectName: "postDiffusionModel"
        fieldKey: "diffusion"
        label: qsTr("Strength")
        minimum: 0
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: clarityModel
        objectName: "postClarityModel"
        fieldKey: "clarity"
        label: qsTr("Clarity")
        minimum: -100
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: sharpenModel
        objectName: "postSharpenModel"
        fieldKey: "sharpen"
        label: qsTr("Sharpen")
        minimum: 0
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: filmGrainModel
        objectName: "postFilmGrainModel"
        fieldKey: "film_grain"
        label: qsTr("Film Grain")
        minimum: 0
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: halationModel
        objectName: "postHalationModel"
        fieldKey: "halation"
        label: qsTr("Halation")
        minimum: 0
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
    }

    component SectionShell: CollapsibleSection {
        Layout.fillWidth: true
        expanded: true
        controlsEnabled: root.controlsEnabled
        surfaceColor: root.colCardSurface
        disabledSurfaceColor: root.colCardSurface
        borderColor: root.colCardBorder
        textColor: root.colText
        mutedColor: root.colMuted
        hoverColor: root.colHover
        accentColor: root.colAccent
    }

    Flickable {
        id: postScroll
        objectName: "editorPostProcessPanelScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: postColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        pressDelay: 0

        property int inputLockCount: 0
        function beginInputLock() {
            if (inputLockCount === 0)
                interactive = false
            inputLockCount += 1
        }
        function endInputLock() {
            if (inputLockCount > 0)
                inputLockCount -= 1
            if (inputLockCount <= 0) {
                inputLockCount = 0
                interactive = true
            }
        }

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            grabPermissions: PointerHandler.CanTakeOverFromItems
                             | PointerHandler.CanTakeOverFromHandlersOfDifferentType
                             | PointerHandler.ApprovesTakeOverByAnything
            onWheel: function (event) {
                var step = event.pixelDelta.y !== 0
                           ? event.pixelDelta.y
                           : event.angleDelta.y / 120 * 48
                var maxY = Math.max(0, postScroll.contentHeight - postScroll.height)
                postScroll.contentY = Math.max(0, Math.min(maxY, postScroll.contentY - step))
                event.accepted = true
            }
        }

        ColumnLayout {
            id: postColumn
            width: postScroll.width
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: qsTr("Post Processing")
                color: root.colText
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightHeading
            }

            SectionShell {
                objectName: "editorAdjustmentGroupShell_post_diffusion"
                title: qsTr("Diffusion")
                bodyContentHeight: diffusionBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: diffusionBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm
                    AdjustmentSlider {
                        objectName: "postDiffusionSlider"
                        Layout.fillWidth: true
                        model: diffusionModel
                        flickable: postScroll
                    }
                }
            }

            SectionShell {
                objectName: "editorAdjustmentGroupShell_post_detail"
                title: qsTr("Detail")
                bodyContentHeight: detailBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: detailBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm
                    AdjustmentSlider {
                        objectName: "postClaritySlider"
                        Layout.fillWidth: true
                        model: clarityModel
                        flickable: postScroll
                    }
                    AdjustmentSlider {
                        objectName: "postSharpenSlider"
                        Layout.fillWidth: true
                        model: sharpenModel
                        flickable: postScroll
                    }
                }
            }

            SectionShell {
                objectName: "editorAdjustmentGroupShell_post_texture"
                title: qsTr("Texture")
                bodyContentHeight: textureBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: textureBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm
                    AdjustmentSlider {
                        objectName: "postFilmGrainSlider"
                        Layout.fillWidth: true
                        model: filmGrainModel
                        flickable: postScroll
                    }
                    AdjustmentSlider {
                        objectName: "postHalationSlider"
                        Layout.fillWidth: true
                        model: halationModel
                        flickable: postScroll
                    }
                }
            }
        }
    }
}
