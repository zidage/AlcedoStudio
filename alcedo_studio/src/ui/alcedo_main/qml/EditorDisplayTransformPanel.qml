import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Phase 6E Display Transform panel: output color-space selection, CST/ODT,
// ACES tone mapping, transfer function (EOTF), peak luminance, and HDR
// display intent. Method-specific controls for ACES 2.0 (limiting space) and
// OpenDRT (look/tonescale/creative-white presets), and the film print (process
// switch, film browser, Format and Film Grain, print material browser, scene light, three
// sliders; a print
// has Rec. 709 SDR output, so its page has no encoding controls; print material
// Negative Scan has its scan settings and the OpenDRT settings and encodings). All models share
// fieldKey
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
    /// EditorBehaviorPreferences (appModules.editorBehavior): the remembered film print settings
    /// (issue #672). Null in tests that do not set it; then the film print starts at the defaults.
    property var editorBehavior: null
    /// True when the document has a RAW white balance for scene light As Shot: a RAW input whose
    /// as-shot temperature resolves (issue #672). Without it the Scene Light list has no As Shot,
    /// and a stored As Shot shows Reference, the light that it renders.
    property bool filmAsShotAvailable: false
    /// True while the Scene Light shows Reference for a stored As Shot with no RAW white balance.
    /// A write then keeps As Shot, so the document and the remembered settings keep it for a RAW
    /// input. A selection of the user ends it.
    property bool filmSceneLightKeepsAsShot: false

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colBase: theme ? theme.colBgBase : appTheme.bgBaseColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor

    readonly property var odtDefaults: parameterCatalog.uiDefault("odt")
    readonly property var peakSpec: parameterCatalog.propertySpec("odt", "peak_luminance")
    readonly property var filmLightSpreadSpec: parameterCatalog.propertySpec("odt", "light_spread")
    readonly property var filmHalationSpec: parameterCatalog.propertySpec("odt", "halation")
    readonly property var filmEnlargerFlareSpec: parameterCatalog.propertySpec("odt", "enlarger_flare")
    readonly property var filmScanAutoStrengthModelSpec: parameterCatalog.propertySpec("odt", "auto_strength")
    readonly property var filmScanAutoDensityModelSpec: parameterCatalog.propertySpec("odt", "auto_density")
    readonly property var filmScanContrastModelSpec: parameterCatalog.propertySpec("odt", "scan_contrast")
    readonly property var filmScanExposureModelSpec: parameterCatalog.propertySpec("odt", "scan_exposure_stops")
    readonly property var filmScanCyanModelSpec: parameterCatalog.propertySpec("odt", "cyan")
    readonly property var filmScanMagentaModelSpec: parameterCatalog.propertySpec("odt", "magenta")
    readonly property var filmScanYellowModelSpec: parameterCatalog.propertySpec("odt", "yellow")
    readonly property var filmScanBlackModelSpec: parameterCatalog.propertySpec("odt", "black")
    readonly property var filmScannerRedModelSpec: parameterCatalog.propertySpec("odt", "scanner_red_nm")
    readonly property var filmScannerGreenModelSpec: parameterCatalog.propertySpec("odt", "scanner_green_nm")
    readonly property var filmScannerBlueModelSpec: parameterCatalog.propertySpec("odt", "scanner_blue_nm")
    readonly property var filmScannerWidthModelSpec: parameterCatalog.propertySpec("odt", "scanner_width_nm")

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

    // ── Film print browser data (issue #657) ─────────────────────────────────
    readonly property var filmBrands: parameterCatalog.filmBrands()
    readonly property var negativeBrowserEntries: parameterCatalog.filmBrowserEntries("negative")
    readonly property var reversalBrowserEntries: parameterCatalog.filmBrowserEntries("reversal")
    readonly property var printBrowserEntries: parameterCatalog.filmBrowserEntries("print")
    /// The last film of each process in this panel session. A change of process selects it.
    property var lastFilmOfProcess: ({ negative: "", reversal: "" })

    /// Translated family caption of the film print browser.
    function filmFamilyText(family) {
        return qsTr(String(family))
    }

    /// Select film @p value through the Film model (one write of the complete ODT state).
    function selectFilm(value) {
        const index = root.indexOfValue(filmNegativeModel.entries, value)
        if (index >= 0 && index !== filmNegativeModel.currentIndex)
            filmNegativeModel.selectIndex(index)
    }

    function selectPrintMaterial(value) {
        const index = root.indexOfValue(filmPrintMaterialModel.entries, value)
        if (index >= 0 && index !== filmPrintMaterialModel.currentIndex)
            filmPrintMaterialModel.selectIndex(index)
    }

    /// A change of process selects the last film of that process, else its default film. The
    /// stored print material does not change.
    function selectFilmProcess(process) {
        if (process === root.filmProcess)
            return
        const last = String(root.lastFilmOfProcess[process] || "")
        root.selectFilm(last.length > 0 ? last : parameterCatalog.filmDefault(process))
    }

    /// Film Grain strength in UI percent that a selection of method Film Print sets when the
    /// stored strength is 0: the datasheet granularity of the film.
    readonly property real filmPrintGrainPercent: 100

    /// A selection of method Film Print on another method applies the remembered film print
    /// settings in the same write (issue #672). The film print of a document is not written when
    /// it has another method, so without them the selection starts at the defaults.
    ///
    /// The film print has its grain in the negative, so the selection also turns on Film Grain at
    /// @ref filmPrintGrainPercent when its stored strength is 0. A stored strength stays. Film
    /// Grain is a field of its own, so this is a second settled write: a second history entry
    /// after the method.
    function selectMethod(index, value) {
        const entersFilmPrint = String(value) === "film_print" && !root.isFilmPrint
        if (entersFilmPrint && root.editorBehavior) {
            const remembered = root.editorBehavior.filmPrintUiValue
            if (remembered && remembered.method === "film_print")
                root.loadFilmValues(remembered)
        }
        methodModel.selectIndex(index)
        if (entersFilmPrint && !(filmGrainModel.value > 0)) {
            filmGrainModel.editValue(root.filmPrintGrainPercent)
            filmGrainModel.commitImmediately()
        }
    }

    /// Remember the film print settings after a settled user write with method Film Print.
    function rememberFilmPrint() {
        if (root.isFilmPrint && root.editorBehavior)
            root.editorBehavior.rememberFilmPrint(root.buildOdtParams())
    }

    /// Scene Light entries: As Shot only when the document has a RAW white balance.
    function sceneLightEntries(asShotAvailable) {
        const all = root.catalogEntries("scene_light")
        if (asShotAvailable)
            return all
        return all.filter(function(entry) { return entry.value !== "as_shot" })
    }

    /// The scene light that the panel shows for the value @p value: As Shot with no RAW white
    /// balance renders as Reference.
    function shownSceneLight(value) {
        return String(value) === "as_shot" && !root.filmAsShotAvailable ? "reference" : value
    }

    /// The projected As Shot availability of the snapshot. A snapshot with no value has none.
    function snapshotAsShotAvailable(snapshot) {
        const wrap = snapshot ? snapshot["odt"] : undefined
        if (wrap === undefined || wrap === null)
            return false
        const odt = wrap["odt"] !== undefined ? wrap["odt"] : wrap
        return odt["film_as_shot_available"] === true
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
            creative_white: root.selectedValue(openDrtCreativeWhiteModel, "creative_white"),
            negative: root.selectedValue(filmNegativeModel, "negative"),
            print_material: root.selectedValue(filmPrintMaterialModel, "print_material"),
            scene_light: root.filmSceneLightKeepsAsShot
                         ? "as_shot" : root.selectedValue(filmSceneLightModel, "scene_light"),
            light_spread: filmLightSpreadModel.value,
            halation: filmHalationModel.value,
            enlarger_flare: filmEnlargerFlareModel.value,
            "auto": root.selectedValue(filmScanAutoModel, "auto"),
            scanner_light: root.selectedValue(filmScannerLightModel, "scanner_light"),
            auto_strength: filmScanAutoStrengthModel.value,
            auto_density: filmScanAutoDensityModel.value,
            scan_contrast: filmScanContrastModel.value,
            scan_exposure_stops: filmScanExposureModel.value,
            cyan: filmScanCyanModel.value,
            magenta: filmScanMagentaModel.value,
            yellow: filmScanYellowModel.value,
            black: filmScanBlackModel.value,
            scanner_red_nm: filmScannerRedModel.value,
            scanner_green_nm: filmScannerGreenModel.value,
            scanner_blue_nm: filmScannerBlueModel.value,
            scanner_width_nm: filmScannerWidthModel.value
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
        filmNegativeModel.enabled = on
        filmPrintMaterialModel.enabled = on
        filmSceneLightModel.enabled = on
        filmLightSpreadModel.enabled = on
        filmHalationModel.enabled = on
        filmEnlargerFlareModel.enabled = on
        filmScanAutoModel.enabled = on
        filmScannerLightModel.enabled = on
        filmScanAutoStrengthModel.enabled = on
        filmScanAutoDensityModel.enabled = on
        filmScanContrastModel.enabled = on
        filmScanExposureModel.enabled = on
        filmScanCyanModel.enabled = on
        filmScanMagentaModel.enabled = on
        filmScanYellowModel.enabled = on
        filmScanBlackModel.enabled = on
        filmScannerRedModel.enabled = on
        filmScannerGreenModel.enabled = on
        filmScannerBlueModel.enabled = on
        filmScannerWidthModel.enabled = on
        filmFormatModel.enabled = on
        filmGrainModel.enabled = on
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
        filmNegativeModel.currentIndex = filmNegativeModel.defaultIndex
        filmPrintMaterialModel.currentIndex = filmPrintMaterialModel.defaultIndex
        filmSceneLightModel.currentIndex = filmSceneLightModel.defaultIndex
        root.filmSceneLightKeepsAsShot = false
        filmLightSpreadModel.value = filmLightSpreadModel.defaultValue
        filmHalationModel.value = filmHalationModel.defaultValue
        filmEnlargerFlareModel.value = filmEnlargerFlareModel.defaultValue
        filmScanAutoModel.currentIndex = filmScanAutoModel.defaultIndex
        filmScannerLightModel.currentIndex = filmScannerLightModel.defaultIndex
        filmScanAutoStrengthModel.value = filmScanAutoStrengthModel.defaultValue
        filmScanAutoDensityModel.value = filmScanAutoDensityModel.defaultValue
        filmScanContrastModel.value = filmScanContrastModel.defaultValue
        filmScanExposureModel.value = filmScanExposureModel.defaultValue
        filmScanCyanModel.value = filmScanCyanModel.defaultValue
        filmScanMagentaModel.value = filmScanMagentaModel.defaultValue
        filmScanYellowModel.value = filmScanYellowModel.defaultValue
        filmScanBlackModel.value = filmScanBlackModel.defaultValue
        filmScannerRedModel.value = filmScannerRedModel.defaultValue
        filmScannerGreenModel.value = filmScannerGreenModel.defaultValue
        filmScannerBlueModel.value = filmScannerBlueModel.defaultValue
        filmScannerWidthModel.value = filmScannerWidthModel.defaultValue
        filmFormatModel.currentIndex = filmFormatModel.defaultIndex
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

    /// Load-only slider write. A drag in progress keeps its value.
    function setValueFromSnapshot(model, value) {
        if (model.dragActive)
            return
        const number = Number(value)
        if (!isNaN(number) && Math.abs(model.value - number) > (model.step * 0.1))
            model.value = number
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
        const odtModels = [methodModel, encodingSpaceModel, encodingEotfModel, peakLuminanceModel,
            acesLimitingSpaceModel, openDrtLookModel, openDrtTonescaleModel,
            openDrtCreativeWhiteModel, filmNegativeModel, filmPrintMaterialModel,
            filmSceneLightModel, filmLightSpreadModel, filmHalationModel, filmEnlargerFlareModel,
            filmScanAutoModel, filmScannerLightModel, filmScanAutoStrengthModel,
            filmScanAutoDensityModel, filmScanContrastModel, filmScanExposureModel,
            filmScanCyanModel, filmScanMagentaModel, filmScanYellowModel, filmScanBlackModel,
            filmScannerRedModel, filmScannerGreenModel, filmScannerBlueModel,
            filmScannerWidthModel]
        for (var i = 0; i < odtModels.length; ++i)
            odtModels[i].settledWriteAccepted.connect(root.rememberFilmPrint)
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
        // The film format and Film Grain are fields of their own (film_format, film_grain), so they
        // load with any method.
        root.loadFilmFormat(snapshot)
        if (snapshot !== undefined && snapshot !== null)
            filmGrainModel.loadFromSnapshot(snapshot)
        const odt = odtUiValue(snapshot)
        if (odt === null)
            return

        // Before the film values: it chooses the Scene Light entries.
        root.filmAsShotAvailable = root.snapshotAsShotAvailable(snapshot)
        setEnumFromSnapshot(methodModel, odt["method"])
        setEnumFromSnapshot(encodingSpaceModel, odt["encoding_space"])

        // EOTF entries depend on encoding space; rebuild then restore.
        updateEotfOptions()
        setEnumFromSnapshot(encodingEotfModel, odt["encoding_eotf"])

        setValueFromSnapshot(peakLuminanceModel, odt["peak_luminance"])

        setEnumFromSnapshot(acesLimitingSpaceModel, odt["limiting_space"])
        setEnumFromSnapshot(openDrtLookModel, odt["look_preset"])
        setEnumFromSnapshot(openDrtTonescaleModel, odt["tonescale_preset"])
        setEnumFromSnapshot(openDrtCreativeWhiteModel, odt["creative_white"])
        loadFilmValues(odt)
    }

    /// Load the film print fields of the ODT UI value @p odt into the film print models. Loading
    /// never submits.
    function loadFilmValues(odt) {
        setEnumFromSnapshot(filmNegativeModel, odt["negative"])
        setEnumFromSnapshot(filmPrintMaterialModel, odt["print_material"])
        if (odt["scene_light"] !== undefined)
            root.filmSceneLightKeepsAsShot =
                String(odt["scene_light"]) === "as_shot" && !root.filmAsShotAvailable
        setEnumFromSnapshot(filmSceneLightModel, root.shownSceneLight(odt["scene_light"]))
        setValueFromSnapshot(filmLightSpreadModel, odt["light_spread"])
        setValueFromSnapshot(filmHalationModel, odt["halation"])
        setValueFromSnapshot(filmEnlargerFlareModel, odt["enlarger_flare"])
        setEnumFromSnapshot(filmScanAutoModel, odt["auto"])
        setEnumFromSnapshot(filmScannerLightModel, odt["scanner_light"])
        setValueFromSnapshot(filmScanAutoStrengthModel, odt["auto_strength"])
        setValueFromSnapshot(filmScanAutoDensityModel, odt["auto_density"])
        setValueFromSnapshot(filmScanContrastModel, odt["scan_contrast"])
        setValueFromSnapshot(filmScanExposureModel, odt["scan_exposure_stops"])
        setValueFromSnapshot(filmScanCyanModel, odt["cyan"])
        setValueFromSnapshot(filmScanMagentaModel, odt["magenta"])
        setValueFromSnapshot(filmScanYellowModel, odt["yellow"])
        setValueFromSnapshot(filmScanBlackModel, odt["black"])
        setValueFromSnapshot(filmScannerRedModel, odt["scanner_red_nm"])
        setValueFromSnapshot(filmScannerGreenModel, odt["scanner_green_nm"])
        setValueFromSnapshot(filmScannerBlueModel, odt["scanner_blue_nm"])
        setValueFromSnapshot(filmScannerWidthModel, odt["scanner_width_nm"])
    }

    // The film format of the DRT node: one write of film_format per selection.
    EditorAdjustmentEnumModel {
        id: filmFormatModel
        objectName: "displayFilmFormatModel"
        fieldKey: "film_format"
        label: qsTr("Format")
        entries: parameterCatalog.options("film_format", "film_format")
        defaultIndex: Math.max(0, root.indexOfFilmFormat(
                                   parameterCatalog.uiDefault("film_format").film_format))
        submitter: root.editorSession
        paramsBuilder: function(value) {
            return parameterCatalog.modelParamsJson("film_format", { film_format: value })
        }
    }

    // Film Grain of the DRT node. With the film print it sets the grain of the negative, so the
    // slider is on this page under Format, and the Post Processing page does not show it.
    EditorAdjustmentValueModel {
        id: filmGrainModel
        objectName: "displayFilmGrainModel"
        fieldKey: "film_grain"
        label: qsTr("Film Grain")
        submitter: root.editorSession
    }

    /// Index of the format identifier @p value in the Format entries, or -1.
    function indexOfFilmFormat(value) {
        const entries = parameterCatalog.options("film_format", "film_format")
        for (var i = 0; i < entries.length; ++i) {
            if (String(entries[i].value) === String(value))
                return i
        }
        return -1
    }

    /// Load the film format of the snapshot into the Format model. Loading never submits.
    function loadFilmFormat(snapshot) {
        if (snapshot === undefined || snapshot === null)
            return
        const entry = snapshot["film_format"]
        if (entry === undefined || entry === null)
            return
        const index = root.indexOfFilmFormat(parameterCatalog.uiValue("film_format", entry).film_format)
        if (index >= 0 && index !== filmFormatModel.currentIndex)
            filmFormatModel.currentIndex = index
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

    EditorAdjustmentEnumModel {
        id: filmNegativeModel
        objectName: "displayFilmNegativeModel"
        fieldKey: "odt"
        label: qsTr("Film")
        entries: root.catalogEntries("negative")
        defaultIndex: root.defaultIndexOf(entries, "negative")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: filmPrintMaterialModel
        objectName: "displayFilmPrintMaterialModel"
        fieldKey: "odt"
        label: qsTr("Print Material")
        entries: root.catalogEntries("print_material")
        defaultIndex: root.defaultIndexOf(entries, "print_material")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: filmSceneLightModel
        objectName: "displayFilmSceneLightModel"
        fieldKey: "odt"
        label: qsTr("Scene Light")
        entries: root.sceneLightEntries(root.filmAsShotAvailable)
        // With no As Shot entry the default is the first entry, Reference.
        defaultIndex: root.defaultIndexOf(entries, "scene_light")
        submitter: root.editorSession
        // A selection of the user replaces a stored As Shot that shows as Reference.
        paramsBuilder: function() {
            root.filmSceneLightKeepsAsShot = false
            return root.buildOdtParams()
        }
    }

    EditorAdjustmentValueModel {
        id: filmLightSpreadModel
        objectName: "displayFilmLightSpreadModel"
        fieldKey: "odt"
        label: qsTr("Light Spread")
        minimum: root.filmLightSpreadSpec.ui_min
        maximum: root.filmLightSpreadSpec.ui_max
        defaultValue: root.odtDefaults.light_spread
        step: root.filmLightSpreadSpec.ui_step
        precision: root.filmLightSpreadSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmHalationModel
        objectName: "displayFilmHalationModel"
        fieldKey: "odt"
        label: qsTr("Halation")
        minimum: root.filmHalationSpec.ui_min
        maximum: root.filmHalationSpec.ui_max
        defaultValue: root.odtDefaults.halation
        step: root.filmHalationSpec.ui_step
        precision: root.filmHalationSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmEnlargerFlareModel
        objectName: "displayFilmEnlargerFlareModel"
        fieldKey: "odt"
        // A slide has no print, so this slider is the flare of the loupe on the light table.
        label: root.isReversalFilm ? qsTr("Loupe Flare") : qsTr("Enlarger Flare")
        minimum: root.filmEnlargerFlareSpec.ui_min
        maximum: root.filmEnlargerFlareSpec.ui_max
        defaultValue: root.odtDefaults.enlarger_flare
        step: root.filmEnlargerFlareSpec.ui_step
        precision: root.filmEnlargerFlareSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    // Negative Scan (issue 661). Only that print material reads these settings.
    EditorAdjustmentEnumModel {
        id: filmScanAutoModel
        objectName: "displayFilmScanAutoModel"
        fieldKey: "odt"
        label: qsTr("Auto Color")
        entries: root.catalogEntries("auto")
        defaultIndex: root.defaultIndexOf(entries, "auto")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentEnumModel {
        id: filmScannerLightModel
        objectName: "displayFilmScannerLightModel"
        fieldKey: "odt"
        label: qsTr("Scanner Light")
        entries: root.catalogEntries("scanner_light")
        defaultIndex: root.defaultIndexOf(entries, "scanner_light")
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScanAutoStrengthModel
        objectName: "displayFilmScanAutoStrengthModel"
        fieldKey: "odt"
        label: qsTr("Auto Color Strength")
        minimum: root.filmScanAutoStrengthModelSpec.ui_min
        maximum: root.filmScanAutoStrengthModelSpec.ui_max
        defaultValue: root.odtDefaults.auto_strength
        step: root.filmScanAutoStrengthModelSpec.ui_step
        precision: root.filmScanAutoStrengthModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScanAutoDensityModel
        objectName: "displayFilmScanAutoDensityModel"
        fieldKey: "odt"
        label: qsTr("Auto Density")
        minimum: root.filmScanAutoDensityModelSpec.ui_min
        maximum: root.filmScanAutoDensityModelSpec.ui_max
        defaultValue: root.odtDefaults.auto_density
        step: root.filmScanAutoDensityModelSpec.ui_step
        precision: root.filmScanAutoDensityModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScanContrastModel
        objectName: "displayFilmScanContrastModel"
        fieldKey: "odt"
        label: qsTr("Scan Contrast")
        minimum: root.filmScanContrastModelSpec.ui_min
        maximum: root.filmScanContrastModelSpec.ui_max
        defaultValue: root.odtDefaults.scan_contrast
        step: root.filmScanContrastModelSpec.ui_step
        precision: root.filmScanContrastModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScanExposureModel
        objectName: "displayFilmScanExposureModel"
        fieldKey: "odt"
        label: qsTr("Scan Exposure")
        minimum: root.filmScanExposureModelSpec.ui_min
        maximum: root.filmScanExposureModelSpec.ui_max
        defaultValue: root.odtDefaults.scan_exposure_stops
        step: root.filmScanExposureModelSpec.ui_step
        precision: root.filmScanExposureModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScanCyanModel
        objectName: "displayFilmScanCyanModel"
        fieldKey: "odt"
        label: qsTr("Cyan")
        minimum: root.filmScanCyanModelSpec.ui_min
        maximum: root.filmScanCyanModelSpec.ui_max
        defaultValue: root.odtDefaults.cyan
        step: root.filmScanCyanModelSpec.ui_step
        precision: root.filmScanCyanModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScanMagentaModel
        objectName: "displayFilmScanMagentaModel"
        fieldKey: "odt"
        label: qsTr("Magenta")
        minimum: root.filmScanMagentaModelSpec.ui_min
        maximum: root.filmScanMagentaModelSpec.ui_max
        defaultValue: root.odtDefaults.magenta
        step: root.filmScanMagentaModelSpec.ui_step
        precision: root.filmScanMagentaModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScanYellowModel
        objectName: "displayFilmScanYellowModel"
        fieldKey: "odt"
        label: qsTr("Yellow")
        minimum: root.filmScanYellowModelSpec.ui_min
        maximum: root.filmScanYellowModelSpec.ui_max
        defaultValue: root.odtDefaults.yellow
        step: root.filmScanYellowModelSpec.ui_step
        precision: root.filmScanYellowModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScanBlackModel
        objectName: "displayFilmScanBlackModel"
        fieldKey: "odt"
        label: qsTr("Black at Film Base")
        minimum: root.filmScanBlackModelSpec.ui_min
        maximum: root.filmScanBlackModelSpec.ui_max
        defaultValue: root.odtDefaults.black
        step: root.filmScanBlackModelSpec.ui_step
        precision: root.filmScanBlackModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScannerRedModel
        objectName: "displayFilmScannerRedModel"
        fieldKey: "odt"
        label: qsTr("Scanner Red (nm)")
        minimum: root.filmScannerRedModelSpec.ui_min
        maximum: root.filmScannerRedModelSpec.ui_max
        defaultValue: root.odtDefaults.scanner_red_nm
        step: root.filmScannerRedModelSpec.ui_step
        precision: root.filmScannerRedModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScannerGreenModel
        objectName: "displayFilmScannerGreenModel"
        fieldKey: "odt"
        label: qsTr("Scanner Green (nm)")
        minimum: root.filmScannerGreenModelSpec.ui_min
        maximum: root.filmScannerGreenModelSpec.ui_max
        defaultValue: root.odtDefaults.scanner_green_nm
        step: root.filmScannerGreenModelSpec.ui_step
        precision: root.filmScannerGreenModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScannerBlueModel
        objectName: "displayFilmScannerBlueModel"
        fieldKey: "odt"
        label: qsTr("Scanner Blue (nm)")
        minimum: root.filmScannerBlueModelSpec.ui_min
        maximum: root.filmScannerBlueModelSpec.ui_max
        defaultValue: root.odtDefaults.scanner_blue_nm
        step: root.filmScannerBlueModelSpec.ui_step
        precision: root.filmScannerBlueModelSpec.ui_decimals
        submitter: root.editorSession
        paramsBuilder: root.buildOdtParams
    }

    EditorAdjustmentValueModel {
        id: filmScannerWidthModel
        objectName: "displayFilmScannerWidthModel"
        fieldKey: "odt"
        label: qsTr("Scanner Band Width (nm)")
        minimum: root.filmScannerWidthModelSpec.ui_min
        maximum: root.filmScannerWidthModelSpec.ui_max
        defaultValue: root.odtDefaults.scanner_width_nm
        step: root.filmScannerWidthModelSpec.ui_step
        precision: root.filmScannerWidthModelSpec.ui_decimals
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
    readonly property bool isFilmPrint: root.selectedMethodValue === "film_print"
    // A colour reversal film has no print material. The stored print material does not change.
    readonly property bool isReversalFilm: {
        var _dep = filmNegativeModel.currentIndex
        return parameterCatalog.filmIsReversal(root.selectedValue(filmNegativeModel, "negative"))
    }
    /// Process of the stored film. The document does not store it.
    readonly property string filmProcess: root.isReversalFilm ? "reversal" : "negative"
    readonly property string selectedFilmValue: {
        var _dep = filmNegativeModel.currentIndex
        return root.selectedValue(filmNegativeModel, "negative")
    }
    readonly property string selectedPrintMaterialValue: {
        var _dep = filmPrintMaterialModel.currentIndex
        return root.selectedValue(filmPrintMaterialModel, "print_material")
    }
    // Negative Scan has no enlarger: the Enlarger Flare slider hides, and its stored value stays.
    readonly property bool isNegativeScan: parameterCatalog.filmIsNegativeScan(
        root.selectedFilmValue, root.selectedPrintMaterialValue)

    Connections {
        target: filmNegativeModel
        function onCurrentIndexChanged() {
            const film = root.selectedValue(filmNegativeModel, "negative")
            root.lastFilmOfProcess[parameterCatalog.filmIsReversal(film) ? "reversal" : "negative"] = film
        }
    }

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
    Flickable {
        id: displayScroll
        objectName: "editorDisplayTransformPanelScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: displayColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick

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
                var maxY = Math.max(0, displayScroll.contentHeight - displayScroll.height)
                displayScroll.contentY = Math.max(0, Math.min(maxY, displayScroll.contentY - step))
                event.accepted = true
            }
        }

        ColumnLayout {
            id: displayColumn
            width: displayScroll.width
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
                    onSelected: function(index, value) { root.selectMethod(index, value) }
                }

                // Method-specific params (inline under the track).
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceSm
                    visible: !root.isOpenDrt && !root.isFilmPrint

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

                ColumnLayout {
                    objectName: "displayFilmPrintControls"
                    Layout.fillWidth: true
                    spacing: appTheme.spaceSm
                    visible: root.isFilmPrint

                    // Process: derived from the stored film, not stored.
                    SegmentedCardSwitcher {
                        objectName: "displayFilmProcessTrack"
                        Layout.fillWidth: true
                        entries: [
                            { value: "negative", label: qsTr("Negative"),
                              shape: "photo_negative", colors: ["#c9772f", "#7a3a17"] },
                            { value: "reversal", label: qsTr("Reversal"),
                              shape: "reversal", colors: ["#2d6fd6", "#8edcf0"] }
                        ]
                        currentValue: root.filmProcess
                        enabled: root.controlsEnabled
                        trackColor: root.colBase
                        trackBorderColor: root.colCardBorder
                        textColor: root.colText
                        hoverColor: root.colHover
                        segmentDecoration: Component {
                            FilmSwatch {
                                readonly property var segmentEntry: parent ? parent.entry : null
                                readonly property bool slide: segmentEntry !== null
                                                              && segmentEntry.shape === "reversal"
                                width: slide ? 13 : 16
                                height: slide ? 13 : 11
                                cornerRadius: 2
                                shape: segmentEntry ? segmentEntry.shape : ""
                                colors: segmentEntry ? segmentEntry.colors : []
                            }
                        }
                        onSelected: function(index, value) { root.selectFilmProcess(value) }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: appTheme.spaceXs
                        text: qsTr("Film")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    FilmStockPicker {
                        objectName: "displayFilmNegativePicker"
                        Layout.fillWidth: true
                        entries: root.isReversalFilm ? root.reversalBrowserEntries
                                                     : root.negativeBrowserEntries
                        brands: root.filmBrands
                        currentValue: root.selectedFilmValue
                        controlsEnabled: root.controlsEnabled
                        accessibleName: qsTr("Film")
                        searchPlaceholder: qsTr("Search films")
                        emptyText: qsTr("No films match")
                        familyText: root.filmFamilyText
                        onPicked: function(value) { root.selectFilm(value) }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: appTheme.spaceXs
                        text: qsTr("Format")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    // One write of film_format per click. The Post Processing page and the
                    // Display Transform page write the same field, so both show its value.
                    SegmentedCardSwitcher {
                        objectName: "displayFilmFormatControl"
                        Layout.fillWidth: true
                        entries: filmFormatModel.entries
                        currentIndex: filmFormatModel.currentIndex
                        currentValue: filmFormatModel.currentValue
                        enabled: root.controlsEnabled
                        trackColor: root.colBase
                        trackBorderColor: root.colCardBorder
                        textColor: root.colText
                        hoverColor: root.colHover
                        onSelected: function(index, value) { filmFormatModel.selectIndex(index) }
                    }

                    AdjustmentSlider {
                        objectName: "displayFilmGrainSlider"
                        Layout.fillWidth: true
                        model: filmGrainModel
                        flickable: displayScroll
                    }

                    // A colour reversal film has no print: the page has no print section.
                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: appTheme.spaceXs
                        visible: !root.isReversalFilm
                        text: qsTr("Print")
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    FilmStockPicker {
                        objectName: "displayFilmPrintMaterialPicker"
                        Layout.fillWidth: true
                        visible: !root.isReversalFilm
                        entries: root.printBrowserEntries
                        brands: root.filmBrands
                        currentValue: root.selectedPrintMaterialValue
                        controlsEnabled: root.controlsEnabled
                        showFamilyOnCard: true
                        accessibleName: qsTr("Print Material")
                        searchPlaceholder: qsTr("Search print materials")
                        emptyText: qsTr("No print materials match")
                        familyText: root.filmFamilyText
                        onPicked: function(value) { root.selectPrintMaterial(value) }
                    }

                    AdjustmentCombo {
                        objectName: "displayFilmSceneLightCombo"
                        Layout.fillWidth: true
                        model: filmSceneLightModel
                    }

                    AdjustmentSlider {
                        objectName: "displayFilmLightSpreadSlider"
                        Layout.fillWidth: true
                        model: filmLightSpreadModel
                        flickable: displayScroll
                    }

                    AdjustmentSlider {
                        objectName: "displayFilmHalationSlider"
                        Layout.fillWidth: true
                        model: filmHalationModel
                        flickable: displayScroll
                    }

                    AdjustmentSlider {
                        objectName: "displayFilmEnlargerFlareSlider"
                        Layout.fillWidth: true
                        visible: !root.isNegativeScan
                        model: filmEnlargerFlareModel
                        flickable: displayScroll
                    }

                    // Negative Scan: the rendering settings act after the scan; the scanner
                    // settings build the scanner table again (issue 661).
                    ColumnLayout {
                        objectName: "displayNegativeScanControls"
                        Layout.fillWidth: true
                        spacing: appTheme.spaceSm
                        visible: root.isNegativeScan

                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: appTheme.spaceXs
                            text: qsTr("Negative Scan")
                            color: root.colMuted
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }

                        AdjustmentCombo {
                            objectName: "displayFilmScanAutoCombo"
                            Layout.fillWidth: true
                            model: filmScanAutoModel
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScanAutoStrengthSlider"
                            Layout.fillWidth: true
                            model: filmScanAutoStrengthModel
                            flickable: displayScroll
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScanAutoDensitySlider"
                            Layout.fillWidth: true
                            model: filmScanAutoDensityModel
                            flickable: displayScroll
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScanContrastSlider"
                            Layout.fillWidth: true
                            model: filmScanContrastModel
                            flickable: displayScroll
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScanExposureSlider"
                            Layout.fillWidth: true
                            model: filmScanExposureModel
                            flickable: displayScroll
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScanCyanSlider"
                            Layout.fillWidth: true
                            model: filmScanCyanModel
                            flickable: displayScroll
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScanMagentaSlider"
                            Layout.fillWidth: true
                            model: filmScanMagentaModel
                            flickable: displayScroll
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScanYellowSlider"
                            Layout.fillWidth: true
                            model: filmScanYellowModel
                            flickable: displayScroll
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScanBlackSlider"
                            Layout.fillWidth: true
                            model: filmScanBlackModel
                            flickable: displayScroll
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: appTheme.spaceXs
                            text: qsTr("Scanner")
                            color: root.colMuted
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScannerRedSlider"
                            Layout.fillWidth: true
                            model: filmScannerRedModel
                            flickable: displayScroll
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScannerGreenSlider"
                            Layout.fillWidth: true
                            model: filmScannerGreenModel
                            flickable: displayScroll
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScannerBlueSlider"
                            Layout.fillWidth: true
                            model: filmScannerBlueModel
                            flickable: displayScroll
                        }

                        AdjustmentSlider {
                            objectName: "displayFilmScannerWidthSlider"
                            Layout.fillWidth: true
                            model: filmScannerWidthModel
                            flickable: displayScroll
                        }

                        AdjustmentCombo {
                            objectName: "displayFilmScannerLightCombo"
                            Layout.fillWidth: true
                            model: filmScannerLightModel
                        }
                    }

                    // Negative Scan ends in linear AP1 that OpenDRT shows, with the settings of
                    // method OpenDRT. The models are those of that method, so a change of method
                    // keeps the values.
                    ColumnLayout {
                        objectName: "displayNegativeScanOpenDrtControls"
                        Layout.fillWidth: true
                        spacing: appTheme.spaceSm
                        visible: root.isNegativeScan

                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: appTheme.spaceXs
                            text: qsTr("OpenDRT")
                            color: root.colMuted
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }

                        AdjustmentCombo {
                            objectName: "displayNegativeScanOpenDrtLookCombo"
                            Layout.fillWidth: true
                            model: openDrtLookModel
                        }

                        AdjustmentCombo {
                            objectName: "displayNegativeScanOpenDrtTonescaleCombo"
                            Layout.fillWidth: true
                            model: openDrtTonescaleModel
                        }

                        AdjustmentCombo {
                            objectName: "displayNegativeScanOpenDrtCreativeWhiteCombo"
                            Layout.fillWidth: true
                            model: openDrtCreativeWhiteModel
                        }
                    }
                }
            }

            // ── Color & Encoding (collapsible, default expanded) ────────────
            CollapsibleSection {
                id: colorEncodingGroup
                objectName: "editorAdjustmentGroupShell_display_color_encoding"
                Layout.fillWidth: true
                // A print encodes Rec. 709 SDR only. Negative Scan has the encodings of OpenDRT.
                visible: !root.isFilmPrint || root.isNegativeScan
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
                bodyContentHeight: ceBody.implicitHeight + 2 * (appTheme.spaceSm + appTheme.spaceXs)

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
                        flickable: displayScroll
                    }
                }
            }
        }
    }
}
