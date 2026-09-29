import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Geometry panel: crop and rotation of the whole image.
//
// The output is the crop frame: an axis-aligned rectangle through which the
// source is seen rotated about the frame center. No frame corner may leave the
// source, so the frame shrinks when a rotation needs it
// (EditorInteractionController::clampCropRect, the same constraint the render
// applies).
//
// Edits follow the path every adjustment uses: slider drags submit interactive
// patches and a settled patch on release; keyboard and field edits submit
// through the model's debounce; overlay drags submit interactive patches while
// moving and one settled patch on release. Each settled patch is one history
// commit. While the panel is open the viewport shows the whole source with the
// document rotation, locked to fit.
Item {
    id: root
    objectName: "editorAdjustmentPanel_geometry"

    property var theme: null
    property var editorSession: null
    property var interaction: null
    property bool controlsEnabled: true
    property bool panelActive: false
    property bool restoring: false
    property bool overlayInputActive: false
    property var aspectEntries: []

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor
    readonly property bool canUseGeometry: root.controlsEnabled && root.interaction !== null
    // Uncropped source size of the presented frame. Zero until a frame arrives.
    readonly property int sourceImageWidth: root.interaction ? root.interaction.sourceImageWidth : 0
    readonly property int sourceImageHeight: root.interaction ? root.interaction.sourceImageHeight : 0
    readonly property bool hasSourceSize: root.sourceImageWidth > 0 && root.sourceImageHeight > 0
    readonly property double imageAspect: root.hasSourceSize
                                          ? root.sourceImageWidth / root.sourceImageHeight : 1.0
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

    function buildAspectEntries() {
        var result = []
        const presets = geometryMath.aspectPresets
        for (var i = 0; i < presets.length; ++i)
            result.push({ value: String(presets[i].value), label: qsTr(String(presets[i].label)) })
        return result
    }

    function indexOfAspect(value) {
        for (var i = 0; i < aspectModel.entries.length; ++i) {
            if (String(aspectModel.entries[i].value) === String(value))
                return i
        }
        return -1
    }

    /// Crop width / height in source pixels for the selected aspect.
    function currentAspectRatio() {
        if (aspectModel.currentValue === "custom")
            return geometryMath.aspectRatio(aspectWidthModel.value, aspectHeightModel.value)
        const ratio = geometryMath.presetRatio(aspectModel.currentValue)
        if (ratio.length < 2)
            return 1.0
        let value = Number(ratio[0]) / Math.max(Number(ratio[1]), 0.0001)
        // Fixed presets describe an unoriented frame shape. Match that shape to
        // the source image so 16:9 becomes 9:16 for a portrait photograph.
        const sourcePortrait = root.imageAspect < 1.0
        const presetPortrait = value < 1.0
        if (Math.abs(value - 1.0) > 0.0001 && sourcePortrait !== presetPortrait)
            value = 1.0 / Math.max(value, 0.0001)
        return value
    }

    function hasLockedAspect() {
        return geometryMath.hasLockedAspect(aspectModel.currentValue,
                                            aspectWidthModel.value,
                                            aspectHeightModel.value)
    }

    function rectFromModels() {
        return Qt.rect(cropXModel.value, cropYModel.value, cropWidthModel.value, cropHeightModel.value)
    }

    function setRectModels(rect) {
        const wasRestoring = root.restoring
        root.restoring = true
        cropXModel.value = Number(rect.x)
        cropYModel.value = Number(rect.y)
        cropWidthModel.value = Number(rect.width)
        cropHeightModel.value = Number(rect.height)
        root.restoring = wasRestoring
    }

    function syncOverlay() {
        if (!root.interaction)
            return
        const locked = root.hasLockedAspect()
        root.interaction.setCropAspectLock(locked, locked ? root.currentAspectRatio() : 1.0)
        root.interaction.setCropRotationDegrees(rotationModel.value)
        root.interaction.setCropRectNormalized(root.rectFromModels())
    }

    /// Apply the aspect lock and the rotated-source constraint after the model
    /// named by @p driver changed, then show the result on the overlay.
    function applyConstraints(driver) {
        let rect = root.rectFromModels()
        if (root.hasLockedAspect()) {
            let fitted = null
            if (driver === "aspect") {
                fitted = geometryMath.maxAspectCropRect(root.imageAspect, root.currentAspectRatio())
            } else if (driver === "width" || driver === "height") {
                fitted = geometryMath.resizeAspectCropRect(rect.x, rect.y, rect.width, rect.height,
                                                           root.imageAspect,
                                                           root.currentAspectRatio(),
                                                           driver === "width")
            }
            if (fitted && fitted.length >= 4)
                rect = Qt.rect(fitted[0], fitted[1], fitted[2], fitted[3])
        }
        if (root.interaction)
            rect = root.interaction.clampCropRect(rect, rotationModel.value)
        root.setRectModels(rect)
        root.syncOverlay()
    }

    function buildCropParams() {
        return JSON.stringify({
            crop_rotate: {
                crop_rect: { x: cropXModel.value, y: cropYModel.value,
                             w: cropWidthModel.value, h: cropHeightModel.value },
                angle_degrees: Number(rotationModel.value),
                aspect_ratio_preset: String(aspectModel.currentValue),
                aspect_ratio: { width: Number(aspectWidthModel.value),
                                height: Number(aspectHeightModel.value) }
            }
        })
    }

    /// Editing the custom width or height switches the preset to custom.
    function selectCustomAspect() {
        const index = root.indexOfAspect("custom")
        if (index < 0 || aspectModel.currentIndex === index)
            return
        aspectModel.currentIndex = index
        root.wireEnabled()
    }

    /// paramsBuilder for the models: constrain, then send the whole geometry.
    function paramsAfter(driver) {
        root.applyConstraints(driver)
        return root.buildCropParams()
    }

    function submitGeometry(settled) {
        if (!root.editorSession || typeof root.editorSession.submitPatch !== "function")
            return false
        return root.editorSession.submitPatch("crop_rotate", root.buildCropParams(), settled)
    }

    /// Enter / numpad Enter in the viewport: leave Geometry for Tone.
    function returnToTone() {
        if (root.editorSession)
            root.editorSession.activeAdjustmentPanel = "tone"
    }

    function resetModelsToDefaults() {
        root.restoring = true
        cropXModel.value = cropXModel.defaultValue
        cropYModel.value = cropYModel.defaultValue
        cropWidthModel.value = cropWidthModel.defaultValue
        cropHeightModel.value = cropHeightModel.defaultValue
        rotationModel.value = rotationModel.defaultValue
        aspectModel.currentIndex = aspectModel.defaultIndex
        aspectWidthModel.value = aspectWidthModel.defaultValue
        aspectHeightModel.value = aspectHeightModel.defaultValue
        root.restoring = false
    }

    /// Reset button: one settled edit back to a full-frame, unrotated, free crop.
    function resetGeometry() {
        root.resetModelsToDefaults()
        root.wireEnabled()
        root.syncOverlay()
        root.submitGeometry(true)
    }

    function loadCropSnapshot(snapshot) {
        const raw = snapshot ? snapshot["crop_rotate"] : undefined
        const entry = raw && raw["crop_rotate"] !== undefined ? raw["crop_rotate"] : raw
        if (!entry) {
            root.resetModelsToDefaults()
            return
        }
        const rect = entry["crop_rect"] || {}
        const aspect = entry["aspect_ratio"] || {}
        root.restoring = true
        cropXModel.value = Number(rect["x"] !== undefined ? rect["x"] : 0.0)
        cropYModel.value = Number(rect["y"] !== undefined ? rect["y"] : 0.0)
        cropWidthModel.value = Number(rect["w"] !== undefined ? rect["w"] : 1.0)
        cropHeightModel.value = Number(rect["h"] !== undefined ? rect["h"] : 1.0)
        rotationModel.value = Number(entry["angle_degrees"] !== undefined ? entry["angle_degrees"] : 0.0)
        aspectWidthModel.value = Number(aspect["width"] !== undefined ? aspect["width"] : 1.0)
        aspectHeightModel.value = Number(aspect["height"] !== undefined ? aspect["height"] : 1.0)
        const presetIndex = root.indexOfAspect(entry["aspect_ratio_preset"] !== undefined
                                               ? entry["aspect_ratio_preset"] : "free")
        aspectModel.currentIndex = presetIndex >= 0 ? presetIndex : aspectModel.defaultIndex
        root.restoring = false
    }

    function loadFromSnapshot(snapshot) {
        if (snapshot === undefined || snapshot === null)
            return
        // An input sequence owns the models until its settled patch echoes back.
        if (root.inputActive)
            return
        if (!root.aspectEntries.length) {
            root.aspectEntries = root.buildAspectEntries()
            aspectModel.entries = root.aspectEntries
        }
        root.loadCropSnapshot(snapshot)
        root.wireEnabled()
        if (root.panelActive)
            root.syncOverlay()
    }

    function enterGeometryTool() {
        if (!root.panelActive || !root.canUseGeometry)
            return
        root.interaction.setCropToolEnabled(true)
        root.interaction.setCropOverlayVisible(true)
        root.syncOverlay()
    }

    function leaveGeometryTool() {
        if (!root.interaction)
            return
        root.interaction.setCropToolEnabled(false)
        root.interaction.setCropOverlayVisible(false)
        root.overlayInputActive = false
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
    }

    onControlsEnabledChanged: {
        root.wireEnabled()
        if (root.controlsEnabled)
            root.enterGeometryTool()
        else
            root.leaveGeometryTool()
    }
    onPanelActiveChanged: {
        if (root.panelActive)
            root.enterGeometryTool()
        else
            root.leaveGeometryTool()
    }
    onInteractionChanged: root.enterGeometryTool()
    onEditorSessionChanged: {
        root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
    }

    EditorAdjustmentValueModel {
        id: cropXModel
        objectName: "geometryCropXModel"
        fieldKey: "crop_rotate"
        label: qsTr("Crop X")
        // With a rotation the unrotated rectangle of the frame may start left of
        // the source; clampCropRect decides the valid range.
        minimum: -1
        maximum: 1
        defaultValue: 0
        step: 0.001
        precision: 3
        submitter: root.editorSession
        paramsBuilder: function () { return root.paramsAfter("position") }
    }
    EditorAdjustmentValueModel {
        id: cropYModel
        objectName: "geometryCropYModel"
        fieldKey: "crop_rotate"
        label: qsTr("Crop Y")
        minimum: -1
        maximum: 1
        defaultValue: 0
        step: 0.001
        precision: 3
        submitter: root.editorSession
        paramsBuilder: function () { return root.paramsAfter("position") }
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
        submitter: root.editorSession
        paramsBuilder: function () { return root.paramsAfter("width") }
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
        submitter: root.editorSession
        paramsBuilder: function () { return root.paramsAfter("height") }
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
        submitter: root.editorSession
        paramsBuilder: function () { return root.paramsAfter("rotation") }
    }
    EditorAdjustmentEnumModel {
        id: aspectModel
        objectName: "geometryAspectModel"
        fieldKey: "crop_rotate"
        label: qsTr("Aspect Ratio")
        entries: root.aspectEntries
        defaultIndex: 0
        submitter: root.editorSession
        paramsBuilder: function () {
            if (aspectModel.currentValue !== "custom") {
                const ratio = geometryMath.presetRatio(aspectModel.currentValue)
                const wasRestoring = root.restoring
                root.restoring = true
                aspectWidthModel.value = ratio.length >= 2 ? Number(ratio[0]) : 1.0
                aspectHeightModel.value = ratio.length >= 2 ? Number(ratio[1]) : 1.0
                root.restoring = wasRestoring
            }
            root.wireEnabled()
            return root.paramsAfter("aspect")
        }
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
        submitter: root.editorSession
        paramsBuilder: function () {
            root.selectCustomAspect()
            return root.paramsAfter("aspect")
        }
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
        submitter: root.editorSession
        paramsBuilder: function () {
            root.selectCustomAspect()
            return root.paramsAfter("aspect")
        }
    }

    Connections {
        target: root.interaction
        ignoreUnknownSignals: true
        // Overlay drag: interactive patches while moving, one settled patch on release.
        function onCropFrameEdited(rect, degrees, isFinal) {
            if (!root.panelActive || root.restoring)
                return
            root.overlayInputActive = !isFinal
            root.setRectModels(rect)
            const wasRestoring = root.restoring
            root.restoring = true
            rotationModel.value = Number(degrees)
            root.restoring = wasRestoring
            root.submitGeometry(isFinal)
        }
        function onImageGeometryChanged() {
            // The source size arrived with a presented frame; redraw the frame
            // and its aspect lock against it.
            if (root.panelActive)
                root.syncOverlay()
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
                objectName: "editorGeometryWholeImageScope"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Geometry applies to the whole image.")
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
            }

            Label {
                objectName: "editorGeometrySourceAspect"
                Layout.fillWidth: true
                text: root.hasSourceSize
                      ? qsTr("Source %1 × %2 (aspect %3)").arg(root.sourceImageWidth)
                                                         .arg(root.sourceImageHeight)
                                                         .arg(root.imageAspect.toFixed(3))
                      : qsTr("Source size is available after the image is shown.")
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
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

            Item { Layout.fillHeight: true }
        }

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
    }

    Component.onCompleted: {
        root.aspectEntries = root.buildAspectEntries()
        aspectModel.entries = root.aspectEntries
        root.resetModelsToDefaults()
        root.wireEnabled()
        root.loadFromSnapshot(root.editorSession ? root.editorSession.adjustmentSnapshot : null)
        root.enterGeometryTool()
    }
}
