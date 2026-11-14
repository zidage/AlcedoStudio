import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Color Grade Look panel: global color amount, selective HSL, tone color wheels
// (Shadows / Midtones / Highlights / Offset in a 2x2 grid). Clarity, Sharpen, Film Grain, Halation, and
// Diffusion live on the Post Processing page (EditorPostProcessPanel.qml).
// Camera white balance lives on the RAW Decode page; the Color section holds the
// CAT02 grade white balance. LUT strength is EditorLutControlPanel.qml; choosing a LUT is
// the Editor rail's LUT browser (EditorLutBrowserPanel.qml).
Item {
    id: root
    objectName: "editorAdjustmentPanel_look"

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
    readonly property color colFill: "#D8D4CD"
    // Slider ranges of the HLS and color wheel controls, in UI units.
    readonly property var hlsHueShiftSpec: parameterCatalog.propertySpec("hls", "hue_shift")
    /// The Grade kernels (ApplyHls in primary_grade.cl / .metal / cuda_primary_grade_pass.cu)
    /// turn the YIQ chroma by 2.25 x the Hue Shift value: +30 moves the hue by about 65 degrees.
    readonly property real hlsHueShiftAngleScale: 2.25
    readonly property var hlsLightnessSpec: parameterCatalog.propertySpec("hls", "lightness")
    readonly property var hlsChromaSpec: parameterCatalog.propertySpec("hls", "chroma")
    readonly property var hlsSmoothnessSpec: parameterCatalog.propertySpec("hls", "hue_smoothness")
    readonly property var hlsDefault: parameterCatalog.uiDefault("hls")
    readonly property var cdlMasterSpec: parameterCatalog.propertySpec("color_wheel", "lift_master")

    EditorParameterCatalog {
        id: parameterCatalog
        objectName: "lookParameterCatalog"
    }

    /// Selective Color rail color: the selected hue turned by @p hueOffset degrees, at HSL
    /// @p saturation and @p lightness. The hue swatches are hsl(hue, 1, 0.5).
    function hlsRailColor(hueOffset, saturation, lightness) {
        const hue = ((hlsModel.targetHue + hueOffset) % 360 + 360) % 360
        return Qt.hsla(hue / 360, saturation, lightness, 1)
    }

    function wireEnabled() {
        const on = root.controlsEnabled
        gradeWhiteBalanceModel.enabled = on
        saturationModel.enabled = on
        vibranceModel.enabled = on
        hlsModel.enabled = on
        cdlModel.enabled = on
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
        if (snapshot.grade_white_balance !== undefined)
            gradeWhiteBalanceModel.loadFromSnapshot(snapshot)
        saturationModel.loadFromSnapshot(snapshot)
        vibranceModel.loadFromSnapshot(snapshot)
        loadHlsFromSnapshot(snapshot)
        loadCdlFromSnapshot(snapshot)
    }

    function loadHlsFromSnapshot(snapshot) {
        if (!snapshot || snapshot.hls === undefined && snapshot.HLS === undefined)
            return
        // Continuous HLS drag must not be aborted by a settled snapshot echo.
        if (hlsModel.dragActive)
            return
        const entry = snapshot.hls !== undefined ? snapshot.hls : snapshot.HLS
        if (!entry)
            return
        // The catalog reads the Model tables in UI units (the Model stores lightness and
        // chroma divided by the panel scale).
        const hls = parameterCatalog.uiValue("hls", entry)
        if (hls.hue === undefined)
            return
        var uiTable = []
        for (var i = 0; i < hls.hue_shift.length; ++i)
            uiTable.push([hls.hue_shift[i], hls.lightness[i], hls.chroma[i]])
        hlsModel.loadFromTables(uiTable, hls.hue_smoothness, hls.hue)
    }

    function loadCdlFromSnapshot(snapshot) {
        if (!snapshot || snapshot.color_wheel === undefined)
            return
        if (cdlModel.dragActive)
            return
        const entry = snapshot.color_wheel
        const cw = entry.color_wheel !== undefined ? entry.color_wheel : entry
        if (!cw)
            return
        function applyWheel(name, obj) {
            if (!obj)
                return
            const disc = obj.disc || {}
            cdlModel.setWheelDisc(name, Number(disc.x || 0), Number(disc.y || 0))
            if (obj.luminance_offset !== undefined)
                cdlModel.setWheelMaster(name, Number(obj.luminance_offset))
        }
        applyWheel("lift", cw.lift)
        applyWheel("gamma", cw.gamma)
        applyWheel("gain", cw.gain)
        applyWheel("offset", cw.offset)
    }

    EditorGradeWhiteBalanceModel {
        id: gradeWhiteBalanceModel
        objectName: "lookGradeWhiteBalanceModel"
        submitter: root.editorSession
    }

    EditorAdjustmentValueModel {
        id: saturationModel
        objectName: "lookSaturationModel"
        fieldKey: "saturation"
        label: qsTr("Saturation")
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: vibranceModel
        objectName: "lookVibranceModel"
        fieldKey: "vibrance"
        label: qsTr("Vibrance")
        submitter: root.editorSession
    }
    EditorHlsModel {
        id: hlsModel
        objectName: "lookHlsModel"
        submitter: root.editorSession
    }
    EditorCdlTrackballModel {
        id: cdlModel
        objectName: "lookCdlModel"
        submitter: root.editorSession
    }
    component SectionShell: CollapsibleSection {
        Layout.fillWidth: true
        controlsEnabled: root.controlsEnabled
        surfaceColor: root.colCardSurface
        disabledSurfaceColor: root.colCardSurface
        borderColor: root.colCardBorder
        textColor: root.colText
        mutedColor: root.colMuted
        hoverColor: root.colHover
        accentColor: root.colFill
    }

    // Flat monochrome action chip (Reset wheels).
    component ActionChip: Rectangle {
        id: action
        property string label: ""
        property bool chipEnabled: true
        signal activated()

        implicitWidth: actionLabel.implicitWidth + 18
        implicitHeight: 26
        radius: 6
        color: root.colBase
        border.width: 1
        border.color: root.colCardBorder
        opacity: chipEnabled ? 1.0 : 0.45

        Text {
            id: actionLabel
            anchors.centerIn: parent
            text: action.label
            color: root.colText
            font.pixelSize: appTheme.fontSizeCaption
        }
        MouseArea {
            anchors.fill: parent
            enabled: action.chipEnabled
            cursorShape: Qt.PointingHandCursor
            onClicked: action.activated()
        }
    }

    // Master value of one wheel, in stops.
    component MasterSlider: ColumnLayout {
        id: masterRoot
        property string wheelRole: "lift"
        property real value: 0
        Layout.fillWidth: true
        spacing: 2

        EditorDragSlider {
            objectName: "lookCdlMasterSlider_" + masterRoot.wheelRole
            partName: "lookCdlMaster"
            label: qsTr("Master")
            infoText: qsTr("Changes the brightness of this tonal range, in stops (EV).")
            valueFieldWidth: 52
            spacing: 6
            from: root.cdlMasterSpec.ui_min
            to: root.cdlMasterSpec.ui_max
            stepSize: root.cdlMasterSpec.ui_step
            precision: root.cdlMasterSpec.ui_decimals
            defaultValue: 0
            value: masterRoot.value
            enabled: root.controlsEnabled
            flickable: lookScroll
            beginEdit: function () { cdlModel.beginMasterDrag(masterRoot.wheelRole) }
            updateEdit: function (v) { cdlModel.updateMasterDrag(masterRoot.wheelRole, v) }
            finishEdit: function () { cdlModel.finishMasterDrag() }
        }
    }

    component WheelColumn: ColumnLayout {
        id: wheelCol
        property string title: ""
        property string hint: ""
        property string role: "lift"
        property string deltaText: ""
        property real masterValue: 0
        property int discSize: 120
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        spacing: 6

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: appTheme.spaceXs
            Text {
                text: wheelCol.title
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
            }
            EditorInfoHint {
                text: wheelCol.hint
            }
        }
        EditorCdlTrackballItem {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: wheelCol.discSize
            Layout.preferredHeight: wheelCol.discSize
            Layout.maximumWidth: wheelCol.discSize
            Layout.maximumHeight: wheelCol.discSize
            model: cdlModel
            wheelRole: wheelCol.role
            backgroundColor: root.colBase
            rimColor: root.colCardBorder
            crosshairColor: root.colMuted
            handleColor: root.colFill
            handleOutlineColor: root.colText
            antialiasing: true
            smooth: true
        }
        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: wheelCol.deltaText
            color: root.colMuted
            font.pixelSize: appTheme.fontSizeCaption
        }
        MasterSlider {
            wheelRole: wheelCol.role
            value: wheelCol.masterValue
        }
    }

    // CDL disc/master drags must not scroll the panel (refcount via lookScroll).
    Connections {
        target: cdlModel
        function onDragActiveChanged() {
            if (!lookScroll)
                return
            if (cdlModel.dragActive)
                lookScroll.beginInputLock()
            else
                lookScroll.endInputLock()
        }
    }

    Flickable {
        id: lookScroll
        objectName: "editorLookPanelScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: lookColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        // Press-delay gives child sliders first chance at the grab before flick.
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

        // Wheel over child sliders must still scroll the panel.
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            grabPermissions: PointerHandler.CanTakeOverFromItems
                             | PointerHandler.CanTakeOverFromHandlersOfDifferentType
                             | PointerHandler.ApprovesTakeOverByAnything
            onWheel: function (event) {
                var step = event.pixelDelta.y !== 0
                           ? event.pixelDelta.y
                           : event.angleDelta.y / 120 * 48
                var maxY = Math.max(0, lookScroll.contentHeight - lookScroll.height)
                lookScroll.contentY = Math.max(0, Math.min(maxY, lookScroll.contentY - step))
                event.accepted = true
            }
        }

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
            padding: 0
        }

        ColumnLayout {
            id: lookColumn
            width: lookScroll.width
            spacing: 10

            Text {
                Layout.fillWidth: true
                text: qsTr("Look")
                color: root.colText
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightHeading
            }

            // ── Color amount ──────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_color"
                title: qsTr("Color")
                expanded: true
                bodyContentHeight: colorBody.implicitHeight + 8

                ColumnLayout {
                    id: colorBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceXs
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("White Balance")
                            color: root.colText
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightHeading
                        }
                        EditorInfoHint {
                            objectName: "lookGradeWhiteBalanceInfoHint"
                            text: qsTr("This white balance is a CAT02 chromatic adaptation in the grading color space. It assumes the image is already balanced to the ACES AP1 white point (about 6000 K), so the default is no change. It does not read the camera's data: use RAW Decode white balance to correct the capture, and this control for creative warming or cooling, including on JPEG and other non-RAW images. The same Kelvin value can look different here than on the RAW Decode page.")
                        }
                    }

                    EditorWhiteBalanceSliders {
                        Layout.fillWidth: true
                        flickable: lookScroll
                        slidersEnabled: root.controlsEnabled
                        textColor: root.colText
                        mutedColor: root.colMuted
                        temperatureObjectName: "lookGradeTemperatureSlider"
                        tintObjectName: "lookGradeTintSlider"
                        fieldKey: "grade_white_balance"
                        temperatureProperty: "temperature"
                        temperature: gradeWhiteBalanceModel.temperature
                        temperatureSliderPos: gradeWhiteBalanceModel.temperatureSliderPos
                        tint: gradeWhiteBalanceModel.tint
                        temperatureDefault: gradeWhiteBalanceModel.defaultTemperature
                        tintDefault: gradeWhiteBalanceModel.defaultTint
                        onTemperatureDragBegin: gradeWhiteBalanceModel.beginTemperatureDrag()
                        onTemperatureDragUpdate: function (pos) {
                            gradeWhiteBalanceModel.updateTemperatureSliderDrag(pos)
                        }
                        onTemperatureDragFinish: gradeWhiteBalanceModel.finishTemperatureDrag()
                        onTemperatureReset: gradeWhiteBalanceModel.resetTemperature()
                        onTintDragBegin: gradeWhiteBalanceModel.beginTintDrag()
                        onTintDragUpdate: function (value) { gradeWhiteBalanceModel.updateTintDrag(value) }
                        onTintDragFinish: gradeWhiteBalanceModel.finishTintDrag()
                        onTintReset: gradeWhiteBalanceModel.resetTint()
                    }

                    AdjustmentSlider {
                        objectName: "lookSaturationSlider"
                        Layout.fillWidth: true
                        model: saturationModel
                        flickable: lookScroll
                    }
                    AdjustmentSlider {
                        objectName: "lookVibranceSlider"
                        Layout.fillWidth: true
                        model: vibranceModel
                        flickable: lookScroll
                    }
                }
            }

            // ── Selective HSL ─────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_hls"
                title: qsTr("Selective Color")
                expanded: true
                bodyContentHeight: hlsBody.implicitHeight + 8

                ColumnLayout {
                    id: hlsBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Target Hue: %1°").arg(Math.round(hlsModel.targetHue))
                        color: root.colText
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    Row {
                        objectName: "lookHlsHueStrip"
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: hlsModel.hueSwatches
                            delegate: Rectangle {
                                width: 22
                                height: 22
                                radius: 11
                                color: modelData.color
                                border.width: hlsModel.activeHueIndex === modelData.index ? 2 : 1
                                border.color: hlsModel.activeHueIndex === modelData.index
                                              ? root.colFill : root.colCardBorder
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: root.controlsEnabled
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: hlsModel.selectHueIndex(modelData.index)
                                }
                            }
                        }
                    }

                    // The rails show what each slider does to the selected hue: the hue
                    // that each Hue Shift gives (a positive shift raises the hue, red
                    // toward yellow), dark to light for Lightness, grey to full color for
                    // Chroma.
                    EditorDragSlider {
                        objectName: "lookHlsHueShiftSlider"
                        partName: "lookHlsHueShift"
                        label: qsTr("Hue Shift")
                        from: root.hlsHueShiftSpec.ui_min
                        to: root.hlsHueShiftSpec.ui_max
                        stepSize: root.hlsHueShiftSpec.ui_step
                        precision: root.hlsHueShiftSpec.ui_decimals
                        suffix: "°"
                        defaultValue: 0
                        value: hlsModel.hueShift
                        enabled: root.controlsEnabled
                        flickable: lookScroll
                        railGradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: root.hlsRailColor(root.hlsHueShiftSpec.ui_min * root.hlsHueShiftAngleScale, 1.0, 0.5) }
                            GradientStop { position: 0.25; color: root.hlsRailColor(root.hlsHueShiftSpec.ui_min / 2 * root.hlsHueShiftAngleScale, 1.0, 0.5) }
                            GradientStop { position: 0.5; color: root.hlsRailColor(0, 1.0, 0.5) }
                            GradientStop { position: 0.75; color: root.hlsRailColor(root.hlsHueShiftSpec.ui_max / 2 * root.hlsHueShiftAngleScale, 1.0, 0.5) }
                            GradientStop { position: 1.0; color: root.hlsRailColor(root.hlsHueShiftSpec.ui_max * root.hlsHueShiftAngleScale, 1.0, 0.5) }
                        }
                        beginEdit: function () { hlsModel.beginHueShiftDrag() }
                        updateEdit: function (v) { hlsModel.updateHueShiftDrag(v) }
                        finishEdit: function () { hlsModel.finishHueShiftDrag() }
                        resetEdit: function () { hlsModel.resetActiveField("hueShift") }
                    }
                    EditorDragSlider {
                        objectName: "lookHlsLightnessSlider"
                        partName: "lookHlsLightness"
                        label: qsTr("Lightness")
                        from: root.hlsLightnessSpec.ui_min
                        to: root.hlsLightnessSpec.ui_max
                        stepSize: root.hlsLightnessSpec.ui_step
                        precision: root.hlsLightnessSpec.ui_decimals
                        defaultValue: 0
                        value: hlsModel.lightness
                        enabled: root.controlsEnabled
                        flickable: lookScroll
                        railGradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: root.hlsRailColor(0, 1.0, 0.1) }
                            GradientStop { position: 0.5; color: root.hlsRailColor(0, 1.0, 0.5) }
                            GradientStop { position: 1.0; color: root.hlsRailColor(0, 1.0, 0.9) }
                        }
                        beginEdit: function () { hlsModel.beginLightnessDrag() }
                        updateEdit: function (v) { hlsModel.updateLightnessDrag(v) }
                        finishEdit: function () { hlsModel.finishLightnessDrag() }
                        resetEdit: function () { hlsModel.resetActiveField("lightness") }
                    }
                    EditorDragSlider {
                        objectName: "lookHlsChromaSlider"
                        partName: "lookHlsChroma"
                        label: qsTr("Chroma")
                        from: root.hlsChromaSpec.ui_min
                        to: root.hlsChromaSpec.ui_max
                        stepSize: root.hlsChromaSpec.ui_step
                        precision: root.hlsChromaSpec.ui_decimals
                        defaultValue: 0
                        value: hlsModel.chroma
                        enabled: root.controlsEnabled
                        flickable: lookScroll
                        railGradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: root.hlsRailColor(0, 0.0, 0.5) }
                            GradientStop { position: 0.5; color: root.hlsRailColor(0, 0.55, 0.5) }
                            GradientStop { position: 1.0; color: root.hlsRailColor(0, 1.0, 0.5) }
                        }
                        beginEdit: function () { hlsModel.beginChromaDrag() }
                        updateEdit: function (v) { hlsModel.updateChromaDrag(v) }
                        finishEdit: function () { hlsModel.finishChromaDrag() }
                        resetEdit: function () { hlsModel.resetActiveField("chroma") }
                    }
                    EditorDragSlider {
                        objectName: "lookHlsSmoothnessSlider"
                        partName: "lookHlsSmoothness"
                        label: qsTr("Hue Smoothness")
                        from: root.hlsSmoothnessSpec.ui_min
                        to: root.hlsSmoothnessSpec.ui_max
                        stepSize: root.hlsSmoothnessSpec.ui_step
                        precision: root.hlsSmoothnessSpec.ui_decimals
                        suffix: "°"
                        defaultValue: root.hlsDefault.hue_smoothness
                                      ? root.hlsDefault.hue_smoothness[0] : from
                        value: hlsModel.hueSmoothness
                        enabled: root.controlsEnabled
                        flickable: lookScroll
                        beginEdit: function () { hlsModel.beginHueSmoothnessDrag() }
                        updateEdit: function (v) { hlsModel.updateHueSmoothnessDrag(v) }
                        finishEdit: function () { hlsModel.finishHueSmoothnessDrag() }
                        resetEdit: function () { hlsModel.resetActiveField("hueSmoothness") }
                    }
                }
            }

            // ── CDL — original triangle layout ────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_cdl"
                title: qsTr("Color Wheels")
                expanded: true
                bodyContentHeight: cdlBody.implicitHeight + 8

                ColumnLayout {
                    id: cdlBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    spacing: 10

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 10
                        rowSpacing: 12

                        WheelColumn {
                            objectName: "lookCdlLiftColumn"
                            title: qsTr("Shadows (Lift)")
                            hint: qsTr("Changes mostly the color and brightness of the shadows. The effect is largest near black, and the highlights stay almost the same.")
                            role: "lift"
                            deltaText: cdlModel.liftDeltaText
                            masterValue: cdlModel.liftMaster
                        }
                        WheelColumn {
                            objectName: "lookCdlGammaColumn"
                            title: qsTr("Midtones (Gamma)")
                            hint: qsTr("Changes mostly the midtones. The effect is largest near middle grey, and the darkest and brightest areas stay almost the same.")
                            role: "gamma"
                            deltaText: cdlModel.gammaDeltaText
                            masterValue: cdlModel.gammaMaster
                        }
                        WheelColumn {
                            objectName: "lookCdlGainColumn"
                            title: qsTr("Highlights (Gain)")
                            hint: qsTr("Changes mostly the color and brightness of the highlights. The effect is largest near white, and the shadows stay almost the same.")
                            role: "gain"
                            deltaText: cdlModel.gainDeltaText
                            masterValue: cdlModel.gainMaster
                        }
                        WheelColumn {
                            objectName: "lookCdlOffsetColumn"
                            title: qsTr("Offset")
                            hint: qsTr("Changes the color and brightness of all tones by the same amount, as an exposure change per color channel.")
                            role: "offset"
                            deltaText: cdlModel.offsetDeltaText
                            masterValue: cdlModel.offsetMaster
                        }
                    }

                    // Double-click any disc still resets that wheel; whole-panel
                    // reset remains available as a quiet text action.
                    ActionChip {
                        Layout.alignment: Qt.AlignRight
                        label: qsTr("Reset wheels")
                        chipEnabled: root.controlsEnabled
                        onActivated: cdlModel.resetAll()
                    }
                }
            }

            // Preserve fold objectName for Look panel selection tests.
            Item {
                objectName: "editorAdjustmentGroupShell_look"
                width: 0
                height: 0
                visible: false
            }
        }
    }
}
