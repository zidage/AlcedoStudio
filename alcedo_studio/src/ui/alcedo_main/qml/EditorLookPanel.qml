import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Phase 6D Look panel — ergonomic IA over the production operator set:
// White Balance first, global color amount, selective HSL, original three-disc
// CDL trackballs (Gamma top / Lift+Gain bottom), detail + texture, then LUT.
// Values submit through the EditorSessionController submitter seam only.
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
    // Monochrome slider chrome (shared with AdjustmentSlider).
    readonly property color colTrack: "#2C2D2F"
    readonly property color colFill: "#D8D4CD"
    readonly property color colHandle: "#F5F1EA"
    readonly property color colHandleBorder: "#1A1B1C"
    readonly property int handleSize: 22
    readonly property int sliderRowHeight: 32

    function numericParams(key, v) {
        var o = {}
        o[key] = v
        return JSON.stringify(o)
    }

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
        lutModel.enabled = on
    }

    onControlsEnabledChanged: wireEnabled()
    Component.onCompleted: {
        wireEnabled()
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
        loadLutFromSnapshot(snapshot)
    }

    function loadModelFromSnapshot(model, fieldKey, snapshot) {
        if (!model || !fieldKey || !snapshot)
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
        const entry = snapshot.color_temp
        const ct = entry.color_temp !== undefined ? entry.color_temp : entry
        if (!ct)
            return
        const mode = ct.mode !== undefined ? String(ct.mode) : "as_shot"
        const cct = Number(ct.cct !== undefined ? ct.cct : 6500)
        const tint = Number(ct.tint !== undefined ? ct.tint : 0)
        const supported = ct.supported !== undefined ? !!ct.supported : true
        colorTempModel.loadFromParams(mode, cct, tint, supported)
        if (ct.resolved_cct !== undefined)
            colorTempModel.asShotCct = Number(ct.resolved_cct)
        if (ct.resolved_tint !== undefined)
            colorTempModel.asShotTint = Number(ct.resolved_tint)
    }

    function loadHlsFromSnapshot(snapshot) {
        if (!snapshot || snapshot.hls === undefined && snapshot.HLS === undefined)
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
            if (Array.isArray(row) && row.length >= 3) {
                uiTable.push([row[0], row[1] * 1000.0, row[2] * 1000.0])
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

    function loadLutFromSnapshot(snapshot) {
        if (!snapshot)
            return
        const entry = snapshot.lut !== undefined ? snapshot.lut : snapshot.ocio_lmt
        if (entry === undefined)
            return
        var path = ""
        if (typeof entry === "string")
            path = entry
        else if (entry.ocio_lmt !== undefined)
            path = String(entry.ocio_lmt)
        else if (entry.path !== undefined)
            path = String(entry.path)
        lutModel.setSelectedPath(path)
        lutModel.refresh(false)
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
        paramsBuilder: function (v) { return root.numericParams("saturation", v) }
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
        paramsBuilder: function (v) { return root.numericParams("vibrance", v) }
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
        paramsBuilder: function (v) { return root.numericParams("clarity", v) }
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
        paramsBuilder: function (v) {
            return JSON.stringify({ sharpen: { offset: v } })
        }
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
        paramsBuilder: function (v) {
            return JSON.stringify({ film_grain: { strength: v } })
        }
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
        paramsBuilder: function (v) {
            return JSON.stringify({ halation: { strength: v } })
        }
    }
    EditorLutCatalogModel {
        id: lutModel
        objectName: "lookLutModel"
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
    // without movement resets. Locks lookScroll while pressed.
    component MonoSlider: Slider {
        id: mono
        property var gradientStops: null
        property var onBegin: function () {}
        property var onUpdate: function (v) {}
        property var onFinish: function () {}
        property var onReset: function () {}
        // External load value; applied only when not pressed.
        property real externalValue: 0

        property bool _gestureMoved: false
        property real _pressValue: 0
        property double _lastClickMs: 0
        property var _savedFlickInteractive: null

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
        Layout.fillWidth: true
        Layout.preferredHeight: root.sliderRowHeight
        implicitHeight: root.sliderRowHeight
        // Seed once; then press owns the value until release.
        value: externalValue

        onExternalValueChanged: {
            if (!pressed)
                value = externalValue
        }

        function lockScroll(lock) {
            if (!lookScroll)
                return
            if (lock) {
                if (_savedFlickInteractive === null)
                    _savedFlickInteractive = lookScroll.interactive
                lookScroll.interactive = false
            } else if (_savedFlickInteractive !== null) {
                lookScroll.interactive = _savedFlickInteractive
                _savedFlickInteractive = null
            }
        }

        onPressedChanged: {
            if (pressed) {
                _gestureMoved = false
                _pressValue = value
                lockScroll(true)
                onBegin()
            } else {
                var now = Date.now()
                var isDouble = !_gestureMoved
                        && (now - _lastClickMs) < 350
                        && Math.abs(value - _pressValue) <= Math.max(stepSize * 0.5, 1e-9)
                _lastClickMs = now
                onFinish()
                lockScroll(false)
                if (isDouble && enabled)
                    onReset()
            }
        }
        onMoved: {
            _gestureMoved = true
            onUpdate(value)
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

    // CDL disc/master drags must not scroll the panel.
    property var _cdlSavedFlickInteractive: null
    Connections {
        target: cdlModel
        function onDragActiveChanged() {
            if (cdlModel.dragActive) {
                if (root._cdlSavedFlickInteractive === null)
                    root._cdlSavedFlickInteractive = lookScroll.interactive
                lookScroll.interactive = false
            } else if (root._cdlSavedFlickInteractive !== null) {
                lookScroll.interactive = root._cdlSavedFlickInteractive
                root._cdlSavedFlickInteractive = null
            }
        }
    }

    Flickable {
        id: lookScroll
        anchors.fill: parent
        contentWidth: width
        contentHeight: lookColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        // Press-delay gives child sliders first chance at the grab before flick.
        pressDelay: 0
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

            // ── LUT ───────────────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_lut"
                title: qsTr("LUT")
                expanded: true
                bodyContentHeight: lutBody.implicitHeight + 8

                ColumnLayout {
                    id: lutBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: lutModel.directoryText
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                        elide: Text.ElideMiddle
                    }
                    Text {
                        Layout.fillWidth: true
                        text: lutModel.statusText
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    TextField {
                        id: lutFilter
                        objectName: "lookLutFilterField"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 28
                        padding: 6
                        placeholderText: qsTr("Filter LUTs")
                        color: root.colText
                        placeholderTextColor: root.colMuted
                        enabled: root.controlsEnabled
                        onTextChanged: lutModel.filterText = text
                        background: Rectangle {
                            color: root.colBase
                            border.width: 1
                            border.color: root.colCardBorder
                            radius: 6
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        ActionChip {
                            label: qsTr("Refresh")
                            chipEnabled: root.controlsEnabled
                            onActivated: lutModel.refresh(true)
                        }
                        ActionChip {
                            label: qsTr("Open folder")
                            chipEnabled: root.controlsEnabled && lutModel.canOpenDirectory
                            onActivated: Qt.openUrlExternally("file:///" + lutModel.directoryPath())
                        }
                        Item { Layout.fillWidth: true }
                    }

                    ListView {
                        id: lutList
                        objectName: "lookLutList"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(200, Math.max(72, count * 36))
                        clip: true
                        model: lutModel.entries
                        spacing: 2
                        boundsBehavior: Flickable.StopAtBounds
                        delegate: Rectangle {
                            width: lutList.width
                            height: 34
                            radius: 6
                            color: modelData.selected ? root.colFill : root.colBase
                            border.color: root.colCardBorder
                            opacity: modelData.selectable ? 1.0 : 0.5
                            Column {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 0
                                Text {
                                    width: parent.width
                                    text: modelData.displayName
                                    color: modelData.selected ? root.colBase : root.colText
                                    elide: Text.ElideRight
                                    font.pixelSize: appTheme.fontSizeBody
                                }
                                Text {
                                    width: parent.width
                                    text: modelData.secondaryText || modelData.statusText || ""
                                    color: modelData.selected ? root.colBase : root.colMuted
                                    elide: Text.ElideRight
                                    font.pixelSize: appTheme.fontSizeCaption
                                    visible: text.length > 0
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: root.controlsEnabled && modelData.selectable
                                onClicked: lutModel.selectPath(modelData.path)
                            }
                        }
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
