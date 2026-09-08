import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// DRT/Post Detail page: Clarity, Sharpen, Halation, and Film Grain.
Item {
    id: root
    objectName: "editorAdjustmentPanel_detail"

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
        clarityModel.enabled = on
        sharpenModel.enabled = on
        filmGrainModel.enabled = on
        halationModel.enabled = on
    }

    onControlsEnabledChanged: wireEnabled()
    Component.onCompleted: {
        wireEnabled()
        loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }

    function loadFromSnapshot(snapshot) {
        if (snapshot === undefined || snapshot === null)
            return
        loadModelFromSnapshot(clarityModel, "clarity", snapshot)
        loadSharpenFromSnapshot(snapshot)
        loadNestedStrength(filmGrainModel, "film_grain", "strength", snapshot)
        loadNestedStrength(halationModel, "halation", "strength", snapshot)
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
        var num = Number(val)
        if (isNaN(num))
            return
        if (Math.abs(model.value - num) > (model.step * 0.1))
            model.value = num
    }

    function loadNestedStrength(model, fieldKey, nestedKey, snapshot) {
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
        var num = Number(val)
        if (isNaN(num))
            return
        if (fieldKey === "film_grain" || fieldKey === "halation")
            num *= 100.0
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
        id: clarityModel
        objectName: "detailClarityModel"
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
        objectName: "detailSharpenModel"
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
        objectName: "detailFilmGrainModel"
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
        objectName: "detailHalationModel"
        fieldKey: "halation"
        label: qsTr("Halation")
        minimum: 0
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
    }

    Flickable {
        id: detailScroll
        objectName: "editorDetailPanelScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: detailColumn.implicitHeight
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
                var maxY = Math.max(0, detailScroll.contentHeight - detailScroll.height)
                detailScroll.contentY = Math.max(0, Math.min(maxY, detailScroll.contentY - step))
                event.accepted = true
            }
        }

        ColumnLayout {
            id: detailColumn
            width: detailScroll.width
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: qsTr("Detail")
                color: root.colText
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightHeading
            }

            CollapsibleSection {
                objectName: "editorAdjustmentGroupShell_detail"
                Layout.fillWidth: true
                title: qsTr("Detail")
                expanded: true
                controlsEnabled: root.controlsEnabled
                surfaceColor: root.colCardSurface
                disabledSurfaceColor: root.colCardSurface
                borderColor: root.colCardBorder
                textColor: root.colText
                mutedColor: root.colMuted
                hoverColor: root.colHover
                accentColor: root.colAccent
                bodyContentHeight: detailBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: detailBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm
                    AdjustmentSlider {
                        objectName: "detailClaritySlider"
                        Layout.fillWidth: true
                        model: clarityModel
                        flickable: detailScroll
                    }
                    AdjustmentSlider {
                        objectName: "detailSharpenSlider"
                        Layout.fillWidth: true
                        model: sharpenModel
                        flickable: detailScroll
                    }
                }
            }

            CollapsibleSection {
                objectName: "editorAdjustmentGroupShell_detail_texture"
                Layout.fillWidth: true
                title: qsTr("Texture")
                expanded: true
                controlsEnabled: root.controlsEnabled
                surfaceColor: root.colCardSurface
                disabledSurfaceColor: root.colCardSurface
                borderColor: root.colCardBorder
                textColor: root.colText
                mutedColor: root.colMuted
                hoverColor: root.colHover
                accentColor: root.colAccent
                bodyContentHeight: textureBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: textureBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm
                    AdjustmentSlider {
                        objectName: "detailFilmGrainSlider"
                        Layout.fillWidth: true
                        model: filmGrainModel
                        flickable: detailScroll
                    }
                    AdjustmentSlider {
                        objectName: "detailHalationSlider"
                        Layout.fillWidth: true
                        model: halationModel
                        flickable: detailScroll
                    }
                }
            }
        }
    }
}
