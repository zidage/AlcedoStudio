import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Phase 6B Tone panel: exposure / contrast / highlights / shadows / whites /
// blacks + the Tone Curve section (EditorToneCurveSection, #890). Values and curve points submit
// through the
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
        // Neutral values from EditorParameterCatalog (the models read their range from it).
        exposureModel.value = exposureModel.defaultValue
        contrastModel.value = contrastModel.defaultValue
        highlightsModel.value = highlightsModel.defaultValue
        shadowsModel.value = shadowsModel.defaultValue
        whitesModel.value = whitesModel.defaultValue
        blacksModel.value = blacksModel.defaultValue
        wireEnabled()
        // Bootstrap when the stack has not yet projected (tests / late session).
        // Equal values no-op if the stack already applied the same snapshot.
        loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }


    /// Phase 6C-7: load tone adjustment values from a snapshot map produced
    /// by EditorSessionController. Idempotent: uses plain setters (no submit)
    /// and only updates values that differ. Re-applying the same snapshot
    /// produces no commit, render request, timer restart, or focus change.
    function loadFromSnapshot(snapshot) {
        if (snapshot === undefined || snapshot === null) return
        for (var i = 0; i < root.toneModels.length; ++i)
            root.toneModels[i].loadFromSnapshot(snapshot)
        loadCurveFromSnapshot(curveModel, "curve", snapshot)
    }

    /// Load the four curves from the snapshot (load-only, no write):
    ///   snapshot.curve = { curve: { points: [{x,y}, ...], red: [...], green: [...], blue: [...] } }
    /// The selected channel is panel state and stays.
    function loadCurveFromSnapshot(model, fieldKey, snapshot) {
        if (!model || !fieldKey || !snapshot) return
        const entry = snapshot[fieldKey]
        if (entry === undefined || entry === null) return
        model.loadFromSnapshotEntry(entry)
    }

    EditorAdjustmentValueModel {
        id: exposureModel
        objectName: "toneExposureModel"
        fieldKey: "exposure"
        label: qsTr("Exposure")
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: contrastModel
        objectName: "toneContrastModel"
        fieldKey: "contrast"
        label: qsTr("Contrast")
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: highlightsModel
        objectName: "toneHighlightsModel"
        fieldKey: "highlights"
        label: qsTr("Highlights")
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: shadowsModel
        objectName: "toneShadowsModel"
        fieldKey: "shadows"
        label: qsTr("Shadows")
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: whitesModel
        objectName: "toneWhitesModel"
        fieldKey: "white"
        label: qsTr("Whites")
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: blacksModel
        objectName: "toneBlacksModel"
        fieldKey: "black"
        label: qsTr("Blacks")
        submitter: root.editorSession
    }
    EditorToneCurveModel {
        id: curveModel
        objectName: "toneCurveModel"
        fieldKey: "curve"
        label: qsTr("Tone Curve")
        submitter: root.editorSession
    }

    // Curve pointer drag must lock the parent Flickable the same way
    // AdjustmentSlider / Look CDL do — otherwise vertical handle motion scrolls
    // the tone panel instead of shaping the curve.
    Connections {
        target: curveModel
        function onDragActiveChanged() {
            if (!toneScroll)
                return
            if (curveModel.dragActive)
                toneScroll.beginInputLock()
            else
                toneScroll.endInputLock()
        }
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

            EditorToneCurveSection {
                id: curveGroup
                Layout.fillWidth: true
                model: curveModel
                editorSession: root.editorSession
                panelVisible: root.visible
                controlsEnabled: root.controlsEnabled
                surfaceColor: root.colCardSurface
                disabledSurfaceColor: root.colCardSurface
                borderColor: root.colCardBorder
                textColor: root.colText
                mutedColor: root.colMuted
                hoverColor: root.colHover
                accentColor: root.colAccent
            }
        }
    }
}
