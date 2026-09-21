import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// RAW Decode panel. Import already asked LibRaw whether the file is
// decodable; this panel only presents controls and submits complete operator
// parameter objects through the typed adjustment models.
Item {
    id: root
    objectName: "editorAdjustmentPanel_raw"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true
    property bool restoring: false
    property var rawParams: ({})
    property var lensBrandEntries: []
    property var lensModelEntries: []

    // ponytail: fixed method list; no per-image capability map.
    readonly property var rawMethodEntries: [
        { value: "default", label: qsTr("Default") },
        { value: "legacy", label: qsTr("Legacy") },
        { value: "neural_engine", label: qsTr("Neural Engine") }
    ]

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colBase: theme ? theme.colBgBase : appTheme.bgBaseColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor

    // Panel-level selection aliases so the method-track segments re-evaluate
    // when currentIndex changes or snapshot load restores (selectedPath rule).
    readonly property int selectedMethodIndex: rawMethodModel.currentIndex
    readonly property string selectedMethodValue: {
        var _dep = rawMethodModel.currentIndex
        return rawMethodModel.currentValue ? String(rawMethodModel.currentValue) : ""
    }
    readonly property int selectedLensBrandIndex: lensBrandModel.currentIndex
    readonly property string selectedLensBrandValue: {
        var _dep = lensBrandModel.currentIndex
        return lensBrandModel.currentValue ? String(lensBrandModel.currentValue) : ""
    }
    readonly property int selectedLensModelIndex: lensModelModel.currentIndex
    readonly property string selectedLensModelValue: {
        var _dep = lensModelModel.currentIndex
        return lensModelModel.currentValue ? String(lensModelModel.currentValue) : ""
    }

    EditorLensCatalogModel {
        id: lensCatalog
        objectName: "rawLensCatalog"
    }

    function buildDefaultRawParams() {
        return {
            raw: {
                // No accelerator backend: the decode backend is a runtime
                // property of the pipeline (user setting), not an edit param.
                method: "default",
                highlights_reconstruct: true,
                use_camera_wb: true,
                user_wb: 7600.0,
                backend: "alcedo"
            }
        }
    }

    function mergeRawParams(rawEntry) {
        var params = root.buildDefaultRawParams()
        if (rawEntry) {
            for (var key in rawEntry)
                params.raw[key] = rawEntry[key]
        }
        return params
    }

    function setEnumValue(model, value, fallbackIndex) {
        if (!model)
            return
        var index = fallbackIndex === undefined ? 0 : fallbackIndex
        for (var i = 0; i < model.entries.length; ++i) {
            if (String(model.entries[i].value) === String(value)) {
                index = i
                break
            }
        }
        model.currentIndex = index
    }

    function buildRawParams() {
        // Pure builder: do not write root.rawParams during submit (avoids
        // binding churn while the patch is already enqueued).
        var base = root.rawParams && root.rawParams.raw
                ? root.rawParams
                : root.buildDefaultRawParams()
        var raw = {}
        if (base.raw) {
            var src = base.raw
            for (var k in src) {
                if (Object.prototype.hasOwnProperty.call(src, k))
                    raw[k] = src[k]
            }
        }
        raw.method = String(rawMethodModel.currentValue || "default")
        raw.highlights_reconstruct = Boolean(rawHighlightsModel.value)
        return JSON.stringify({ raw: raw })
    }

    function buildLensEntries(values) {
        var result = [{ value: "", label: qsTr("Auto (metadata)") }]
        for (var i = 0; i < values.length; ++i) {
            result.push({ value: String(values[i].value), label: String(values[i].label) })
        }
        return result
    }

    function buildLensModelEntries(values) {
        var result = []
        for (var i = 0; i < values.length; ++i) {
            result.push({ value: String(values[i].value), label: String(values[i].label) })
        }
        return result
    }

    function refreshLensBrandEntries() {
        root.lensBrandEntries = root.buildLensEntries(lensCatalog.brands)
        lensBrandModel.entries = root.lensBrandEntries
    }

    function refreshLensModelEntries(brand, requestedModel) {
        const catalogValues = lensCatalog.modelsForBrand(brand)
        var entries = brand && brand.length > 0
                      ? root.buildLensModelEntries(catalogValues)
                      : root.buildLensEntries([])
        if (requestedModel && requestedModel.length > 0) {
            var found = false
            for (var i = 0; i < entries.length; ++i) {
                if (entries[i].value === requestedModel) {
                    found = true
                    break
                }
            }
            if (!found)
                entries.push({ value: requestedModel, label: requestedModel })
        }
        root.lensModelEntries = entries
        lensModelModel.entries = entries
        root.setEnumValue(lensModelModel, requestedModel, 0)
    }

    function buildLensParams() {
        var payload = {}
        try {
            payload = JSON.parse(lensCatalog.defaultParamsJson)
        } catch (error) {
            payload = { lens_calib: {} }
        }
        if (!payload.lens_calib)
            payload.lens_calib = {}
        const brand = String(lensBrandModel.currentValue)
        payload.lens_calib.enabled = Boolean(lensEnabledModel.value)
        payload.lens_calib.lens_maker = brand
        payload.lens_calib.lens_model = brand.length > 0 ? String(lensModelModel.currentValue) : ""
        return JSON.stringify(payload)
    }

    function submitLens(settled) {
        if (!root.editorSession || typeof root.editorSession.submitPatch !== "function")
            return false
        return root.editorSession.submitPatch("lens_calib", root.buildLensParams(), settled)
    }

    function resetLensModels() {
        lensEnabledModel.value = lensEnabledModel.defaultValue
        root.setEnumValue(lensBrandModel, "", 0)
        root.refreshLensModelEntries("", "")
    }

    function resetLens() {
        root.restoring = true
        root.resetLensModels()
        root.restoring = false
        root.submitLens(true)
    }

    function wireLensEnabled() {
        const enabled = root.controlsEnabled
        lensEnabledModel.enabled = enabled
        lensBrandModel.enabled = enabled
        lensModelModel.enabled = enabled && lensBrandModel.currentValue.length > 0
                                 && lensModelModel.entries.length > 0
    }

    function loadLensSnapshot(snapshot) {
        const raw = snapshot ? snapshot["lens_calib"] : undefined
        const entry = raw && raw["lens_calib"] !== undefined ? raw["lens_calib"] : raw
        root.restoring = true
        if (!entry) {
            root.resetLensModels()
            root.restoring = false
            root.wireLensEnabled()
            return
        }
        lensEnabledModel.value = entry["enabled"] !== undefined
                                 ? Boolean(entry["enabled"]) : lensEnabledModel.defaultValue
        const brand = entry["lens_maker"] !== undefined ? String(entry["lens_maker"]) : ""
        const model = entry["lens_model"] !== undefined ? String(entry["lens_model"]) : ""
        if (brand.length > 0) {
            var brandKnown = false
            for (var brandIndex = 0; brandIndex < lensBrandModel.entries.length; ++brandIndex) {
                if (String(lensBrandModel.entries[brandIndex].value) === brand) {
                    brandKnown = true
                    break
                }
            }
            if (!brandKnown) {
                var entries = lensBrandModel.entries.slice(0)
                entries.push({ value: brand, label: brand })
                root.lensBrandEntries = entries
                lensBrandModel.entries = entries
            }
        }
        root.setEnumValue(lensBrandModel, brand, 0)
        root.refreshLensModelEntries(brand, model)
        root.restoring = false
        root.wireLensEnabled()
    }

    function loadFromSnapshot(snapshot) {
        root.restoring = true
        var rawWrapper = snapshot ? snapshot["raw_decode"] : undefined
        var rawEntry = rawWrapper && rawWrapper["raw"] !== undefined
                ? rawWrapper["raw"] : rawWrapper
        root.rawParams = root.mergeRawParams(rawEntry)

        const method = rawEntry && rawEntry.method !== undefined
                ? String(rawEntry.method) : "default"
        root.setEnumValue(rawMethodModel, method, rawMethodModel.defaultIndex)
        rawHighlightsModel.value = rawEntry && rawEntry.highlights_reconstruct !== undefined
                ? Boolean(rawEntry.highlights_reconstruct) : rawHighlightsModel.defaultValue
        if (typeof whiteBalanceSection.loadFromSnapshot === "function")
            whiteBalanceSection.loadFromSnapshot(snapshot)
        if (!root.lensBrandEntries.length)
            root.refreshLensBrandEntries()
        root.loadLensSnapshot(snapshot)
        root.restoring = false
    }

    Flickable {
        id: rawScroll
        objectName: "editorRawDecodePanelScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: rawColumn.implicitHeight
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
                var maxY = Math.max(0, rawScroll.contentHeight - rawScroll.height)
                rawScroll.contentY = Math.max(0, Math.min(maxY, rawScroll.contentY - step))
                event.accepted = true
            }
        }

        ColumnLayout {
            id: rawColumn
            width: rawScroll.width
            spacing: appTheme.spaceSm

            Label {
                objectName: "rawDecodeTitle"
                Layout.fillWidth: true
                text: qsTr("RAW Decode")
                color: root.colText
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightHeading
            }

            CollapsibleSection {
                id: rawSection
                objectName: "editorAdjustmentGroupShell_raw_decode"
                Layout.fillWidth: true
                title: qsTr("RAW Decode")
                expanded: true
                controlsEnabled: root.controlsEnabled
                surfaceColor: root.colCardSurface
                disabledSurfaceColor: root.colCardSurface
                borderColor: root.colCardBorder
                textColor: root.colText
                mutedColor: root.colMuted
                hoverColor: root.colHover
                accentColor: root.colAccent
                bodyContentHeight: rawBody.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: rawBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Method")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    SegmentedCardSwitcher {
                        objectName: "rawDemosaicMethodControl"
                        Layout.fillWidth: true
                        entries: rawMethodModel.entries
                        currentIndex: root.selectedMethodIndex
                        currentValue: root.selectedMethodValue
                        enabled: root.controlsEnabled
                        trackColor: root.colBase
                        trackBorderColor: root.colCardBorder
                        textColor: root.colText
                        hoverColor: root.colHover
                        onSelected: function(index, value) { rawMethodModel.selectIndex(index) }
                    }

                    AdjustmentToggle {
                        objectName: "rawHighlightsControl"
                        model: rawHighlightsModel
                    }
                }
            }

            EditorWhiteBalanceSection {
                id: whiteBalanceSection
                Layout.fillWidth: true
                theme: root.theme
                editorSession: root.editorSession
                flickable: rawScroll
                controlsEnabled: root.controlsEnabled
            }

            CollapsibleSection {
                id: lensSection
                objectName: "editorAdjustmentGroupShell_raw_lens"
                Layout.fillWidth: true
                title: qsTr("Lens Calibration")
                expanded: true
                controlsEnabled: root.controlsEnabled
                surfaceColor: root.colCardSurface
                disabledSurfaceColor: root.colCardSurface
                borderColor: root.colCardBorder
                textColor: root.colText
                mutedColor: root.colMuted
                hoverColor: root.colHover
                accentColor: root.colAccent
                bodyContentHeight: lensControls.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: lensControls
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm

                    AdjustmentToggle {
                        objectName: "rawLensEnabledToggle"
                        Layout.fillWidth: true
                        model: lensEnabledModel
                    }
                    AdjustmentCombo {
                        objectName: "rawLensBrandCombo"
                        Layout.fillWidth: true
                        model: lensBrandModel
                    }
                    AdjustmentCombo {
                        objectName: "rawLensModelCombo"
                        Layout.fillWidth: true
                        model: lensModelModel
                    }
                    Label {
                        Layout.fillWidth: true
                        text: lensCatalog.statusText
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceSm
                        Item { Layout.fillWidth: true }
                        IconActionButton {
                            objectName: "rawLensResetButton"
                            compact: true
                            enabled: root.controlsEnabled
                            iconSrc: "qrc:/panel_icons/reset.svg"
                            actionName: qsTr("Reset lens calibration")
                            onClicked: root.resetLens()
                        }
                    }
                }
            }
        }
    }

    EditorAdjustmentEnumModel {
        id: rawMethodModel
        objectName: "rawDemosaicMethodModel"
        fieldKey: "raw_decode"
        label: qsTr("Method")
        entries: root.rawMethodEntries
        defaultIndex: 0
        enabled: root.controlsEnabled
        submitter: root.editorSession
        paramsBuilder: root.buildRawParams
    }

    EditorAdjustmentToggleModel {
        id: rawHighlightsModel
        objectName: "rawHighlightsModel"
        fieldKey: "raw_decode"
        label: qsTr("Enable Highlight Reconstruction")
        defaultValue: true
        value: true
        enabled: root.controlsEnabled
        submitter: root.editorSession
        paramsBuilder: root.buildRawParams
    }

    EditorAdjustmentToggleModel {
        id: lensEnabledModel
        objectName: "rawLensEnabledModel"
        fieldKey: "lens_calib"
        label: qsTr("Enable Lens Calibration")
        defaultValue: false
        value: false
        submitter: root.editorSession
        paramsBuilder: function (value) { return root.buildLensParams() }
    }
    EditorAdjustmentEnumModel {
        id: lensBrandModel
        objectName: "rawLensBrandModel"
        fieldKey: "lens_calib"
        label: qsTr("Lens Brand")
        entries: root.lensBrandEntries
        submitter: root.editorSession
        paramsBuilder: function (value) { return root.buildLensParams() }
    }
    EditorAdjustmentEnumModel {
        id: lensModelModel
        objectName: "rawLensModelModel"
        fieldKey: "lens_calib"
        label: qsTr("Lens Model")
        entries: root.lensModelEntries
        submitter: root.editorSession
        paramsBuilder: function (value) { return root.buildLensParams() }
    }

    Connections {
        target: lensEnabledModel
        function onValueChanged() {
            root.wireLensEnabled()
        }
    }
    Connections {
        target: lensBrandModel
        function onCurrentIndexChanged() {
            const requested = root.restoring ? lensModelModel.currentValue : ""
            root.refreshLensModelEntries(lensBrandModel.currentValue, requested)
            root.wireLensEnabled()
        }
    }

    onControlsEnabledChanged: root.wireLensEnabled()

    onEditorSessionChanged: {
        root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }

    Component.onCompleted: {
        root.refreshLensBrandEntries()
        root.refreshLensModelEntries("", "")
        root.wireLensEnabled()
        root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }
}
