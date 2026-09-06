import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Phase 6D Look panel — ergonomic IA over the production operator set:
// White Balance first, global color amount, selective HSL, original three-disc
// CDL trackballs (Gamma top / Lift+Gain bottom), detail + texture.
// LUT moved to its own standalone panel (LUTPanel.qml).
// Values submit through the EditorSessionController submitter seam only.
Item {
    id: root
    objectName: "editorAdjustmentPanel_look"

    property var theme: null
    property var editorSession: null
    property var lutModel: null
    property bool controlsEnabled: true

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colBase: theme ? theme.colBgBase : appTheme.bgBaseColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor
    // Monochrome slider chrome (shared with AdjustmentSlider).
    readonly property color colTrack: "#2C2D2F"
    readonly property color colFill: "#D8D4CD"
    readonly property color colHandle: "#F5F1EA"
    readonly property color colHandleBorder: "#1A1B1C"
    readonly property int handleSize: 22
    readonly property int sliderRowHeight: 32

    function wireEnabled() {
        const on = root.controlsEnabled
        colorTempModel.enabled = on
        saturationModel.enabled = on
        vibranceModel.enabled = on
        hlsModel.enabled = on
        cdlModel.enabled = on
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
        loadColorTempFromSnapshot(snapshot)
        loadModelFromSnapshot(saturationModel, "saturation", snapshot)
        loadModelFromSnapshot(vibranceModel, "vibrance", snapshot)
        loadHlsFromSnapshot(snapshot)
        loadCdlFromSnapshot(snapshot)
        loadModelFromSnapshot(clarityModel, "clarity", snapshot)
        loadSharpenFromSnapshot(snapshot)
        loadNestedStrength(filmGrainModel, "film_grain", "strength", snapshot)
        loadNestedStrength(halationModel, "halation", "strength", snapshot)
    }

    function loadModelFromSnapshot(model, fieldKey, snapshot) {
        if (!model || !fieldKey || !snapshot)
            return
        // In-flight pointer drag owns the value; snapshot echo must not fight it.
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

    function loadNestedStrength(model, fieldKey, nestedKey, snapshot) {
        if (!model || !snapshot)
            return
        // In-flight pointer drag owns the value; settled echo must not fight it.
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
        const num = Number(val)
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

    function loadColorTempFromSnapshot(snapshot) {
        if (!snapshot || snapshot.color_temp === undefined)
            return
        // Continuous CCT/tint drag must not be aborted by the interactive
        // snapshot echo (EditorAdjustmentStack onAdjustmentSnapshotChanged).
        if (colorTempModel.dragActive)
            return
        // Snapshot field is ColorTempOp::GetParams shape (via live GetOperator).
        // C++ owns key mapping: mode, custom_*, as_shot_* (+ legacy aliases).
        // Do not re-interpret cct/tint defaults here — that silently loads 6500/0
        // when only custom_*/as_shot_* are present and breaks as_shot ↔ custom.
        colorTempModel.loadFromOperatorParams(snapshot.color_temp)
    }

    function loadHlsFromSnapshot(snapshot) {
        if (!snapshot || snapshot.hls === undefined && snapshot.HLS === undefined)
            return
        // Continuous HLS drag must not be aborted by a settled snapshot echo.
        if (hlsModel.dragActive)
            return
        const entry = snapshot.hls !== undefined ? snapshot.hls : snapshot.HLS
        const hls = entry.HLS !== undefined ? entry.HLS : entry
        if (!hls)
            return
        const table = hls.hls_adj_table || []
        const ranges = hls.h_range_table || []
        // Operator stores L/S scaled by 1/1000; convert to UI units for load.
        var uiTable = []
        for (var i = 0; i < table.length; ++i) {
            var row = table[i]
            // Qt 6.9 QML does not fully unwrap nested QVariantList (from
            // BuildSnapshotMap) to native JS Arrays — Array.isArray(row) is
            // false even though row supports indexed access and has .length.
            // Check via typeof to handle both native Arrays and QVariantList.
            if (row && typeof row[0] !== "undefined" && typeof row[1] !== "undefined" && typeof row[2] !== "undefined") {
                uiTable.push([Number(row[0]), Number(row[1]) * 1000.0, Number(row[2]) * 1000.0])
            }
        }
        var target = 0
        if (hls.target_hls && hls.target_hls.length > 0)
            target = Number(hls.target_hls[0])
        hlsModel.loadFromTables(uiTable, ranges, target)
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
    }


    EditorColorTempModel {
        id: colorTempModel
        objectName: "lookColorTempModel"
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: saturationModel
        objectName: "lookSaturationModel"
        fieldKey: "saturation"
        label: qsTr("Saturation")
        minimum: -100
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: vibranceModel
        objectName: "lookVibranceModel"
        fieldKey: "vibrance"
        label: qsTr("Vibrance")
        minimum: -100
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
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
    EditorAdjustmentValueModel {
        id: clarityModel
        objectName: "lookClarityModel"
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
        objectName: "lookSharpenModel"
        fieldKey: "sharpen"
        label: qsTr("Sharpen")
        minimum: -100
        maximum: 100
        defaultValue: 0
        step: 1
        precision: 0
        submitter: root.editorSession
    }
    EditorAdjustmentValueModel {
        id: filmGrainModel
        objectName: "lookFilmGrainModel"
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
        objectName: "lookHalationModel"
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
        controlsEnabled: root.controlsEnabled
        surfaceColor: root.colCardSurface
        disabledSurfaceColor: root.colCardSurface
        borderColor: root.colCardBorder
        textColor: root.colText
        mutedColor: root.colMuted
        hoverColor: root.colHover
        accentColor: root.colFill
    }

    // Plain chip (replaces Material Button) for As Shot / Custom.
    component ModeChip: Rectangle {
        id: chip
        property string label: ""
        property bool selected: false
        property bool chipEnabled: true
        signal activated()

        implicitWidth: chipLabel.implicitWidth + 20
        implicitHeight: 26
        radius: 6
        color: selected ? root.colFill : root.colBase
        border.width: 1
        border.color: root.colCardBorder
        opacity: chipEnabled ? 1.0 : 0.45

        Text {
            id: chipLabel
            anchors.centerIn: parent
            text: chip.label
            color: chip.selected ? root.colBase : root.colText
            font.pixelSize: appTheme.fontSizeCaption
        }
        MouseArea {
            anchors.fill: parent
            enabled: chip.chipEnabled
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.activated()
        }
    }

    // Flat monochrome action chip (Refresh / Open folder / Reset wheels).
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

    // Generic monochrome slider for Look-only controls (CCT/tint/HSL/master).
    // Owns its value during press (no model→value binding fight). Double-click
    // without movement resets. Locks lookScroll while pressed. Pointer drag uses
    // the same decelerated relative mapping as AdjustmentSlider (pointerGain).
    component MonoSlider: Slider {
        id: mono
        property var gradientStops: null
        property var onBegin: function () {}
        property var onUpdate: function (v) {}
        property var onFinish: function () {}
        property var onReset: function () {}
        // External load value; applied only when not pressed.
        property real externalValue: 0
        /// Full-track mouse travel maps to this fraction of the value range.
        property real pointerGain: 0.32

        property bool _gestureMoved: false
        property real _pressValue: 0
        property real _pressLocalX: 0
        property double _lastClickMs: 0
        property bool _scrollLocked: false
        property bool _handleDragging: false

        from: 0
        to: 1
        stepSize: 1
        live: true
        touchDragThreshold: 0
        snapMode: Slider.SnapAlways
        padding: 0
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        // Controls.Slider absolute seek is disabled; MouseArea owns pointer drag.
        hoverEnabled: false
        Layout.fillWidth: true
        Layout.preferredHeight: root.sliderRowHeight
        implicitHeight: root.sliderRowHeight
        // Seed once via Component.onCompleted / externalValue — do not bind
        // `value: externalValue` permanently (that fights continuous drag and
        // can leave the handle stale after reset once the binding is broken).
        Component.onCompleted: value = externalValue

        onExternalValueChanged: {
            if (!_handleDragging)
                value = externalValue
        }

        function lockScroll(lock) {
            if (!lookScroll)
                return
            if (lock) {
                if (!_scrollLocked) {
                    lookScroll.beginInputLock()
                    _scrollLocked = true
                }
            } else if (_scrollLocked) {
                lookScroll.endInputLock()
                _scrollLocked = false
            }
        }

        function syncFromExternal() {
            value = externalValue
        }

        function handleCenterX() {
            return mono.leftPadding
                    + mono.visualPosition * (mono.availableWidth - root.handleSize)
                    + root.handleSize * 0.5
        }

        function isNearHandle(localX) {
            return Math.abs(localX - handleCenterX())
                    <= Math.max(root.handleSize * 0.5 + 12, 16)
        }

        function valueFromDragDelta(localX) {
            var trackW = Math.max(1e-6, mono.availableWidth - root.handleSize)
            var dx = localX - mono._pressLocalX
            var range = mono.to - mono.from
            var v = mono._pressValue + (dx / trackW) * range * mono.pointerGain
            if (mono.stepSize > 0)
                v = Math.round(v / mono.stepSize) * mono.stepSize
            return Math.max(mono.from, Math.min(mono.to, v))
        }

        function finishPointerGesture() {
            var now = Date.now()
            var isDouble = !_gestureMoved
                    && (now - _lastClickMs) < 350
                    && Math.abs(value - _pressValue) <= Math.max(stepSize * 0.5, 1e-9)
            _lastClickMs = now
            lockScroll(false)
            if (isDouble && enabled) {
                onReset()
                syncFromExternal()
            } else if (_handleDragging) {
                onFinish()
                syncFromExternal()
            }
            _handleDragging = false
        }

        background: Rectangle {
            readonly property real edge: root.handleSize * 0.5
            x: mono.leftPadding + edge
            y: mono.topPadding + mono.availableHeight / 2 - height / 2
            width: Math.max(0, mono.availableWidth - root.handleSize)
            height: 6
            radius: 3
            color: root.colTrack
            border.width: mono.gradientStops ? 1 : 0
            border.color: root.colCardBorder
            gradient: mono.gradientStops

            Rectangle {
                visible: mono.gradientStops === null
                x: 0
                y: 0
                width: mono.visualPosition * parent.width
                height: parent.height
                radius: 3
                color: root.colFill
            }
        }
        handle: Rectangle {
            x: mono.leftPadding + mono.visualPosition * (mono.availableWidth - width)
            y: mono.topPadding + mono.availableHeight / 2 - height / 2
            width: root.handleSize
            height: root.handleSize
            radius: width / 2
            color: root.colHandle
            border.width: 1
            border.color: root.colHandleBorder
        }

        MouseArea {
            anchors.fill: parent
            enabled: mono.enabled
            preventStealing: true
            hoverEnabled: false
            acceptedButtons: Qt.LeftButton
            cursorShape: mono._handleDragging ? Qt.ClosedHandCursor : Qt.ArrowCursor

            onPressed: function (mouse) {
                mono._gestureMoved = false
                mono._pressValue = mono.value
                mono._pressLocalX = mouse.x
                mono.lockScroll(true)
                if (mono.isNearHandle(mouse.x)) {
                    mono._handleDragging = true
                    mono.onBegin()
                } else {
                    mono._handleDragging = false
                }
                mouse.accepted = true
            }
            onPositionChanged: function (mouse) {
                if (!mono._handleDragging)
                    return
                mono._gestureMoved = true
                var v = mono.valueFromDragDelta(mouse.x)
                mono.value = v
                mono.onUpdate(v)
            }
            onReleased: function (/*mouse*/) {
                mono.finishPointerGesture()
            }
            onCanceled: {
                if (mono._handleDragging)
                    mono.onFinish()
                mono._handleDragging = false
                mono.lockScroll(false)
            }
        }
    }

    component MonoSliderRow: ColumnLayout {
        id: row
        property string title: ""
        property string valueText: ""
        property real from: 0
        property real to: 100
        property real value: 0
        property var gradientStops: null
        property bool rowEnabled: true
        property var onBegin: function () {}
        property var onUpdate: function (v) {}
        property var onFinish: function () {}
        property var onReset: function () {}

        Layout.fillWidth: true
        spacing: 2

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 16
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: row.title
                color: root.colText
                font.pixelSize: appTheme.fontSizeCaption
                elide: Text.ElideRight
            }
            Text {
                text: row.valueText
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
            }
        }
        MonoSlider {
            from: row.from
            to: row.to
            externalValue: row.value
            enabled: row.rowEnabled
            gradientStops: row.gradientStops
            onBegin: function () { row.onBegin() }
            onUpdate: function (v) { row.onUpdate(v) }
            onFinish: function () { row.onFinish() }
            onReset: function () { row.onReset() }
        }
    }

    component MasterSlider: ColumnLayout {
        id: masterRoot
        property string wheelRole: "lift"
        property int valueUi: 0
        Layout.fillWidth: true
        spacing: 2

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Master")
            color: root.colMuted
            font.pixelSize: appTheme.fontSizeCaption
        }
        MonoSlider {
            from: -800
            to: 800
            externalValue: masterRoot.valueUi
            enabled: root.controlsEnabled
            onBegin: function () { cdlModel.beginMasterDrag(masterRoot.wheelRole) }
            onUpdate: function (v) {
                cdlModel.updateMasterDragUi(masterRoot.wheelRole, Math.round(v))
            }
            onFinish: function () { cdlModel.finishMasterDrag() }
            onReset: function () {
                cdlModel.beginMasterDrag(masterRoot.wheelRole)
                cdlModel.updateMasterDragUi(masterRoot.wheelRole, 0)
                cdlModel.finishMasterDrag()
            }
        }
    }

    component WheelColumn: ColumnLayout {
        id: wheelCol
        property string title: ""
        property string role: "lift"
        property string deltaText: ""
        property int masterUi: 0
        property int discSize: 132
        Layout.fillWidth: true
        spacing: 6

        Text {
            Layout.alignment: Qt.AlignHCenter
            text: wheelCol.title
            color: root.colMuted
            font.pixelSize: appTheme.fontSizeCaption
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
            valueUi: wheelCol.masterUi
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

            // ── White Balance ─────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_wb"
                title: qsTr("White Balance")
                expanded: true
                bodyContentHeight: wbBody.implicitHeight + 8

                ColumnLayout {
                    id: wbBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        ModeChip {
                            label: qsTr("As Shot")
                            selected: colorTempModel.modeIndex === 0
                            chipEnabled: root.controlsEnabled && colorTempModel.supported
                            onActivated: colorTempModel.selectMode(0)
                        }
                        ModeChip {
                            label: qsTr("Custom")
                            selected: colorTempModel.modeIndex === 1
                            chipEnabled: root.controlsEnabled && colorTempModel.supported
                            onActivated: colorTempModel.selectMode(1)
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        visible: !colorTempModel.supported
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("White balance is unavailable for this image.")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    MonoSliderRow {
                        objectName: "lookCctSlider"
                        title: qsTr("Temperature")
                        valueText: Math.round(colorTempModel.cct) + " K"
                        from: 0
                        to: 4096
                        value: colorTempModel.cctSliderPos
                        rowEnabled: root.controlsEnabled && colorTempModel.supported
                        gradientStops: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: "#9BD8FF" }
                            GradientStop { position: 0.5; color: "#FFE8B0" }
                            GradientStop { position: 1.0; color: "#FF8A3D" }
                        }
                        onBegin: function () { colorTempModel.beginCctDrag() }
                        onUpdate: function (v) { colorTempModel.updateCctSliderDrag(Math.round(v)) }
                        onFinish: function () { colorTempModel.finishCctDrag() }
                        onReset: function () { colorTempModel.reset() }
                    }

                    MonoSliderRow {
                        objectName: "lookTintSlider"
                        title: qsTr("Tint")
                        valueText: String(Math.round(colorTempModel.tint))
                        from: -150
                        to: 150
                        value: colorTempModel.tint
                        rowEnabled: root.controlsEnabled && colorTempModel.supported
                        gradientStops: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: "#49C26D" }
                            GradientStop { position: 0.5; color: "#E6E6E6" }
                            GradientStop { position: 1.0; color: "#A85AE6" }
                        }
                        onBegin: function () { colorTempModel.beginTintDrag() }
                        onUpdate: function (v) { colorTempModel.updateTintDrag(v) }
                        onFinish: function () { colorTempModel.finishTintDrag() }
                        onReset: function () { colorTempModel.reset() }
                    }
                }
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

                    MonoSliderRow {
                        objectName: "lookHlsHueShiftSlider"
                        title: qsTr("Hue Shift")
                        valueText: Math.round(hlsModel.hueShift) + "°"
                        from: -30
                        to: 30
                        value: hlsModel.hueShift
                        rowEnabled: root.controlsEnabled
                        onBegin: function () { hlsModel.beginHueShiftDrag() }
                        onUpdate: function (v) { hlsModel.updateHueShiftDrag(v) }
                        onFinish: function () { hlsModel.finishHueShiftDrag() }
                        onReset: function () { hlsModel.resetActiveField("hueShift") }
                    }
                    MonoSliderRow {
                        objectName: "lookHlsLightnessSlider"
                        title: qsTr("Lightness")
                        valueText: String(Math.round(hlsModel.lightness))
                        from: -100
                        to: 100
                        value: hlsModel.lightness
                        rowEnabled: root.controlsEnabled
                        onBegin: function () { hlsModel.beginLightnessDrag() }
                        onUpdate: function (v) { hlsModel.updateLightnessDrag(v) }
                        onFinish: function () { hlsModel.finishLightnessDrag() }
                        onReset: function () { hlsModel.resetActiveField("lightness") }
                    }
                    MonoSliderRow {
                        objectName: "lookHlsChromaSlider"
                        title: qsTr("Chroma")
                        valueText: String(Math.round(hlsModel.chroma))
                        from: -100
                        to: 100
                        value: hlsModel.chroma
                        rowEnabled: root.controlsEnabled
                        onBegin: function () { hlsModel.beginChromaDrag() }
                        onUpdate: function (v) { hlsModel.updateChromaDrag(v) }
                        onFinish: function () { hlsModel.finishChromaDrag() }
                        onReset: function () { hlsModel.resetActiveField("chroma") }
                    }
                    MonoSliderRow {
                        objectName: "lookHlsSmoothnessSlider"
                        title: qsTr("Hue Smoothness")
                        valueText: Math.round(hlsModel.hueSmoothness) + "°"
                        from: 1
                        to: 180
                        value: hlsModel.hueSmoothness
                        rowEnabled: root.controlsEnabled
                        onBegin: function () { hlsModel.beginHueSmoothnessDrag() }
                        onUpdate: function (v) { hlsModel.updateHueSmoothnessDrag(v) }
                        onFinish: function () { hlsModel.finishHueSmoothnessDrag() }
                        onReset: function () { hlsModel.resetActiveField("hueSmoothness") }
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

                    WheelColumn {
                        objectName: "lookCdlGammaColumn"
                        title: qsTr("Gamma")
                        role: "gamma"
                        deltaText: cdlModel.gammaDeltaText
                        masterUi: cdlModel.gammaMasterUi
                        discSize: 148
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        WheelColumn {
                            objectName: "lookCdlLiftColumn"
                            title: qsTr("Lift")
                            role: "lift"
                            deltaText: cdlModel.liftDeltaText
                            masterUi: cdlModel.liftMasterUi
                            discSize: 128
                        }
                        WheelColumn {
                            objectName: "lookCdlGainColumn"
                            title: qsTr("Gain")
                            role: "gain"
                            deltaText: cdlModel.gainDeltaText
                            masterUi: cdlModel.gainMasterUi
                            discSize: 128
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

            // ── Detail ────────────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_detail"
                title: qsTr("Detail")
                expanded: false
                bodyContentHeight: detailBody.implicitHeight + 8

                ColumnLayout {
                    id: detailBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    spacing: 8
                    AdjustmentSlider {
                        objectName: "lookClaritySlider"
                        Layout.fillWidth: true
                        model: clarityModel
                        flickable: lookScroll
                    }
                    AdjustmentSlider {
                        objectName: "lookSharpenSlider"
                        Layout.fillWidth: true
                        model: sharpenModel
                        flickable: lookScroll
                    }
                }
            }

            // ── Texture ───────────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_texture"
                title: qsTr("Texture")
                expanded: false
                bodyContentHeight: textureBody.implicitHeight + 8

                ColumnLayout {
                    id: textureBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    spacing: 8
                    AdjustmentSlider {
                        objectName: "lookFilmGrainSlider"
                        Layout.fillWidth: true
                        model: filmGrainModel
                        flickable: lookScroll
                    }
                    AdjustmentSlider {
                        objectName: "lookHalationSlider"
                        Layout.fillWidth: true
                        model: halationModel
                        flickable: lookScroll
                    }
                }
            }


            // Preserve Phase 4C fold objectName for Look panel selection tests.
            Item {
                objectName: "editorAdjustmentGroupShell_look"
                width: 0
                height: 0
                visible: false
            }
        }
    }
}
