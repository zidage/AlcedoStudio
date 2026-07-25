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
        id: section
        Layout.fillWidth: true
        controlsEnabled: root.controlsEnabled
        surfaceColor: root.colCardSurface
        disabledSurfaceColor: root.colCardSurface
        borderColor: root.colCardBorder
        textColor: root.colText
        mutedColor: root.colMuted
        hoverColor: root.colHover
        accentColor: root.colAccent
    }

    component MasterSlider: Item {
        id: masterRoot
        property string wheelRole: "lift"
        property int valueUi: 0
        property string labelText: ""
        height: 28
        Layout.fillWidth: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 2
            Label {
                Layout.fillWidth: true
                text: masterRoot.labelText
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
                horizontalAlignment: Text.AlignHCenter
            }
            Slider {
                id: masterSlider
                Layout.fillWidth: true
                from: -800
                to: 800
                stepSize: 1
                value: masterRoot.valueUi
                enabled: root.controlsEnabled
                onPressedChanged: {
                    if (pressed)
                        cdlModel.beginMasterDrag(masterRoot.wheelRole)
                    else
                        cdlModel.finishMasterDrag()
                }
                onMoved: cdlModel.updateMasterDragUi(masterRoot.wheelRole, Math.round(value))
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
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: lookColumn
            width: lookScroll.width
            spacing: appTheme.spaceSm

            Label {
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
                bodyContentHeight: wbBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: wbBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceXs
                        Repeater {
                            model: [
                                { label: qsTr("As Shot"), index: 0 },
                                { label: qsTr("Custom"), index: 1 }
                            ]
                            delegate: Button {
                                text: modelData.label
                                checkable: true
                                checked: colorTempModel.modeIndex === modelData.index
                                enabled: root.controlsEnabled && colorTempModel.supported
                                onClicked: colorTempModel.selectMode(modelData.index)
                                flat: true
                            }
                        }
                        Item { Layout.fillWidth: true }
                        AdjustmentResetButton {
                            objectName: "lookColorTempResetButton"
                            model: colorTempModel
                        }
                    }

                    Label {
                        visible: !colorTempModel.supported
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("White balance is unavailable for this image.")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    // CCT with non-linear Kelvin mapping via model.cctSliderPos
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        opacity: colorTempModel.supported ? 1 : 0.45
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: qsTr("Temperature")
                                color: root.colText
                                font.pixelSize: appTheme.fontSizeCaption
                                Layout.fillWidth: true
                            }
                            Label {
                                text: Math.round(colorTempModel.cct) + " K"
                                color: root.colMuted
                                font.pixelSize: appTheme.fontSizeCaption
                            }
                        }
                        Slider {
                            id: cctSlider
                            objectName: "lookCctSlider"
                            Layout.fillWidth: true
                            from: 0
                            to: 4096
                            stepSize: 1
                            value: colorTempModel.cctSliderPos
                            enabled: root.controlsEnabled && colorTempModel.supported
                            onPressedChanged: {
                                if (pressed)
                                    colorTempModel.beginCctDrag()
                                else
                                    colorTempModel.finishCctDrag()
                            }
                            onMoved: colorTempModel.updateCctSliderDrag(Math.round(value))
                            background: Rectangle {
                                x: cctSlider.leftPadding
                                y: cctSlider.topPadding + cctSlider.availableHeight / 2 - height / 2
                                implicitWidth: 200
                                implicitHeight: 8
                                width: cctSlider.availableWidth
                                height: implicitHeight
                                radius: 4
                                border.color: root.colCardBorder
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0.0; color: "#9BD8FF" }
                                    GradientStop { position: 0.5; color: "#FFE8B0" }
                                    GradientStop { position: 1.0; color: "#FF8A3D" }
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        opacity: colorTempModel.supported ? 1 : 0.45
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: qsTr("Tint")
                                color: root.colText
                                font.pixelSize: appTheme.fontSizeCaption
                                Layout.fillWidth: true
                            }
                            Label {
                                text: Math.round(colorTempModel.tint)
                                color: root.colMuted
                                font.pixelSize: appTheme.fontSizeCaption
                            }
                        }
                        Slider {
                            id: tintSlider
                            objectName: "lookTintSlider"
                            Layout.fillWidth: true
                            from: -150
                            to: 150
                            stepSize: 1
                            value: colorTempModel.tint
                            enabled: root.controlsEnabled && colorTempModel.supported
                            onPressedChanged: {
                                if (pressed)
                                    colorTempModel.beginTintDrag()
                                else
                                    colorTempModel.finishTintDrag()
                            }
                            onMoved: colorTempModel.updateTintDrag(value)
                            background: Rectangle {
                                x: tintSlider.leftPadding
                                y: tintSlider.topPadding + tintSlider.availableHeight / 2 - height / 2
                                implicitWidth: 200
                                implicitHeight: 8
                                width: tintSlider.availableWidth
                                height: implicitHeight
                                radius: 4
                                border.color: root.colCardBorder
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0.0; color: "#49C26D" }
                                    GradientStop { position: 0.5; color: "#E6E6E6" }
                                    GradientStop { position: 1.0; color: "#A85AE6" }
                                }
                            }
                        }
                    }
                }
            }

            // ── Color amount ──────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_color"
                title: qsTr("Color")
                expanded: true
                bodyContentHeight: colorBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: colorBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm

                    AdjustmentSlider {
                        objectName: "lookSaturationSlider"
                        Layout.fillWidth: true
                        model: saturationModel
                    }
                    AdjustmentSlider {
                        objectName: "lookVibranceSlider"
                        Layout.fillWidth: true
                        model: vibranceModel
                    }
                }
            }

            // ── Selective HSL ─────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_hls"
                title: qsTr("Selective Color")
                expanded: true
                bodyContentHeight: hlsBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: hlsBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Target Hue: %1°").arg(Math.round(hlsModel.targetHue))
                        color: root.colText
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    Row {
                        id: hueStrip
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
                                border.width: hlsModel.activeHueIndex === modelData.index ? 3 : 1
                                border.color: hlsModel.activeHueIndex === modelData.index
                                              ? root.colAccent : root.colCardBorder
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: root.controlsEnabled
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: hlsModel.selectHueIndex(modelData.index)
                                }
                            }
                        }
                    }

                    // Lightweight HSL sliders bound to model drag APIs
                    component HlsSliderRow: ColumnLayout {
                        property string title: ""
                        property real from: -100
                        property real to: 100
                        property real value: 0
                        property string suffix: ""
                        property var onBegin: function () {}
                        property var onUpdate: function (v) {}
                        property var onFinish: function () {}
                        property var onReset: function () {}
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: title
                                color: root.colText
                                font.pixelSize: appTheme.fontSizeCaption
                                Layout.fillWidth: true
                            }
                            Label {
                                text: Math.round(value) + suffix
                                color: root.colMuted
                                font.pixelSize: appTheme.fontSizeCaption
                            }
                            ToolButton {
                                text: "↺"
                                flat: true
                                enabled: root.controlsEnabled
                                onClicked: onReset()
                            }
                        }
                        Slider {
                            Layout.fillWidth: true
                            from: parent.from
                            to: parent.to
                            stepSize: 1
                            value: parent.value
                            enabled: root.controlsEnabled
                            onPressedChanged: {
                                if (pressed)
                                    parent.onBegin()
                                else
                                    parent.onFinish()
                            }
                            onMoved: parent.onUpdate(value)
                        }
                    }

                    HlsSliderRow {
                        objectName: "lookHlsHueShiftSlider"
                        title: qsTr("Hue Shift")
                        from: -30
                        to: 30
                        value: hlsModel.hueShift
                        suffix: "°"
                        onBegin: function () { hlsModel.beginHueShiftDrag() }
                        onUpdate: function (v) { hlsModel.updateHueShiftDrag(v) }
                        onFinish: function () { hlsModel.finishHueShiftDrag() }
                        onReset: function () { hlsModel.resetActiveField("hueShift") }
                    }
                    HlsSliderRow {
                        objectName: "lookHlsLightnessSlider"
                        title: qsTr("Lightness")
                        value: hlsModel.lightness
                        onBegin: function () { hlsModel.beginLightnessDrag() }
                        onUpdate: function (v) { hlsModel.updateLightnessDrag(v) }
                        onFinish: function () { hlsModel.finishLightnessDrag() }
                        onReset: function () { hlsModel.resetActiveField("lightness") }
                    }
                    HlsSliderRow {
                        objectName: "lookHlsChromaSlider"
                        title: qsTr("Chroma")
                        value: hlsModel.chroma
                        onBegin: function () { hlsModel.beginChromaDrag() }
                        onUpdate: function (v) { hlsModel.updateChromaDrag(v) }
                        onFinish: function () { hlsModel.finishChromaDrag() }
                        onReset: function () { hlsModel.resetActiveField("chroma") }
                    }
                    HlsSliderRow {
                        objectName: "lookHlsSmoothnessSlider"
                        title: qsTr("Hue Smoothness")
                        from: 1
                        to: 180
                        value: hlsModel.hueSmoothness
                        suffix: "°"
                        onBegin: function () { hlsModel.beginHueSmoothnessDrag() }
                        onUpdate: function (v) { hlsModel.updateHueSmoothnessDrag(v) }
                        onFinish: function () { hlsModel.finishHueSmoothnessDrag() }
                        onReset: function () { hlsModel.resetActiveField("hueSmoothness") }
                    }
                }
            }

            // ── CDL color wheels — original triangle layout ───────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_cdl"
                title: qsTr("Color Wheels")
                expanded: true
                bodyContentHeight: cdlBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: cdlBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm

                    // Gamma centered on top (legacy triangle).
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Gamma")
                            color: root.colMuted
                            font.pixelSize: appTheme.fontSizeCaption
                        }
                        EditorCdlTrackballItem {
                            id: gammaDisc
                            objectName: "lookCdlGammaDisc"
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 148
                            Layout.preferredHeight: 148
                            model: cdlModel
                            wheelRole: "gamma"
                            backgroundColor: root.colBase
                            rimColor: root.colCardBorder
                            crosshairColor: root.colMuted
                            handleColor: root.colAccent
                            handleOutlineColor: root.colText
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: cdlModel.gammaDeltaText
                            color: root.colMuted
                            font.pixelSize: appTheme.fontSizeCaption
                        }
                        MasterSlider {
                            objectName: "lookCdlGammaMaster"
                            wheelRole: "gamma"
                            valueUi: cdlModel.gammaMasterUi
                            labelText: qsTr("Master")
                        }
                    }

                    // Lift | Gain on the bottom row.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceSm

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: qsTr("Lift")
                                color: root.colMuted
                                font.pixelSize: appTheme.fontSizeCaption
                            }
                            EditorCdlTrackballItem {
                                objectName: "lookCdlLiftDisc"
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 132
                                Layout.preferredHeight: 132
                                Layout.fillWidth: true
                                model: cdlModel
                                wheelRole: "lift"
                                backgroundColor: root.colBase
                                rimColor: root.colCardBorder
                                crosshairColor: root.colMuted
                                handleColor: root.colAccent
                                handleOutlineColor: root.colText
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: cdlModel.liftDeltaText
                                color: root.colMuted
                                font.pixelSize: appTheme.fontSizeCaption
                                wrapMode: Text.WordWrap
                            }
                            MasterSlider {
                                objectName: "lookCdlLiftMaster"
                                wheelRole: "lift"
                                valueUi: cdlModel.liftMasterUi
                                labelText: qsTr("Master")
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: qsTr("Gain")
                                color: root.colMuted
                                font.pixelSize: appTheme.fontSizeCaption
                            }
                            EditorCdlTrackballItem {
                                objectName: "lookCdlGainDisc"
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 132
                                Layout.preferredHeight: 132
                                Layout.fillWidth: true
                                model: cdlModel
                                wheelRole: "gain"
                                backgroundColor: root.colBase
                                rimColor: root.colCardBorder
                                crosshairColor: root.colMuted
                                handleColor: root.colAccent
                                handleOutlineColor: root.colText
                            }
                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: cdlModel.gainDeltaText
                                color: root.colMuted
                                font.pixelSize: appTheme.fontSizeCaption
                                wrapMode: Text.WordWrap
                            }
                            MasterSlider {
                                objectName: "lookCdlGainMaster"
                                wheelRole: "gain"
                                valueUi: cdlModel.gainMasterUi
                                labelText: qsTr("Master")
                            }
                        }
                    }

                    Button {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("Reset wheels")
                        flat: true
                        enabled: root.controlsEnabled
                        onClicked: cdlModel.resetAll()
                    }
                }
            }

            // ── Detail ────────────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_detail"
                title: qsTr("Detail")
                expanded: false
                bodyContentHeight: detailBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: detailBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm
                    AdjustmentSlider {
                        objectName: "lookClaritySlider"
                        Layout.fillWidth: true
                        model: clarityModel
                    }
                    AdjustmentSlider {
                        objectName: "lookSharpenSlider"
                        Layout.fillWidth: true
                        model: sharpenModel
                    }
                }
            }

            // ── Texture ───────────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_texture"
                title: qsTr("Texture")
                expanded: false
                bodyContentHeight: textureBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: textureBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm
                    AdjustmentSlider {
                        objectName: "lookFilmGrainSlider"
                        Layout.fillWidth: true
                        model: filmGrainModel
                    }
                    AdjustmentSlider {
                        objectName: "lookHalationSlider"
                        Layout.fillWidth: true
                        model: halationModel
                    }
                }
            }

            // ── LUT ───────────────────────────────────────────────────────
            SectionShell {
                objectName: "editorAdjustmentGroupShell_look_lut"
                // Keep fold objectName family for look panel selection tests.
                title: qsTr("LUT")
                expanded: true
                bodyContentHeight: lutBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: lutBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm

                    Label {
                        Layout.fillWidth: true
                        text: lutModel.directoryText
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                        elide: Text.ElideMiddle
                    }
                    Label {
                        Layout.fillWidth: true
                        text: lutModel.statusText
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    TextField {
                        id: lutFilter
                        objectName: "lookLutFilterField"
                        Layout.fillWidth: true
                        placeholderText: qsTr("Filter LUTs")
                        enabled: root.controlsEnabled
                        onTextChanged: lutModel.filterText = text
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceXs
                        Button {
                            text: qsTr("Refresh")
                            enabled: root.controlsEnabled
                            onClicked: lutModel.refresh(true)
                        }
                        Button {
                            text: qsTr("Open folder")
                            enabled: root.controlsEnabled && lutModel.canOpenDirectory
                            onClicked: Qt.openUrlExternally("file:///" + lutModel.directoryPath())
                        }
                        Item { Layout.fillWidth: true }
                    }

                    ListView {
                        id: lutList
                        objectName: "lookLutList"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(220, Math.max(88, count * 40))
                        clip: true
                        model: lutModel.entries
                        spacing: 2
                        delegate: Rectangle {
                            width: lutList.width
                            height: 36
                            radius: 6
                            color: modelData.selected ? root.colAccent : root.colBase
                            border.color: root.colCardBorder
                            opacity: modelData.selectable ? 1.0 : 0.55
                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 8
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.displayName
                                        color: modelData.selected ? root.colCardSurface : root.colText
                                        elide: Text.ElideRight
                                        font.pixelSize: appTheme.fontSizeBody
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.secondaryText || modelData.statusText || ""
                                        color: modelData.selected ? root.colCardSurface : root.colMuted
                                        elide: Text.ElideRight
                                        font.pixelSize: appTheme.fontSizeCaption
                                        visible: text.length > 0
                                    }
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

            Item { Layout.fillHeight: true }
        }
    }
}
