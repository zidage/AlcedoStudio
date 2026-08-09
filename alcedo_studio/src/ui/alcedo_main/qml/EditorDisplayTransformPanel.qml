import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Phase 6E Display Transform panel: output color-space selection, CST/ODT,
// ACES tone mapping, transfer function (EOTF), peak luminance, and HDR
// display intent. Method-specific controls for ACES 2.0 (limiting space) and
// OpenDRT (look/tonescale/creative-white presets). All models share fieldKey
// "odt" with a panel-level paramsBuilder that collects the complete nested
// state so every partial edit carries the full ODT JSON.
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

    // EOTF choices supported by the unified display-transform model.
    readonly property var eotfOptionsRec709: [
        { value: "bt1886", label: qsTr("BT.1886") },
        { value: "gamma_2_2", label: qsTr("Gamma 2.2") }
    ]
    readonly property var eotfOptionsP3D65: [
        { value: "gamma_2_2", label: qsTr("Gamma 2.2") },
        { value: "st2084", label: qsTr("ST 2084 (PQ)") }
    ]
    readonly property var eotfOptionsP3Theater: [
        { value: "gamma_2_6", label: qsTr("Gamma 2.6") }
    ]
    readonly property var eotfOptionsRec2020: [
        { value: "st2084", label: qsTr("ST 2084 (PQ)") },
        { value: "hlg", label: qsTr("HLG") }
    ]
    readonly property var eotfOptionsDefault: [
        { value: "gamma_2_2", label: qsTr("Gamma 2.2") }
    ]

    function eotfOptionsForSpace(spaceValue) {
        switch (spaceValue) {
        case "rec709": return root.eotfOptionsRec709
        case "p3_d65": return root.eotfOptionsP3D65
        case "p3_d60":
        case "p3_dci":
        case "xyz": return root.eotfOptionsP3Theater
        case "rec2020": return root.eotfOptionsRec2020
        default: return root.eotfOptionsDefault
        }
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

    // ── Shared params builder: collects every current model value into the
    //    complete nested {"odt": {...}} JSON. Each model's paramsBuilder
    //    delegates to this so patches always carry the full ODT state.
    function buildOdtParams() {
        var odt = {}

        // method
        var me = methodModel.entries[methodModel.currentIndex]
        odt.method = me ? String(me.value) : "open_drt"

        // encoding space
        var se = encodingSpaceModel.entries[encodingSpaceModel.currentIndex]
        odt.encoding_space = se ? String(se.value) : "rec709"

        // EOTF
        var ee = encodingEotfModel.entries[encodingEotfModel.currentIndex]
        odt.encoding_eotf = ee ? String(ee.value) : "gamma_2_2"

        // peak luminance
        odt.peak_luminance = peakLuminanceModel.value

        if (odt.method === "aces_2_0") {
            var le = acesLimitingSpaceModel.entries[acesLimitingSpaceModel.currentIndex]
            odt.limiting_space = le ? String(le.value) : "rec709"
        } else {
            // OpenDRT
            var lk = openDrtLookModel.entries[openDrtLookModel.currentIndex]
            var tn = openDrtTonescaleModel.entries[openDrtTonescaleModel.currentIndex]
            var cw = openDrtCreativeWhiteModel.entries[openDrtCreativeWhiteModel.currentIndex]
            odt.open_drt = {
                look_preset: lk ? String(lk.value) : "standard",
                tonescale_preset: tn ? String(tn.value) : "use_look_preset",
                creative_white: cw ? String(cw.value) : "use_look_preset"
            }
        }

        return JSON.stringify({ odt: odt })
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

    /// Unwrap operator-shaped ODT params. BuildSnapshotMap stores
    /// snapshot["odt"] = {"odt": {...}}; tolerate a flat inner map too.
    function unwrapOdt(snapshot) {
        if (snapshot === undefined || snapshot === null)
            return null
        const wrap = snapshot["odt"]
        if (wrap === undefined || wrap === null)
            return null
        if (wrap["odt"] !== undefined && wrap["odt"] !== null)
            return wrap["odt"]
        if (wrap["method"] !== undefined || wrap["encoding_space"] !== undefined)
            return wrap
        return null
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
        const odt = unwrapOdt(snapshot)
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

        const drt = odt["open_drt"]
        if (drt !== undefined && drt !== null) {
            setEnumFromSnapshot(openDrtLookModel, drt["look_preset"])
            setEnumFromSnapshot(openDrtTonescaleModel, drt["tonescale_preset"])
            setEnumFromSnapshot(openDrtCreativeWhiteModel, drt["creative_white"])
        }
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
        var spaceEntry = encodingSpaceModel.entries[encodingSpaceModel.currentIndex]
        var spaceVal = "rec709"
        if (spaceEntry) {
            spaceVal = String(spaceEntry["value"] !== undefined ? spaceEntry["value"]
                                                                : spaceEntry.value)
        }
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
        entries: [
            { value: "aces_2_0", label: qsTr("ACES 2.0") },
            { value: "open_drt", label: qsTr("OpenDRT") }
        ]
        defaultIndex: 1  // open_drt
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: encodingSpaceModel
        objectName: "displayEncodingSpaceModel"
        fieldKey: "odt"
        label: qsTr("Encoding Space")
        entries: [
            { value: "rec709", label: qsTr("Rec.709") },
            { value: "p3_d65", label: qsTr("P3-D65") },
            { value: "p3_d60", label: qsTr("P3-D60") },
            { value: "p3_dci", label: qsTr("P3-DCI") },
            { value: "xyz", label: qsTr("XYZ") },
            { value: "rec2020", label: qsTr("Rec.2020") }
        ]
        defaultIndex: 0
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: encodingEotfModel
        objectName: "displayEncodingEotfModel"
        fieldKey: "odt"
        label: qsTr("Encoding EOTF")
        entries: root.eotfOptionsRec709
        defaultIndex: 1  // gamma_2_2
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: peakLuminanceModel
        objectName: "displayPeakLuminanceModel"
        fieldKey: "odt"
        label: qsTr("Peak Luminance")
        minimum: 100
        maximum: 1000
        defaultValue: 100
        step: 1
        precision: 0
        suffix: " nits"
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: acesLimitingSpaceModel
        objectName: "displayAcesLimitingSpaceModel"
        fieldKey: "odt"
        label: qsTr("Limiting Space")
        entries: [
            { value: "rec709", label: qsTr("Rec.709") },
            { value: "rec2020", label: qsTr("Rec.2020") },
            { value: "p3_d65", label: qsTr("P3-D65") },
            { value: "p3_d60", label: qsTr("P3-D60") },
            { value: "p3_dci", label: qsTr("P3-DCI") },
            { value: "prophoto", label: qsTr("ProPhoto RGB") },
            { value: "adobe_rgb", label: qsTr("Adobe RGB") }
        ]
        defaultIndex: 0
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: openDrtLookModel
        objectName: "displayOpenDrtLookModel"
        fieldKey: "odt"
        label: qsTr("Look")
        entries: [
            { value: "standard", label: qsTr("Standard") },
            { value: "arriba", label: qsTr("Arriba") },
            { value: "sylvan", label: qsTr("Sylvan") },
            { value: "colorful", label: qsTr("Colorful") },
            { value: "aery", label: qsTr("Aery") },
            { value: "dystopic", label: qsTr("Dystopic") },
            { value: "umbra", label: qsTr("Umbra") },
            { value: "custom", label: qsTr("Custom") }
        ]
        defaultIndex: 0
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: openDrtTonescaleModel
        objectName: "displayOpenDrtTonescaleModel"
        fieldKey: "odt"
        label: qsTr("Tonescale")
        entries: [
            { value: "use_look_preset", label: qsTr("Use Look Preset") },
            { value: "low_contrast", label: qsTr("Low Contrast") },
            { value: "medium_contrast", label: qsTr("Medium Contrast") },
            { value: "high_contrast", label: qsTr("High Contrast") },
            { value: "arriba_tonescale", label: qsTr("Arriba Tonescale") },
            { value: "sylvan_tonescale", label: qsTr("Sylvan Tonescale") },
            { value: "colorful_tonescale", label: qsTr("Colorful Tonescale") },
            { value: "aery_tonescale", label: qsTr("Aery Tonescale") },
            { value: "dystopic_tonescale", label: qsTr("Dystopic Tonescale") },
            { value: "umbra_tonescale", label: qsTr("Umbra Tonescale") },
            { value: "aces_1_x", label: qsTr("ACES 1.x") },
            { value: "aces_2_0", label: qsTr("ACES 2.0") },
            { value: "marvelous_tonscape", label: qsTr("Marvelous Tonscape") },
            { value: "dagrinchi_tonegroan", label: qsTr("Dagrinchi Tonegroan") },
            { value: "custom", label: qsTr("Custom") }
        ]
        defaultIndex: 0
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: openDrtCreativeWhiteModel
        objectName: "displayOpenDrtCreativeWhiteModel"
        fieldKey: "odt"
        label: qsTr("Creative White")
        entries: [
            { value: "use_look_preset", label: qsTr("Use Look Preset") },
            { value: "d93", label: qsTr("D93") },
            { value: "d75", label: qsTr("D75") },
            { value: "d65", label: qsTr("D65") },
            { value: "d60", label: qsTr("D60") },
            { value: "d55", label: qsTr("D55") },
            { value: "d50", label: qsTr("D50") }
        ]
        defaultIndex: 0
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
