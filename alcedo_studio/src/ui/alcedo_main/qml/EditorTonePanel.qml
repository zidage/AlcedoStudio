import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Phase 6B Tone panel: exposure / contrast / highlights / shadows / whites /
// blacks + tone-curve editor. Values and curve points submit through the
// EditorSessionController submitter seam (operator-shaped params JSON). Models
// own pointer-drag state (one settled transaction per completed drag).
// No pipeline/scheduler calls from this file.
Item {
    id: root
    objectName: "editorAdjustmentPanel_tone"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true

    // Prefer appTheme tokens so panel chrome tracks the active theme even when
    // the optional `theme` bag is incomplete or uses stale aliases.
    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colAccent: appTheme.accentColor
    readonly property color colCardSurface: appTheme.cardSurfaceColor
    readonly property color colCardBorder: appTheme.cardBorderColor
    readonly property color colBase: appTheme.bgBaseColor
    readonly property color colHover: appTheme.hoverColor

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
        loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }


    /// Phase 6C-7: load tone adjustment values from a snapshot map produced
    /// by EditorSessionController. Idempotent: uses plain setters (no submit)
    /// and only updates values that differ. Re-applying the same snapshot
    /// produces no commit, render request, timer restart, or focus change.
    function loadFromSnapshot(snapshot) {
        if (snapshot === undefined || snapshot === null) return
        loadModelFromSnapshot(exposureModel, "exposure", snapshot)
        loadModelFromSnapshot(contrastModel, "contrast", snapshot)
        loadModelFromSnapshot(highlightsModel, "highlights", snapshot)
        loadModelFromSnapshot(shadowsModel, "shadows", snapshot)
        loadModelFromSnapshot(whitesModel, "white", snapshot)
        loadModelFromSnapshot(blacksModel, "black", snapshot)
        loadCurveFromSnapshot(curveModel, "curve", snapshot)
    }

    /// Set a numeric model value from a snapshot entry. The snapshot stores
    /// operator-shaped params as a QVariantMap, e.g. {"exposure": 1.5}.
    /// The key inside the params map matches the fieldKey.
    function loadModelFromSnapshot(model, fieldKey, snapshot) {
        if (!model || !fieldKey || !snapshot) return
        // In-flight pointer drag owns the value; snapshot echo must not fight it
        // (multi-slider handoff / settled publish while another slider is live).
        if (model.dragActive) return
        const entry = snapshot[fieldKey]
        if (entry === undefined) return
        // entry is a QVariantMap like {"exposure": 1.5}
        const val = entry[fieldKey]
        if (val === undefined) return
        const num = Number(val)
        if (isNaN(num)) return
        // Plain setter — no submit, no render request.
        // Only assign if the value actually differs to avoid noise.
        if (Math.abs(model.value - num) > (model.step * 0.1)) {
            model.value = num
        }
    }

    /// Load curve control points from the snapshot.
    /// Snapshot shape matches the operator params published by the session:
    ///   snapshot.curve = { curve: { size, points: [{x,y}, ...] } }
    function loadCurveFromSnapshot(model, fieldKey, snapshot) {
        if (!model || !fieldKey || !snapshot) return
        if (model.dragActive) return
        const entry = snapshot[fieldKey]
        if (entry === undefined || entry === null) return
        if (typeof model.loadFromSnapshotEntry === "function") {
            model.loadFromSnapshotEntry(entry)
            return
        }
        // Fallback for older fakes: unwrap nested {"curve":{…}} and setPoints.
        var curve = entry
        if (entry[fieldKey] !== undefined)
            curve = entry[fieldKey]
        const points = curve.points
        if (points === undefined) return
        var list = []
        for (var i = 0; i < points.length; ++i) {
            var p = points[i]
            if (p === undefined || p === null) continue
            var x = p.x !== undefined ? Number(p.x)
                  : (p[0] !== undefined ? Number(p[0]) : NaN)
            var y = p.y !== undefined ? Number(p.y)
                  : (p[1] !== undefined ? Number(p[1]) : NaN)
            if (!isFinite(x) || !isFinite(y)) continue
            list.push({ x: x, y: y })
        }
        if (list.length < 2) return
        model.setPoints(list)
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

    Flickable {
        id: toneScroll
        objectName: "editorTonePanelScroll"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: toneColumn.implicitHeight + appTheme.spaceMd
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick

        // Refcounted press locks from sliders/curve so nested unlock cannot leave
        // interactive permanently false (breaks wheel scroll after an edit).
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

        // Wheel over Controls.Slider children must still scroll the panel.
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            grabPermissions: PointerHandler.CanTakeOverFromItems
                             | PointerHandler.CanTakeOverFromHandlersOfDifferentType
                             | PointerHandler.ApprovesTakeOverByAnything
            onWheel: function (event) {
                var step = event.pixelDelta.y !== 0
                           ? event.pixelDelta.y
                           : event.angleDelta.y / 120 * 48
                var maxY = Math.max(0, toneScroll.contentHeight - toneScroll.height)
                toneScroll.contentY = Math.max(0, Math.min(maxY, toneScroll.contentY - step))
                event.accepted = true
            }
        }

        ColumnLayout {
            id: toneColumn
            width: toneScroll.width
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
                        flickable: toneScroll
                    }
                    AdjustmentSlider {
                        objectName: "toneContrastSlider"
                        Layout.fillWidth: true
                        model: contrastModel
                        flickable: toneScroll
                    }
                    AdjustmentSlider {
                        objectName: "toneHighlightsSlider"
                        Layout.fillWidth: true
                        model: highlightsModel
                        flickable: toneScroll
                    }
                    AdjustmentSlider {
                        objectName: "toneShadowsSlider"
                        Layout.fillWidth: true
                        model: shadowsModel
                        flickable: toneScroll
                    }
                    AdjustmentSlider {
                        objectName: "toneWhitesSlider"
                        Layout.fillWidth: true
                        model: whitesModel
                        flickable: toneScroll
                    }
                    AdjustmentSlider {
                        objectName: "toneBlacksSlider"
                        Layout.fillWidth: true
                        model: blacksModel
                        flickable: toneScroll
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
                        Layout.preferredHeight: 300
                        Layout.minimumHeight: 260
                        model: curveModel
                        // Monochrome only — same family as AdjustmentSlider (no accent gold/blue).
                        backgroundColor: appTheme.cardSurfaceColor
                        plotColor: appTheme.bgBaseColor
                        gridColor: appTheme.cardBorderColor
                        diagonalColor: appTheme.textMutedColor
                        curveColor: appTheme.editorSliderHandleColor
                        handleColor: appTheme.editorSliderHandleColor
                        handleActiveColor: appTheme.textColor
                        handleOutlineColor: appTheme.editorSliderHandleBorderColor
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
        }
    }
}
