import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Phase 6E Display Transform panel: output color-space selection, CST/ODT,
// ACES tone mapping, transfer function (EOTF), peak luminance, and HDR
// display intent. Method-specific controls for ACES 2.0 (limiting space) and
// OpenDRT (look/tonescale/creative-white presets). All models share fieldKey
// "odt" with a panel-level paramsBuilder that collects the complete state so
// every partial edit carries the full ODT JSON. The option lists, the EOTF
// choices of each encoding space, the peak range, and the parameter object
// come from the editor parameter catalog.
Item {
    id: root
    objectName: "editorAdjustmentPanel_display"

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

    readonly property var odtDefaults: parameterCatalog.uiDefault("odt")
    readonly property var peakSpec: parameterCatalog.propertySpec("odt", "peak_luminance")

    EditorParameterCatalog {
        id: parameterCatalog
        objectName: "displayParameterCatalog"
    }

    /// Translated `{value, label}` menu entries of an option list of the catalog.
    function translatedEntries(options) {
        var result = []
        for (var i = 0; i < options.length; ++i)
            result.push({ value: String(options[i].value), label: qsTr(String(options[i].label)) })
        return result
    }

    function catalogEntries(name) {
        return root.translatedEntries(parameterCatalog.options("odt", name))
    }

    /// EOTF choices of the display transform for an encoding space.
    function eotfOptionsForSpace(spaceValue) {
        return root.translatedEntries(parameterCatalog.odtEotfOptions(String(spaceValue)))
    }

    function defaultIndexOf(entries, name) {
        return Math.max(0, root.indexOfValue(entries, root.odtDefaults[name]))
    }

    /// Find index of a value in an entries array. Returns -1 when missing so
    /// callers can leave the model alone instead of snapping to entry 0.
    function indexOfValue(entries, val) {
        if (val === undefined || val === null)
            return -1
        if (!entries || entries.length === undefined)
            return -1
        const want = String(val)
        for (var i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            if (!entry)
                continue
            const have = entry["value"] !== undefined ? entry["value"] : entry.value
            if (String(have) === want)
                return i
        }
        return -1
    }

    function selectedValue(model, name) {
        const entry = model.entries[model.currentIndex]
        return entry ? String(entry.value) : String(root.odtDefaults[name])
    }

    // ── Shared params builder: collects every current model value into the
    //    complete ODT state. Each model's paramsBuilder delegates to this so
    //    patches always carry the full ODT state; the catalog writes the
    //    controls of the selected method.
    function buildOdtParams() {
        return parameterCatalog.modelParamsJson("odt", {
            method: root.selectedValue(methodModel, "method"),
            encoding_space: root.selectedValue(encodingSpaceModel, "encoding_space"),
            encoding_eotf: root.selectedValue(encodingEotfModel, "encoding_eotf"),
            peak_luminance: peakLuminanceModel.value,
            limiting_space: root.selectedValue(acesLimitingSpaceModel, "limiting_space"),
            look_preset: root.selectedValue(openDrtLookModel, "look_preset"),
            tonescale_preset: root.selectedValue(openDrtTonescaleModel, "tonescale_preset"),
            creative_white: root.selectedValue(openDrtCreativeWhiteModel, "creative_white")
        })
    }

    function wireEnabled() {
        const on = root.controlsEnabled
        methodModel.enabled = on
        encodingSpaceModel.enabled = on
        encodingEotfModel.enabled = on
        peakLuminanceModel.enabled = on
        acesLimitingSpaceModel.enabled = on
        openDrtLookModel.enabled = on
        openDrtTonescaleModel.enabled = on
        openDrtCreativeWhiteModel.enabled = on
    }

    /// Apply declared defaultIndex / defaultValue before any snapshot load so the
    /// panel never boots on construction zeros (enum currentIndex starts at 0).
    function applyDeclaredDefaults() {
        methodModel.currentIndex = methodModel.defaultIndex
        encodingSpaceModel.currentIndex = encodingSpaceModel.defaultIndex
        encodingEotfModel.currentIndex = encodingEotfModel.defaultIndex
        peakLuminanceModel.value = peakLuminanceModel.defaultValue
        acesLimitingSpaceModel.currentIndex = acesLimitingSpaceModel.defaultIndex
        openDrtLookModel.currentIndex = openDrtLookModel.defaultIndex
        openDrtTonescaleModel.currentIndex = openDrtTonescaleModel.defaultIndex
        openDrtCreativeWhiteModel.currentIndex = openDrtCreativeWhiteModel.defaultIndex
        updateEotfOptions()
    }

    /// Load-only enum write. Enum models have no dragActive; peak uses it below.
    function setEnumFromSnapshot(model, value) {
        if (!model || value === undefined || value === null)
            return
        const idx = indexOfValue(model.entries, value)
        if (idx < 0)
            return
        if (idx !== model.currentIndex)
            model.currentIndex = idx
    }

    /// ODT UI value of the snapshot. BuildSnapshotMap stores
    /// snapshot["odt"] = {"odt": {...}}; the catalog also reads a flat map.
    function odtUiValue(snapshot) {
        if (snapshot === undefined || snapshot === null)
            return null
        const wrap = snapshot["odt"]
        if (wrap === undefined || wrap === null)
            return null
        const odt = parameterCatalog.uiValue("odt", wrap)
        return odt.method !== undefined ? odt : null
    }

    onControlsEnabledChanged: wireEnabled()
    Component.onCompleted: {
        wireEnabled()
        applyDeclaredDefaults()
        // Bootstrap when the stack has not yet projected.
        loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }
    onEditorSessionChanged: {
        applyDeclaredDefaults()
        loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }

    // ── Snapshot load (Phase 6C-7 pattern) ──────────────────────────────────
    /// Load display transform values from the session adjustment snapshot.
    /// The snapshot stores odt params as snapshot["odt"] = {"odt": {...}}.
    function loadFromSnapshot(snapshot) {
        const odt = odtUiValue(snapshot)
        if (odt === null)
            return

        setEnumFromSnapshot(methodModel, odt["method"])
        setEnumFromSnapshot(encodingSpaceModel, odt["encoding_space"])

        // EOTF entries depend on encoding space; rebuild then restore.
        updateEotfOptions()
        setEnumFromSnapshot(encodingEotfModel, odt["encoding_eotf"])

        if (!peakLuminanceModel.dragActive) {
            const pl = Number(odt["peak_luminance"])
            if (!isNaN(pl)
                    && Math.abs(peakLuminanceModel.value - pl) > (peakLuminanceModel.step * 0.1)) {
                peakLuminanceModel.value = pl
            }
        }

        setEnumFromSnapshot(acesLimitingSpaceModel, odt["limiting_space"])
        setEnumFromSnapshot(openDrtLookModel, odt["look_preset"])
        setEnumFromSnapshot(openDrtTonescaleModel, odt["tonescale_preset"])
        setEnumFromSnapshot(openDrtCreativeWhiteModel, odt["creative_white"])
    }

    /// Rebuild EOTF entries based on current encoding space, preserving EOTF
    /// selection when the value exists in the new options.
    function updateEotfOptions() {
        var prevValue = ""
        if (encodingEotfModel.entries && encodingEotfModel.entries.length > 0) {
            var pe = encodingEotfModel.entries[encodingEotfModel.currentIndex]
            if (pe)
                prevValue = String(pe["value"] !== undefined ? pe["value"] : pe.value)
        }
        const spaceVal = root.selectedValue(encodingSpaceModel, "encoding_space")
        encodingEotfModel.entries = root.eotfOptionsForSpace(spaceVal)
        var newIdx = indexOfValue(encodingEotfModel.entries, prevValue)
        if (newIdx < 0)
            newIdx = 0
        if (newIdx !== encodingEotfModel.currentIndex)
            encodingEotfModel.currentIndex = newIdx
    }

    // ── Models ──────────────────────────────────────────────────────────────
    EditorAdjustmentEnumModel {
        id: methodModel
        objectName: "displayMethodModel"
        fieldKey: "odt"
        label: qsTr("Method")
        entries: root.catalogEntries("method")
        defaultIndex: root.defaultIndexOf(entries, "method")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: encodingSpaceModel
        objectName: "displayEncodingSpaceModel"
        fieldKey: "odt"
        label: qsTr("Encoding Space")
        entries: root.catalogEntries("encoding_space")
        defaultIndex: root.defaultIndexOf(entries, "encoding_space")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: encodingEotfModel
        objectName: "displayEncodingEotfModel"
        fieldKey: "odt"
        label: qsTr("Encoding EOTF")
        entries: root.eotfOptionsForSpace(root.odtDefaults.encoding_space)
        defaultIndex: root.defaultIndexOf(entries, "encoding_eotf")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: peakLuminanceModel
        objectName: "displayPeakLuminanceModel"
        fieldKey: "odt"
        label: qsTr("Peak Luminance")
        minimum: root.peakSpec.ui_min
        maximum: root.peakSpec.ui_max
        defaultValue: root.odtDefaults.peak_luminance
        step: root.peakSpec.ui_step
        precision: root.peakSpec.ui_decimals
        suffix: " nits"
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: acesLimitingSpaceModel
        objectName: "displayAcesLimitingSpaceModel"
        fieldKey: "odt"
        label: qsTr("Limiting Space")
        entries: root.catalogEntries("limiting_space")
        defaultIndex: root.defaultIndexOf(entries, "limiting_space")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: openDrtLookModel
        objectName: "displayOpenDrtLookModel"
        fieldKey: "odt"
        label: qsTr("Look")
        entries: root.catalogEntries("look_preset")
        defaultIndex: root.defaultIndexOf(entries, "look_preset")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: openDrtTonescaleModel
        objectName: "displayOpenDrtTonescaleModel"
        fieldKey: "odt"
        label: qsTr("Tonescale")
        entries: root.catalogEntries("tonescale_preset")
        defaultIndex: root.defaultIndexOf(entries, "tonescale_preset")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: openDrtCreativeWhiteModel
        objectName: "displayOpenDrtCreativeWhiteModel"
        fieldKey: "odt"
        label: qsTr("Creative White")
        entries: root.catalogEntries("creative_white")
        defaultIndex: root.defaultIndexOf(entries, "creative_white")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    // ── Derived state ───────────────────────────────────────────────────────
    // Panel-level selection aliases so method-card delegates re-evaluate when
    // currentIndex changes or snapshot load restores (LUT selectedPath pattern).
    readonly property int selectedMethodIndex: methodModel.currentIndex
    readonly property string selectedMethodValue: {
        var _dep = methodModel.currentIndex
        return methodModel.currentValue ? String(methodModel.currentValue) : ""
    }
    readonly property bool isOpenDrt: root.selectedMethodValue === "open_drt"
        || (root.selectedMethodValue.length === 0)

    // Encoding space drives the EOTF option table (user select + snapshot load).
    Connections {
        target: encodingSpaceModel
        function onCurrentIndexChanged() {
            root.updateEotfOptions()
        }
    }

    // ── Layout ──────────────────────────────────────────────────────────────
    // Grill-locked IA (2026-07-25):
    //   Method first (always expanded) → method params inline → Color & Encoding
    //   collapsible default-open. Method switcher = shared sunk track + monochrome
    //   inverted well, title-only segments, medium height (~48), fillWidth for a
    //   future resizable right rail.
    ColumnLayout {
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Label {
            Layout.fillWidth: true
            text: qsTr("Display Transform")
            color: root.colText
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightHeading
        }

        // ── Method (always visible; not collapsible) ────────────────────
        ColumnLayout {
            id: methodGroup
            objectName: "editorAdjustmentGroupShell_display_method"
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: qsTr("Method")
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
            }

            // Shared sunk track; segments fillWidth so a resizable rail keeps
            // equal halves without fixed pixel cards. Selection aliases feed
            // the value-led highlight so snapshot load and selectIndex both
            // invalidate it (LUT selectedPath pattern).
            SegmentedCardSwitcher {
                objectName: "displayMethodTrack"
                Layout.fillWidth: true
                entries: methodModel.entries
                currentIndex: root.selectedMethodIndex
                currentValue: root.selectedMethodValue
                enabled: root.controlsEnabled
                trackColor: root.colBase
                trackBorderColor: root.colCardBorder
                textColor: root.colText
                hoverColor: root.colHover
                onSelected: function(index, value) { methodModel.selectIndex(index) }
            }

            // Method-specific params (inline under the track).
            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm
                visible: !root.isOpenDrt

                AdjustmentCombo {
                    objectName: "displayAcesLimitingSpaceCombo"
                    Layout.fillWidth: true
                    model: acesLimitingSpaceModel
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm
                visible: root.isOpenDrt

                AdjustmentCombo {
                    objectName: "displayOpenDrtLookCombo"
                    Layout.fillWidth: true
                    model: openDrtLookModel
                }

                AdjustmentCombo {
                    objectName: "displayOpenDrtTonescaleCombo"
                    Layout.fillWidth: true
                    model: openDrtTonescaleModel
                }

                AdjustmentCombo {
                    objectName: "displayOpenDrtCreativeWhiteCombo"
                    Layout.fillWidth: true
                    model: openDrtCreativeWhiteModel
                }
            }
        }

        // ── Color & Encoding (collapsible, default expanded) ────────────
        CollapsibleSection {
            id: colorEncodingGroup
            objectName: "editorAdjustmentGroupShell_display_color_encoding"
            Layout.fillWidth: true
            title: qsTr("Color & Encoding")
            expanded: true
            controlsEnabled: root.controlsEnabled
            surfaceColor: root.colCardSurface
            disabledSurfaceColor: root.colCardSurface
            borderColor: root.colCardBorder
            textColor: root.colText
            mutedColor: root.colMuted
            hoverColor: root.colHover
            accentColor: root.colAccent
            bodyContentHeight: ceBody.implicitHeight + appTheme.spaceSm

            ColumnLayout {
                id: ceBody
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: appTheme.spaceXs
                spacing: appTheme.spaceSm

                AdjustmentCombo {
                    objectName: "displayEncodingSpaceCombo"
                    Layout.fillWidth: true
                    model: encodingSpaceModel
                }

                AdjustmentCombo {
                    objectName: "displayEncodingEotfCombo"
                    Layout.fillWidth: true
                    model: encodingEotfModel
                }

                AdjustmentSlider {
                    objectName: "displayPeakLuminanceSlider"
                    Layout.fillWidth: true
                    model: peakLuminanceModel
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
