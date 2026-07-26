import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

Item {
    id: root
    objectName: "editorAdjustmentPanel_geometry"

    property var theme: null
    property var editorSession: null
    property var interaction: null
    property bool controlsEnabled: true
    property bool panelActive: false
    property bool restoring: false
    property bool syncingToInteraction: false
    property bool overlayInputActive: false
    // Geometry draft dirty: crop/rotate edits while the panel is open stay on the
    // overlay until confirm (panel leave or Enter). Pipeline submit is deferred.
    property bool draftDirty: false
    property var aspectEntries: []
    property var lensBrandEntries: []
    property var lensModelEntries: []

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor
    readonly property bool canUseGeometry: root.controlsEnabled && root.interaction !== null
    readonly property double imageAspect: {
        if (!root.interaction)
            return 1.0
        const value = Number(root.interaction.metricAspect)
        return isFinite(value) && value > 0.0001 ? value : 1.0
    }
    readonly property bool inputActive: root.overlayInputActive
                                      || cropXModel.dragActive
                                      || cropYModel.dragActive
                                      || cropWidthModel.dragActive
                                      || cropHeightModel.dragActive
                                      || rotationModel.dragActive
                                      || aspectWidthModel.dragActive
                                      || aspectHeightModel.dragActive

    EditorGeometryMath {
        id: geometryMath
        objectName: "geometryMath"
    }

    EditorLensCatalogModel {
        id: lensCatalog
        objectName: "geometryLensCatalog"
    }

    function buildAspectEntries() {
        var result = []
        const presets = geometryMath.aspectPresets
        for (var i = 0; i < presets.length; ++i) {
            result.push({ value: String(presets[i].value), label: qsTr(String(presets[i].label)) })
        }
        return result
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

    function currentAspectRatio() {
        if (aspectModel.currentValue === "free")
            return 1.0
        if (aspectModel.currentValue === "custom")
            return geometryMath.aspectRatio(aspectWidthModel.value, aspectHeightModel.value)
        const ratio = geometryMath.presetRatio(aspectModel.currentValue)
        return ratio.length >= 2 ? Number(ratio[0]) / Math.max(Number(ratio[1]), 0.0001) : 1.0
    }

    function hasLockedAspect() {
        return geometryMath.hasLockedAspect(aspectModel.currentValue,
                                            aspectWidthModel.value,
                                            aspectHeightModel.value)
    }

    function clampRect(x, y, width, height) {
        const rect = geometryMath.clampCropRect(Number(x), Number(y), Number(width), Number(height))
        return { x: Number(rect[0]), y: Number(rect[1]),
                 w: Number(rect[2]), h: Number(rect[3]) }
    }

    function setRectModels(rect) {
        if (!rect)
            return
        const wasSyncing = root.syncingToInteraction
        root.syncingToInteraction = true
        cropXModel.value = Number(rect.x)
        cropYModel.value = Number(rect.y)
        cropWidthModel.value = Number(rect.width !== undefined ? rect.width : rect.w)
        cropHeightModel.value = Number(rect.height !== undefined ? rect.height : rect.h)
        root.syncingToInteraction = wasSyncing
    }

    function syncToInteraction() {
        if (!root.panelActive || !root.canUseGeometry || root.restoring)
            return
        const rect = root.clampRect(cropXModel.value, cropYModel.value,
                                    cropWidthModel.value, cropHeightModel.value)
        const locked = root.hasLockedAspect()
        const ratio = locked ? root.currentAspectRatio() : 1.0
        const wasSyncing = root.syncingToInteraction
        root.syncingToInteraction = true
        // Panel-owned overlay sync must not emit viewChangeReported: the session
        // controller already requests the source-frame preview when Geometry is
        // selected. Duplicate CropRotate routes race the CUDA resize path.
        const canSuppressRouting = root.interaction
                && typeof root.interaction.setViewChangeRoutingEnabled === "function"
        if (canSuppressRouting)
            root.interaction.setViewChangeRoutingEnabled(false)
        try {
            root.interaction.setCropAspectLock(locked, ratio)
            root.interaction.setCropRotationDegrees(rotationModel.value)
            root.interaction.setCropRectNormalized(Qt.rect(rect.x, rect.y, rect.w, rect.h))
            const applied = root.interaction.cropRectNormalized
            if (applied)
                root.setRectModels(applied)
        } finally {
            if (canSuppressRouting)
                root.interaction.setViewChangeRoutingEnabled(true)
            root.syncingToInteraction = wasSyncing
        }
    }

    function markDraftDirty() {
        if (!root.restoring && !root.syncingToInteraction)
            root.draftDirty = true
    }

    function submitCrop(settled) {
        if (!root.editorSession || typeof root.editorSession.submitPatch !== "function")
            return false
        return root.editorSession.submitPatch("crop_rotate", root.buildCropParams(), settled)
    }

    /// Commit the in-panel crop/rotate draft into the pipeline once. Safe to call
    /// repeatedly: a clean draft is a no-op. Must run before leaving Geometry so
    /// the panel-switch CropRotate refresh sees the new adjustment snapshot.
    function confirmPendingCrop() {
        if (!root.draftDirty)
            return false
        const ok = root.submitCrop(true)
        if (ok)
            root.draftDirty = false
        return ok
    }

    /// Enter / numpad Enter: apply draft crop and return to Tone (legacy shortcut).
    function confirmAndReturnToTone() {
        root.confirmPendingCrop()
        if (root.editorSession)
            root.editorSession.activeAdjustmentPanel = "tone"
    }

    function buildCropParams() {
        const rect = root.clampRect(cropXModel.value, cropYModel.value,
                                    cropWidthModel.value, cropHeightModel.value)
        const locked = root.hasLockedAspect()
        // No UI toggle: crop applies whenever the draft rect is not full-frame.
        const hasCrop = Math.abs(rect.x) > 0.0001 || Math.abs(rect.y) > 0.0001
                        || Math.abs(rect.w - 1.0) > 0.0001
                        || Math.abs(rect.h - 1.0) > 0.0001
        const payload = {
            crop_rotate: {
                enabled: Math.abs(rotationModel.value) > 0.0001 || hasCrop || locked,
                angle_degrees: Number(rotationModel.value),
                enable_crop: hasCrop,
                crop_rect: { x: rect.x, y: rect.y, w: rect.w, h: rect.h },
                // expand_to_fit is ignored under rotated-crop-frame semantics;
                // keep the pipeline default without exposing a control.
                expand_to_fit: true,
                aspect_ratio_preset: String(aspectModel.currentValue),
                aspect_ratio: {
                    width: Number(aspectWidthModel.value),
                    height: Number(aspectHeightModel.value)
                }
            }
        }
        return JSON.stringify(payload)
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

    function applyAspectSelection() {
        const wasSyncing = root.syncingToInteraction
        root.syncingToInteraction = true
        if (aspectModel.currentValue !== "custom") {
            const ratio = geometryMath.presetRatio(aspectModel.currentValue)
            if (ratio.length >= 2) {
                aspectWidthModel.value = Number(ratio[0])
                aspectHeightModel.value = Number(ratio[1])
            } else {
                aspectWidthModel.value = 1.0
                aspectHeightModel.value = 1.0
            }
        }
        if (root.hasLockedAspect()) {
            const rect = geometryMath.maxAspectCropRect(root.imageAspect,
                                                        root.currentAspectRatio())
            root.setRectModels({ x: rect[0], y: rect[1], w: rect[2], h: rect[3] })
        }
        root.syncingToInteraction = wasSyncing
        root.syncToInteraction()
    }

    function resizeLockedRect(useWidthDriver) {
        if (!root.hasLockedAspect()) {
            root.syncToInteraction()
            return
        }
        const rect = geometryMath.resizeAspectCropRect(
                    cropXModel.value, cropYModel.value, cropWidthModel.value,
                    cropHeightModel.value, root.imageAspect, root.currentAspectRatio(),
                    useWidthDriver)
        root.setRectModels({ x: rect[0], y: rect[1], w: rect[2], h: rect[3] })
        root.syncToInteraction()
    }

    function resetCropModels() {
        cropXModel.value = cropXModel.defaultValue
        cropYModel.value = cropYModel.defaultValue
        cropWidthModel.value = cropWidthModel.defaultValue
        cropHeightModel.value = cropHeightModel.defaultValue
        rotationModel.value = rotationModel.defaultValue
        aspectModel.currentIndex = aspectModel.defaultIndex
        aspectWidthModel.value = aspectWidthModel.defaultValue
        aspectHeightModel.value = aspectHeightModel.defaultValue
    }

    function resetLensModels() {
        lensEnabledModel.value = lensEnabledModel.defaultValue
        root.setEnumValue(lensBrandModel, "", 0)
        root.refreshLensModelEntries("", "")
    }

    function resetGeometry() {
        root.restoring = true
        root.resetCropModels()
        root.restoring = false
        root.syncToInteraction()
        // Reset stays draft-only (legacy MarkGeometryEditDirty). Enter / leave
        // commits the full-frame crop when the user confirms.
        root.draftDirty = true
    }

    function resetLens() {
        root.restoring = true
        root.resetLensModels()
        root.restoring = false
        root.submitLens(true)
    }

    function restoreDefaults() {
        root.restoring = true
        root.resetCropModels()
        root.resetLensModels()
        root.restoring = false
    }

    function loadCropSnapshot(snapshot) {
        const raw = snapshot ? snapshot["crop_rotate"] : undefined
        const entry = raw && raw["crop_rotate"] !== undefined ? raw["crop_rotate"] : raw
        root.restoring = true
        if (!entry) {
            root.resetCropModels()
            root.restoring = false
            return
        }

        const rect = entry["crop_rect"] || {}
        cropXModel.value = Number(rect["x"] !== undefined ? rect["x"] : 0.0)
        cropYModel.value = Number(rect["y"] !== undefined ? rect["y"] : 0.0)
        cropWidthModel.value = Number(rect["w"] !== undefined ? rect["w"] : 1.0)
        cropHeightModel.value = Number(rect["h"] !== undefined ? rect["h"] : 1.0)
        rotationModel.value = Number(entry["angle_degrees"] !== undefined
                                     ? entry["angle_degrees"] : 0.0)

        const aspect = entry["aspect_ratio"] || {}
        aspectWidthModel.value = Number(aspect["width"] !== undefined ? aspect["width"] : 1.0)
        aspectHeightModel.value = Number(aspect["height"] !== undefined ? aspect["height"] : 1.0)
        var preset = entry["aspect_ratio_preset"] !== undefined
                     ? String(entry["aspect_ratio_preset"]) : "free"
        var presetIndex = 0
        for (var i = 0; i < aspectModel.entries.length; ++i) {
            if (String(aspectModel.entries[i].value) === preset) {
                presetIndex = i
                break
            }
        }
        aspectModel.currentIndex = presetIndex
        if (aspectModel.currentValue !== "free" && aspectModel.currentValue !== "custom") {
            const fixedRatio = geometryMath.presetRatio(aspectModel.currentValue)
            if (fixedRatio.length >= 2) {
                aspectWidthModel.value = Number(fixedRatio[0])
                aspectHeightModel.value = Number(fixedRatio[1])
            }
        }
        root.restoring = false
    }

    function loadLensSnapshot(snapshot) {
        const raw = snapshot ? snapshot["lens_calib"] : undefined
        const entry = raw && raw["lens_calib"] !== undefined ? raw["lens_calib"] : raw
        root.restoring = true
        if (!entry) {
            root.resetLensModels()
            root.restoring = false
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
    }

    function loadFromSnapshot(snapshot) {
        if (snapshot === undefined || snapshot === null)
            return
        if (root.inputActive)
            return
        var normalizedSnapshot = snapshot
        try {
            normalizedSnapshot = JSON.parse(JSON.stringify(snapshot))
        } catch (error) {
            normalizedSnapshot = snapshot
        }
        if (!root.aspectEntries.length) {
            root.aspectEntries = root.buildAspectEntries()
            aspectModel.entries = root.aspectEntries
        }
        if (!root.lensBrandEntries.length)
            root.refreshLensBrandEntries()
        root.loadCropSnapshot(normalizedSnapshot)
        root.loadLensSnapshot(normalizedSnapshot)
        root.draftDirty = false
        if (root.panelActive)
            root.syncToInteraction()
    }

    function enterGeometryTool() {
        if (!root.panelActive || !root.canUseGeometry)
            return
        root.draftDirty = false
        root.interaction.setCropToolEnabled(true)
        root.interaction.setCropOverlayVisible(true)
        root.syncToInteraction()
    }

    function leaveGeometryTool() {
        if (!root.interaction)
            return
        root.interaction.setCropToolEnabled(false)
        root.interaction.setCropOverlayVisible(false)
        root.overlayInputActive = false
    }

    function syncFromOverlayRect(rect) {
        const wasSyncing = root.syncingToInteraction
        root.syncingToInteraction = true
        root.setRectModels(rect)
        if (root.interaction)
            rotationModel.value = Number(root.interaction.cropRotationDegrees)
        root.syncingToInteraction = wasSyncing
    }

    function wireEnabled() {
        const enabled = root.controlsEnabled
        cropXModel.enabled = enabled
        cropYModel.enabled = enabled
        cropWidthModel.enabled = enabled
        cropHeightModel.enabled = enabled
        rotationModel.enabled = enabled
        aspectModel.enabled = enabled
        aspectWidthModel.enabled = enabled && aspectModel.currentValue === "custom"
        aspectHeightModel.enabled = enabled && aspectModel.currentValue === "custom"
        lensEnabledModel.enabled = enabled
        lensBrandModel.enabled = enabled
        lensModelModel.enabled = enabled && lensBrandModel.currentValue.length > 0
                                 && lensModelModel.entries.length > 0
    }

    onControlsEnabledChanged: {
        root.wireEnabled()
        if (root.controlsEnabled)
            root.enterGeometryTool()
        else
            root.leaveGeometryTool()
    }
    onPanelActiveChanged: {
        if (root.panelActive) {
            root.enterGeometryTool()
        } else {
            // Fallback if panel is deactivated without selectPanel/Enter (direct
            // property write). selectPanel commits first so this is usually a no-op.
            root.confirmPendingCrop()
            root.leaveGeometryTool()
        }
    }
    onInteractionChanged: root.enterGeometryTool()
    onEditorSessionChanged: {
        root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }

    // Crop/rotate models intentionally have no submitter: while Geometry is
    // active they only drive the draft overlay. confirmPendingCrop() owns the
    // single settled pipeline submit on leave or Enter.
    EditorAdjustmentValueModel {
        id: cropXModel
        objectName: "geometryCropXModel"
        fieldKey: "crop_rotate"
        label: qsTr("Crop X")
        minimum: 0
        maximum: 1
        defaultValue: 0
        step: 0.001
        precision: 3
    }
    EditorAdjustmentValueModel {
        id: cropYModel
        objectName: "geometryCropYModel"
        fieldKey: "crop_rotate"
        label: qsTr("Crop Y")
        minimum: 0
        maximum: 1
        defaultValue: 0
        step: 0.001
        precision: 3
    }
    EditorAdjustmentValueModel {
        id: cropWidthModel
        objectName: "geometryCropWidthModel"
        fieldKey: "crop_rotate"
        label: qsTr("Crop Width")
        minimum: 0.0001
        maximum: 1
        defaultValue: 1
        step: 0.001
        precision: 3
    }
    EditorAdjustmentValueModel {
        id: cropHeightModel
        objectName: "geometryCropHeightModel"
        fieldKey: "crop_rotate"
        label: qsTr("Crop Height")
        minimum: 0.0001
        maximum: 1
        defaultValue: 1
        step: 0.001
        precision: 3
    }
    EditorAdjustmentValueModel {
        id: rotationModel
        objectName: "geometryRotationModel"
        fieldKey: "crop_rotate"
        label: qsTr("Rotation")
        minimum: -180
        maximum: 180
        defaultValue: 0
        step: 0.1
        precision: 1
        suffix: "°"
    }
    EditorAdjustmentEnumModel {
        id: aspectModel
        objectName: "geometryAspectModel"
        fieldKey: "crop_rotate"
        label: qsTr("Aspect Ratio")
        entries: root.aspectEntries
        defaultIndex: 0
    }
    EditorAdjustmentValueModel {
        id: aspectWidthModel
        objectName: "geometryAspectWidthModel"
        fieldKey: "crop_rotate"
        label: qsTr("Aspect Width")
        minimum: 0.0001
        maximum: 100
        defaultValue: 1
        step: 0.01
        precision: 2
    }
    EditorAdjustmentValueModel {
        id: aspectHeightModel
        objectName: "geometryAspectHeightModel"
        fieldKey: "crop_rotate"
        label: qsTr("Aspect Height")
        minimum: 0.0001
        maximum: 100
        defaultValue: 1
        step: 0.01
        precision: 2
    }
    EditorAdjustmentToggleModel {
        id: lensEnabledModel
        objectName: "geometryLensEnabledModel"
        fieldKey: "lens_calib"
        label: qsTr("Enable Lens Calibration")
        defaultValue: false
        value: false
        submitter: root.editorSession
        paramsBuilder: function (value) { return root.buildLensParams() }
    }
    EditorAdjustmentEnumModel {
        id: lensBrandModel
        objectName: "geometryLensBrandModel"
        fieldKey: "lens_calib"
        label: qsTr("Lens Brand")
        entries: root.lensBrandEntries
        submitter: root.editorSession
        paramsBuilder: function (value) { return root.buildLensParams() }
    }
    EditorAdjustmentEnumModel {
        id: lensModelModel
        objectName: "geometryLensModelModel"
        fieldKey: "lens_calib"
        label: qsTr("Lens Model")
        entries: root.lensModelEntries
        submitter: root.editorSession
        paramsBuilder: function (value) { return root.buildLensParams() }
    }

    Connections {
        target: cropXModel
        function onValueChanged() {
            if (!root.restoring && !root.syncingToInteraction) {
                root.markDraftDirty()
                root.syncToInteraction()
            }
        }
    }
    Connections {
        target: cropYModel
        function onValueChanged() {
            if (!root.restoring && !root.syncingToInteraction) {
                root.markDraftDirty()
                root.syncToInteraction()
            }
        }
    }
    Connections {
        target: cropWidthModel
        function onValueChanged() {
            if (!root.restoring && !root.syncingToInteraction) {
                root.markDraftDirty()
                root.resizeLockedRect(true)
            }
        }
    }
    Connections {
        target: cropHeightModel
        function onValueChanged() {
            if (!root.restoring && !root.syncingToInteraction) {
                root.markDraftDirty()
                root.resizeLockedRect(false)
            }
        }
    }
    Connections {
        target: rotationModel
        function onValueChanged() {
            if (!root.restoring && !root.syncingToInteraction) {
                root.markDraftDirty()
                root.syncToInteraction()
            }
        }
    }
    Connections {
        target: aspectModel
        function onCurrentIndexChanged() {
            root.wireEnabled()
            if (!root.restoring && !root.syncingToInteraction) {
                root.markDraftDirty()
                root.applyAspectSelection()
            }
        }
    }
    Connections {
        target: aspectWidthModel
        function onValueChanged() {
            if (!root.restoring && !root.syncingToInteraction) {
                root.markDraftDirty()
                root.setEnumValue(aspectModel, "custom", 1)
                root.resizeLockedRect(true)
            }
        }
    }
    Connections {
        target: aspectHeightModel
        function onValueChanged() {
            if (!root.restoring && !root.syncingToInteraction) {
                root.markDraftDirty()
                root.setEnumValue(aspectModel, "custom", 1)
                root.resizeLockedRect(false)
            }
        }
    }
    Connections {
        target: lensEnabledModel
        function onValueChanged() {
            root.wireEnabled()
        }
    }
    Connections {
        target: lensBrandModel
        function onCurrentIndexChanged() {
            const requested = root.restoring ? lensModelModel.currentValue : ""
            root.refreshLensModelEntries(lensBrandModel.currentValue, requested)
            root.wireEnabled()
        }
    }

    Connections {
        target: root.interaction
        function onCropRectCommitted(rect, isFinal) {
            if (!root.panelActive || root.syncingToInteraction || root.restoring)
                return
            // Overlay drag is pure UI: update draft models + dirty bit only.
            // Pipeline bake waits for confirmPendingCrop (leave / Enter).
            root.overlayInputActive = !isFinal
            root.syncFromOverlayRect(rect)
            root.draftDirty = true
            if (isFinal)
                root.overlayInputActive = false
        }
        function onCropRotationCommitted(degrees, isFinal) {
            if (!root.panelActive || root.syncingToInteraction || root.restoring)
                return
            root.overlayInputActive = !isFinal
            const wasSyncing = root.syncingToInteraction
            root.syncingToInteraction = true
            rotationModel.value = Number(degrees)
            if (root.interaction.cropRectNormalized)
                root.setRectModels(root.interaction.cropRectNormalized)
            root.syncingToInteraction = wasSyncing
            root.draftDirty = true
            if (isFinal)
                root.overlayInputActive = false
        }
    }

    Flickable {
        id: scroller
        objectName: "editorGeometryPanelScroll"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: panelColumn.implicitHeight + appTheme.spaceMd
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
                var maxY = Math.max(0, scroller.contentHeight - scroller.height)
                scroller.contentY = Math.max(0, Math.min(maxY, scroller.contentY - step))
                event.accepted = true
            }
        }

        ColumnLayout {
            id: panelColumn
            width: root.width
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: qsTr("Geometry")
                color: root.colText
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightHeading
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Source aspect %1").arg(root.imageAspect.toFixed(3))
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Press Enter or switch panels to apply. Reset returns to a full-frame, unrotated crop.")
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.WordWrap
            }

            CollapsibleSection {
                id: cropSection
                objectName: "editorAdjustmentGroupShell_geometry_crop"
                Layout.fillWidth: true
                title: qsTr("Crop and Rotate")
                expanded: true
                controlsEnabled: root.controlsEnabled
                surfaceColor: root.colCardSurface
                disabledSurfaceColor: root.colCardSurface
                borderColor: root.colCardBorder
                textColor: root.colText
                mutedColor: root.colMuted
                hoverColor: root.colHover
                accentColor: root.colAccent
                bodyContentHeight: cropControls.implicitHeight + appTheme.spaceSm

                ColumnLayout {
                    id: cropControls
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceXs
                    spacing: appTheme.spaceSm

                    AdjustmentCombo {
                        objectName: "geometryAspectCombo"
                        Layout.fillWidth: true
                        model: aspectModel
                    }
                    AdjustmentSlider {
                        objectName: "geometryAspectWidthSlider"
                        Layout.fillWidth: true
                        visible: aspectModel.currentValue === "custom"
                        model: aspectWidthModel
                        flickable: scroller
                    }
                    AdjustmentSlider {
                        objectName: "geometryAspectHeightSlider"
                        Layout.fillWidth: true
                        visible: aspectModel.currentValue === "custom"
                        model: aspectHeightModel
                        flickable: scroller
                    }
                    AdjustmentSlider {
                        objectName: "geometryCropXSlider"
                        Layout.fillWidth: true
                        model: cropXModel
                        flickable: scroller
                    }
                    AdjustmentSlider {
                        objectName: "geometryCropYSlider"
                        Layout.fillWidth: true
                        model: cropYModel
                        flickable: scroller
                    }
                    AdjustmentSlider {
                        objectName: "geometryCropWidthSlider"
                        Layout.fillWidth: true
                        model: cropWidthModel
                        flickable: scroller
                    }
                    AdjustmentSlider {
                        objectName: "geometryCropHeightSlider"
                        Layout.fillWidth: true
                        model: cropHeightModel
                        flickable: scroller
                    }
                    AdjustmentSlider {
                        objectName: "geometryRotationSlider"
                        Layout.fillWidth: true
                        model: rotationModel
                        flickable: scroller
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceSm
                        Item { Layout.fillWidth: true }
                        IconActionButton {
                            objectName: "geometryResetButton"
                            compact: true
                            enabled: root.canUseGeometry
                            iconSrc: "qrc:/panel_icons/reset.svg"
                            actionName: qsTr("Reset crop and rotation")
                            onClicked: root.resetGeometry()
                        }
                    }
                }
            }

            CollapsibleSection {
                id: lensSection
                objectName: "editorAdjustmentGroupShell_geometry_lens"
                Layout.fillWidth: true
                title: qsTr("Lens Calibration")
                expanded: false
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
                        objectName: "geometryLensEnabledToggle"
                        Layout.fillWidth: true
                        model: lensEnabledModel
                    }
                    AdjustmentCombo {
                        objectName: "geometryLensBrandCombo"
                        Layout.fillWidth: true
                        model: lensBrandModel
                    }
                    AdjustmentCombo {
                        objectName: "geometryLensModelCombo"
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
                            objectName: "geometryLensResetButton"
                            compact: true
                            enabled: root.controlsEnabled
                            iconSrc: "qrc:/panel_icons/reset.svg"
                            actionName: qsTr("Reset lens calibration")
                            onClicked: root.resetLens()
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
    }

    Component.onCompleted: {
        root.aspectEntries = root.buildAspectEntries()
        aspectModel.entries = root.aspectEntries
        root.refreshLensBrandEntries()
        root.refreshLensModelEntries("", "")
        root.restoreDefaults()
        root.wireEnabled()
        root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
        root.enterGeometryTool()
    }
}
