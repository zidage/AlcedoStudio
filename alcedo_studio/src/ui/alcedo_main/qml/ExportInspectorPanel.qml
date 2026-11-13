pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Dialogs
import QtQuick.Layouts
import Alcedo.Main 1.0

// Inspector Export page: export presets, destination, SDR/HDR options, scrollable queue, and
// themed progress — replaces the former AlbumExportDialog modal.
Item {
    id: root
    objectName: "exportInspectorPanel"

    property var selectionState: null
    property int selectedCount: 0
    property bool pageActive: false

    // The queue, defaults, limits, and checks are C++ owners shared with automation.
    readonly property var exportQueue: appModules.exportQueue
    readonly property var exportRecipe: appModules.exportRecipe
    readonly property var recipeDefaults: exportRecipe.DefaultOptions()
    readonly property int exportQueueCount: exportQueue.count
    readonly property var exportPreviewRows: exportQueue.previewRows
    readonly property bool exportBusy: appModules.importExport.exportInFlight
    readonly property bool controlsEnabled: !exportBusy

    property string sdrFormat: recipeDefaults.format
    property int sdrBitDepth: recipeDefaults.bit_depth
    property string tiffCompression: recipeDefaults.tiff_compression
    property bool subfolderEnabled: recipeDefaults.subfolder_enabled
    property bool ultraHdrDitherEnabled: recipeDefaults.ultra_hdr_dither
    property bool includeMetadata: recipeDefaults.include_metadata
    property bool embedIccProfile: recipeDefaults.embed_icc_profile
    property bool includeGps: recipeDefaults.include_gps
    property string ultraHdrMaxSideText: String(recipeDefaults.ultra_hdr_long_edge)
    property string outDirText: ""
    property string subfolderText: qsTr("Processed")
    property string sdrResizeMode: recipeDefaults.resize_mode
    property string sdrMaxSideText: String(recipeDefaults.long_edge)
    property string sdrWidthText: String(recipeDefaults.width)
    property string sdrHeightText: String(recipeDefaults.height)
    property string physicalWidthText: String(recipeDefaults.print_width)
    property string physicalHeightText: String(recipeDefaults.print_height)
    property string physicalUnit: recipeDefaults.print_unit
    property string dpiText: String(recipeDefaults.dpi)

    readonly property int queuePreviewLimit: 36
    readonly property int queueListMaxHeight: 180
    readonly property bool settingsValid: exportRecipe.ValidateOptions(root.recipeOptions()).valid
    readonly property string namingSampleSource: {
        const rows = root.exportPreviewRows || []
        return rows.length > 0 && rows[0].label ? String(rows[0].label) : "DSCF2074"
    }
    readonly property bool namingSampleIsHdr: {
        const rows = root.exportPreviewRows || []
        return rows.length > 0 && rows[0].isHdr === true
    }
    readonly property string outputExtension: {
        if (root.namingSampleIsHdr)
            return ".jpg"
        switch (root.sdrFormat) {
        case "PNG": return ".png"
        case "TIFF": return ".tif"
        case "EXR": return ".exr"
        default: return ".jpg"
        }
    }

    readonly property var formatEntries: [
        { label: qsTr("JPEG"), value: "JPEG" },
        { label: qsTr("PNG"), value: "PNG" },
        { label: qsTr("TIFF"), value: "TIFF" },
        { label: qsTr("EXR"), value: "EXR" }
    ]

    readonly property var bitDepthEntries: {
        const allowed = root.exportRecipe.AllowedBitDepthsFor(root.sdrFormat)
        const allow8 = allowed.indexOf(8) >= 0
        const allow16 = allowed.indexOf(16) >= 0
        const allow32 = allowed.indexOf(32) >= 0
        return [
            { label: qsTr("8 Bit"), value: "8", enabled: allow8 },
            { label: qsTr("16 Bit"), value: "16", enabled: allow16 },
            { label: qsTr("32 Bit"), value: "32", enabled: allow32 }
        ]
    }

    readonly property var tiffCompressionEntries: [
        { label: qsTr("None"), value: "NONE" },
        { label: qsTr("LZW"), value: "LZW" },
        { label: qsTr("ZIP"), value: "ZIP" }
    ]

    readonly property var resizeModeEntries: [
        { label: qsTr("Original"), value: "original" },
        { label: qsTr("Long Edge"), value: "long_edge" },
        { label: qsTr("Bounds"), value: "bounds" },
        { label: qsTr("Print"), value: "print" }
    ]

    readonly property var physicalUnitEntries: [
        { label: qsTr("mm"), value: "mm" },
        { label: qsTr("cm"), value: "cm" },
        { label: qsTr("in"), value: "in" }
    ]

    readonly property var visibleExportRows: {
        const source = root.exportPreviewRows ? root.exportPreviewRows : []
        const limit = Math.max(1, Number(root.queuePreviewLimit))
        const visibleCount = Math.min(source.length, limit)
        const rows = []
        for (let i = 0; i < visibleCount; ++i)
            rows.push(source[i])
        if (source.length > visibleCount) {
            rows.push({
                summaryRow: true,
                label: qsTr("...(%1 more)").arg(source.length - visibleCount)
            })
        }
        return rows
    }

    readonly property real exportProgressPercent: {
        const total = appModules.importExport.exportTotal
        if (total <= 0)
            return 0
        return (appModules.importExport.exportCompleted / total) * 100.0
    }

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    function isBitDepthAllowed(formatValue, depth) {
        return root.exportRecipe.AllowedBitDepthsFor(formatValue).indexOf(depth) >= 0
    }

    function ensureValidBitDepthSelection() {
        if (root.isBitDepthAllowed(root.sdrFormat, root.sdrBitDepth))
            return
        root.sdrBitDepth = root.exportRecipe.PreferredBitDepthFor(root.sdrFormat)
    }

    // The panel values in the option form of ExportRecipeBuilder. The size fields stay text;
    // ExportRecipeBuilder reads them and rejects text that is not a valid number.
    function recipeOptions() {
        return {
            format: root.sdrFormat,
            bit_depth: root.sdrBitDepth,
            quality: Math.round(sdrQualityModel.value),
            png_level: Math.round(pngLevelModel.value),
            tiff_compression: root.tiffCompression,
            include_metadata: root.includeMetadata,
            embed_icc_profile: root.embedIccProfile,
            include_gps: root.includeGps,
            ultra_hdr_quality: Math.round(ultraHdrQualityModel.value),
            ultra_hdr_dither: root.ultraHdrDitherEnabled,
            ultra_hdr_long_edge: root.ultraHdrMaxSideText,
            out_dir: root.outDirText,
            subfolder_enabled: root.subfolderEnabled,
            subfolder: root.subfolderText,
            resize_mode: root.sdrResizeMode,
            long_edge: root.sdrMaxSideText,
            width: root.sdrWidthText,
            height: root.sdrHeightText,
            print_width: root.physicalWidthText,
            print_height: root.physicalHeightText,
            print_unit: root.physicalUnit,
            dpi: root.dpiText,
            file_name_pattern: namingEditor.pattern
        }
    }

    function exportStatusForRow(statusKey, summaryRow) {
        if (summaryRow === true)
            return ""
        if (!statusKey)
            return root.exportBusy ? "running" : "queued"
        const map = appModules.importExport.exportItemStatuses
        const value = map ? map[statusKey] : ""
        if (!value || value.length === 0)
            return "queued"
        return String(value)
    }

    function exportStatusText(status) {
        switch (status) {
        case "running":
            return qsTr("Exporting")
        case "succeeded":
            return qsTr("Done")
        case "failed":
            return qsTr("Failed")
        default:
            return qsTr("Queued")
        }
    }

    function exportStatusColor(status) {
        switch (status) {
        case "running":
            return appTheme.backgroundTaskWorkingColor
        case "succeeded":
            return appTheme.backgroundTaskFinishedColor
        case "failed":
            return appTheme.backgroundTaskFailedColor
        default:
            return appTheme.textMutedColor
        }
    }

    function preparePage() {
        if (outDirText.length === 0)
            outDirText = appModules.importExport.LoadExportFolder()
        ensureValidBitDepthSelection()
        appModules.importExport.ResetExportState()
        sdrQualityModel.enabled = root.controlsEnabled && root.sdrFormat === "JPEG"
        ultraHdrQualityModel.enabled = root.controlsEnabled
        pngLevelModel.enabled = root.controlsEnabled && root.sdrFormat === "PNG"
    }

    function setPercentValue(model, value) {
        // Set the range first: EditorAdjustmentValueModel clamps to its maximum, which defaults
        // to 1.0.
        model.minimum = 1
        model.maximum = 100
        model.value = Number(value)
    }

    // The print size fields are read in the default locale (ExportRecipeBuilder::FromVariantMap).
    function localeNumberText(value) {
        const number = Number(value)
        return Number.isInteger(number) ? String(number)
                                        : number.toLocaleString(Qt.locale(), "f", 2)
    }

    // Sets the panel values from options in the option form of ExportRecipeBuilder, for example
    // the options of an export preset or of the last export.
    function applyOptions(options) {
        if (!options)
            return
        root.sdrFormat = String(options.format)
        root.sdrBitDepth = Number(options.bit_depth)
        root.tiffCompression = String(options.tiff_compression)
        root.includeMetadata = options.include_metadata === true
        root.embedIccProfile = options.embed_icc_profile === true
        root.includeGps = options.include_gps === true
        root.ultraHdrDitherEnabled = options.ultra_hdr_dither === true
        root.ultraHdrMaxSideText = String(options.ultra_hdr_long_edge)
        root.sdrResizeMode = String(options.resize_mode)
        root.sdrMaxSideText = String(options.long_edge)
        root.sdrWidthText = String(options.width)
        root.sdrHeightText = String(options.height)
        root.physicalWidthText = root.localeNumberText(options.print_width)
        root.physicalHeightText = root.localeNumberText(options.print_height)
        root.physicalUnit = String(options.print_unit)
        root.dpiText = String(options.dpi)
        root.setPercentValue(sdrQualityModel, options.quality)
        root.setPercentValue(ultraHdrQualityModel, options.ultra_hdr_quality)
        pngLevelModel.value = Number(options.png_level)
        namingEditor.pattern = String(options.file_name_pattern)
        root.ensureValidBitDepthSelection()
    }

    function persistLastUsedOptions() {
        exportRecipe.SaveLastUsedOptionsMap(root.recipeOptions())
    }

    function addSelectedToQueue() {
        if (!selectionState)
            return
        exportQueue.AddTargets(selectionState.currentSelectedItems())
        selectionState.clearSelectedImages()
    }

    function clearQueue() {
        exportQueue.Clear()
    }

    function startExport() {
        if (root.exportQueueCount <= 0 || root.exportBusy || !root.settingsValid)
            return
        persistLastUsedOptions()
        appModules.importExport.SaveExportFolder(outDirText)
        exportRecipe.StartExport(root.recipeOptions(), exportQueue.Targets())
    }

    onSdrFormatChanged: {
        ensureValidBitDepthSelection()
        sdrQualityModel.enabled = root.controlsEnabled && root.sdrFormat === "JPEG"
        pngLevelModel.enabled = root.controlsEnabled && root.sdrFormat === "PNG"
    }

    onControlsEnabledChanged: {
        sdrQualityModel.enabled = root.controlsEnabled && root.sdrFormat === "JPEG"
        ultraHdrQualityModel.enabled = root.controlsEnabled
        pngLevelModel.enabled = root.controlsEnabled && root.sdrFormat === "PNG"
    }

    onPageActiveChanged: {
        if (pageActive)
            preparePage()
        else
            persistLastUsedOptions()
    }

    Component.onCompleted: applyOptions(exportRecipe.LastUsedOptionsMap())

    EditorAdjustmentValueModel {
        id: sdrQualityModel
        objectName: "exportSdrQualityModel"
        fieldKey: "export_sdr_quality"
        label: qsTr("Quality")
        minimum: 1
        maximum: 100
        defaultValue: root.recipeDefaults.quality
        step: 1
        precision: 0
        suffix: "%"
        enabled: root.controlsEnabled && root.sdrFormat === "JPEG"
    }

    EditorAdjustmentValueModel {
        id: ultraHdrQualityModel
        objectName: "exportUltraHdrQualityModel"
        fieldKey: "export_uhdr_quality"
        label: qsTr("HDR Quality")
        minimum: 1
        maximum: 100
        defaultValue: root.recipeDefaults.ultra_hdr_quality
        step: 1
        precision: 0
        suffix: "%"
        enabled: root.controlsEnabled
    }

    EditorAdjustmentValueModel {
        id: pngLevelModel
        objectName: "exportPngLevelModel"
        fieldKey: "export_png_level"
        label: qsTr("PNG Level")
        minimum: 0
        maximum: 9
        defaultValue: root.recipeDefaults.png_level
        value: root.recipeDefaults.png_level
        step: 1
        precision: 0
        enabled: root.controlsEnabled && root.sdrFormat === "PNG"
    }

    FolderDialog {
        id: exportFolderDialog
        title: qsTr("Select Export Folder")
        onAccepted: {
            const local = appModules.importExport.SaveExportFolder(selectedFolder.toString())
            root.outDirText = local.length > 0 ? local : selectedFolder.toString()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Flickable {
            id: settingsScroll
            objectName: "exportInspectorScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: settingsColumn.implicitHeight + appTheme.spaceMd
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick

            property int inputLockCount: 0
            function beginInputLock() {
                if (inputLockCount === 0)
                    interactive = false
                inputLockCount += 1
            }
            function endInputLock() {
                inputLockCount = Math.max(0, inputLockCount - 1)
                if (inputLockCount === 0)
                    interactive = true
            }

            ColumnLayout {
                id: settingsColumn
                width: settingsScroll.width
                spacing: appTheme.spaceMd

                // ── Preset ───────────────────────────────────────────
                CollapsibleSection {
                    id: presetSection
                    Layout.fillWidth: true
                    Layout.leftMargin: appTheme.spaceSm
                    Layout.rightMargin: appTheme.spaceSm
                    Layout.topMargin: appTheme.spaceSm
                    title: qsTr("Preset")
                    expanded: true
                    controlsEnabled: root.controlsEnabled
                    bodyContentHeight: presetPicker.implicitHeight + appTheme.spaceSm

                    ExportPresetPicker {
                        id: presetPicker
                        objectName: "exportPresetPicker"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        recipe: root.exportRecipe
                        currentOptions: root.recipeOptions()
                        optionsValid: root.settingsValid
                        controlsEnabled: root.controlsEnabled
                        onPresetSelected: function(options) { root.applyOptions(options) }
                    }
                }

                // ── Destination ──────────────────────────────────────
                CollapsibleSection {
                    id: destinationSection
                    Layout.fillWidth: true
                    Layout.leftMargin: appTheme.spaceSm
                    Layout.rightMargin: appTheme.spaceSm
                    title: qsTr("Destination")
                    expanded: true
                    controlsEnabled: root.controlsEnabled
                    bodyContentHeight: destinationBody.implicitHeight + appTheme.spaceSm

                    ColumnLayout {
                        id: destinationBody
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        spacing: appTheme.spaceSm

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceSm

                            TextField {
                                id: exportOutDirField
                                Layout.fillWidth: true
                                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                placeholderText: qsTr("Select output directory...")
                                font.family: appTheme.dataFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                color: appTheme.textColor
                                enabled: root.controlsEnabled
                                selectByMouse: true
                                Component.onCompleted: text = root.outDirText
                                onTextEdited: root.outDirText = text
                                background: Rectangle {
                                    radius: appTheme.controlRadiusSmall
                                    color: appTheme.bgBaseColor
                                    border.width: 1
                                    border.color: appTheme.cardBorderColor
                                }

                                Connections {
                                    target: root
                                    function onOutDirTextChanged() {
                                        if (exportOutDirField.text !== root.outDirText)
                                            exportOutDirField.text = root.outDirText
                                    }
                                }
                            }

                            Button {
                                text: qsTr("Browse")
                                enabled: root.controlsEnabled
                                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                Layout.preferredWidth: Math.max(implicitWidth, 72)
                                hoverEnabled: true
                                activeFocusOnTab: true
                                Accessible.name: text
                                onClicked: {
                                    exportFolderDialog.currentFolder =
                                            appModules.importExport.ExportFolderUrl(root.outDirText)
                                    exportFolderDialog.open()
                                }
                                contentItem: Label {
                                    text: parent.text
                                    color: parent.enabled ? appTheme.textColor : appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    radius: appTheme.controlRadiusSmall
                                    color: parent.down ? appTheme.buttonPressedFillColor
                                           : (parent.hovered ? appTheme.buttonHoveredFillColor
                                                             : appTheme.bgBaseColor)
                                    border.width: 1
                                    border.color: appTheme.cardBorderColor
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceSm

                            ThemeCheckBox {
                                objectName: "exportSubfolderCheck"
                                Layout.fillWidth: true
                                text: qsTr("Put in Subfolder")
                                checked: root.subfolderEnabled
                                enabled: root.controlsEnabled
                                alwaysPrimaryText: true
                                onToggled: function(next) { root.subfolderEnabled = next }
                            }

                            TextField {
                                Layout.fillWidth: true
                                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                visible: root.subfolderEnabled
                                text: root.subfolderText
                                enabled: root.controlsEnabled
                                font.pixelSize: appTheme.fontSizeCaption
                                color: appTheme.textColor
                                selectByMouse: true
                                onTextChanged: root.subfolderText = text
                                background: Rectangle {
                                    radius: appTheme.controlRadiusSmall
                                    color: appTheme.bgBaseColor
                                    border.width: 1
                                    border.color: appTheme.cardBorderColor
                                }
                            }
                        }
                    }
                }

                // ── File Naming ──────────────────────────────────────
                CollapsibleSection {
                    id: namingSection
                    Layout.fillWidth: true
                    Layout.leftMargin: appTheme.spaceSm
                    Layout.rightMargin: appTheme.spaceSm
                    title: qsTr("File Naming")
                    expanded: true
                    controlsEnabled: root.controlsEnabled
                    bodyContentHeight: namingEditor.implicitHeight + appTheme.spaceSm

                    ExportNamingEditor {
                        id: namingEditor
                        objectName: "exportNamingEditor"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        controlsEnabled: root.controlsEnabled
                        sampleSourceName: root.namingSampleSource
                        outputExtension: root.outputExtension
                        patternRules: root.exportRecipe
                    }
                }

                // ── Metadata and Color ───────────────────────────────
                CollapsibleSection {
                    id: metadataSection
                    Layout.fillWidth: true
                    Layout.leftMargin: appTheme.spaceSm
                    Layout.rightMargin: appTheme.spaceSm
                    title: qsTr("Metadata & Color")
                    expanded: true
                    controlsEnabled: root.controlsEnabled
                    bodyContentHeight: metadataBody.implicitHeight + appTheme.spaceSm

                    ColumnLayout {
                        id: metadataBody
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        spacing: appTheme.spaceSm

                        ThemeCheckBox {
                            objectName: "exportMetadataCheck"
                            Layout.fillWidth: true
                            text: qsTr("EXIF metadata")
                            checked: root.includeMetadata
                            enabled: root.controlsEnabled
                            alwaysPrimaryText: true
                            onToggled: function(next) { root.includeMetadata = next }
                        }

                        ThemeCheckBox {
                            objectName: "exportIccCheck"
                            Layout.fillWidth: true
                            text: qsTr("ICC profile")
                            checked: root.embedIccProfile
                            enabled: root.controlsEnabled
                            alwaysPrimaryText: true
                            onToggled: function(next) { root.embedIccProfile = next }
                        }

                        ThemeCheckBox {
                            objectName: "exportGpsCheck"
                            Layout.fillWidth: true
                            text: qsTr("GPS location in EXIF")
                            checked: root.includeGps
                            enabled: root.controlsEnabled && root.includeMetadata
                            alwaysPrimaryText: true
                            onToggled: function(next) { root.includeGps = next }
                        }
                    }
                }

                // ── SDR Export ───────────────────────────────────────
                CollapsibleSection {
                    id: sdrSection
                    Layout.fillWidth: true
                    Layout.leftMargin: appTheme.spaceSm
                    Layout.rightMargin: appTheme.spaceSm
                    title: qsTr("SDR Export")
                    expanded: true
                    controlsEnabled: root.controlsEnabled
                    bodyContentHeight: sdrBody.implicitHeight + appTheme.spaceSm

                    ColumnLayout {
                        id: sdrBody
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        spacing: appTheme.spaceMd

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceXs

                            Label {
                                text: qsTr("Format")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            SegmentedCardSwitcher {
                                objectName: "exportFormatSwitcher"
                                Layout.fillWidth: true
                                entries: root.formatEntries
                                currentValue: root.sdrFormat
                                enabled: root.controlsEnabled
                                onSelected: function(index, value) {
                                    if (value && value.length > 0)
                                        root.sdrFormat = value
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceXs

                            Label {
                                text: qsTr("Bit Depth")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            SegmentedCardSwitcher {
                                objectName: "exportBitDepthSwitcher"
                                Layout.fillWidth: true
                                entries: root.bitDepthEntries
                                currentValue: String(root.sdrBitDepth)
                                enabled: root.controlsEnabled
                                onSelected: function(index, value) {
                                    const depth = parseInt(value)
                                    if (!isNaN(depth) && root.isBitDepthAllowed(root.sdrFormat, depth))
                                        root.sdrBitDepth = depth
                                }
                            }
                        }

                        AdjustmentSlider {
                            objectName: "exportSdrQualitySlider"
                            Layout.fillWidth: true
                            visible: root.sdrFormat === "JPEG"
                            model: sdrQualityModel
                            flickable: settingsScroll
                        }

                        AdjustmentSlider {
                            objectName: "exportPngLevelSlider"
                            Layout.fillWidth: true
                            visible: root.sdrFormat === "PNG"
                            model: pngLevelModel
                            flickable: settingsScroll
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceXs
                            visible: root.sdrFormat === "TIFF"

                            Label {
                                text: qsTr("Compression")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            SegmentedCardSwitcher {
                                objectName: "exportTiffCompressionSwitcher"
                                Layout.fillWidth: true
                                entries: root.tiffCompressionEntries
                                currentValue: root.tiffCompression
                                enabled: root.controlsEnabled
                                onSelected: function(index, value) {
                                    if (value && value.length > 0)
                                        root.tiffCompression = value
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceXs

                            Label {
                                text: qsTr("Size")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            SegmentedCardSwitcher {
                                objectName: "exportResizeModeSwitcher"
                                Layout.fillWidth: true
                                entries: root.resizeModeEntries
                                currentValue: root.sdrResizeMode
                                enabled: root.controlsEnabled
                                onSelected: function(index, value) {
                                    if (value && value.length > 0)
                                        root.sdrResizeMode = value
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceXs
                            visible: root.sdrResizeMode === "long_edge"

                            Label {
                                text: qsTr("Longest edge (px)")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            TextField {
                                Layout.fillWidth: true
                                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                text: root.sdrMaxSideText
                                validator: IntValidator { bottom: 256; top: 16384 }
                                inputMethodHints: Qt.ImhDigitsOnly
                                font.family: appTheme.dataFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                color: appTheme.textColor
                                enabled: root.controlsEnabled
                                selectByMouse: true
                                onTextChanged: root.sdrMaxSideText = text
                                background: Rectangle {
                                    radius: appTheme.controlRadiusSmall
                                    color: appTheme.bgBaseColor
                                    border.width: 1
                                    border.color: appTheme.cardBorderColor
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceSm
                            visible: root.sdrResizeMode === "bounds"

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: appTheme.spaceXs

                                Label {
                                    text: qsTr("Width (px)")
                                    color: appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }

                                TextField {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                    text: root.sdrWidthText
                                    validator: IntValidator { bottom: 1; top: 100000 }
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    font.family: appTheme.dataFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    color: appTheme.textColor
                                    enabled: root.controlsEnabled
                                    selectByMouse: true
                                    onTextChanged: root.sdrWidthText = text
                                    background: Rectangle {
                                        radius: appTheme.controlRadiusSmall
                                        color: appTheme.bgBaseColor
                                        border.width: 1
                                        border.color: appTheme.cardBorderColor
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: appTheme.spaceXs

                                Label {
                                    text: qsTr("Height (px)")
                                    color: appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }

                                TextField {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                    text: root.sdrHeightText
                                    validator: IntValidator { bottom: 1; top: 100000 }
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    font.family: appTheme.dataFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    color: appTheme.textColor
                                    enabled: root.controlsEnabled
                                    selectByMouse: true
                                    onTextChanged: root.sdrHeightText = text
                                    background: Rectangle {
                                        radius: appTheme.controlRadiusSmall
                                        color: appTheme.bgBaseColor
                                        border.width: 1
                                        border.color: appTheme.cardBorderColor
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceSm
                            visible: root.sdrResizeMode === "print"

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: appTheme.spaceSm

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: appTheme.spaceXs

                                    Label {
                                        text: qsTr("Width")
                                        color: appTheme.textMutedColor
                                        font.family: appTheme.uiFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                        font.weight: appTheme.fontWeightStrong
                                    }

                                    TextField {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                        text: root.physicalWidthText
                                        validator: DoubleValidator { bottom: 0.01; top: 100000; decimals: 2 }
                                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                                        font.family: appTheme.dataFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                        color: appTheme.textColor
                                        enabled: root.controlsEnabled
                                        selectByMouse: true
                                        onTextChanged: root.physicalWidthText = text
                                        background: Rectangle {
                                            radius: appTheme.controlRadiusSmall
                                            color: appTheme.bgBaseColor
                                            border.width: 1
                                            border.color: appTheme.cardBorderColor
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: appTheme.spaceXs

                                    Label {
                                        text: qsTr("Height")
                                        color: appTheme.textMutedColor
                                        font.family: appTheme.uiFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                        font.weight: appTheme.fontWeightStrong
                                    }

                                    TextField {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                        text: root.physicalHeightText
                                        validator: DoubleValidator { bottom: 0.01; top: 100000; decimals: 2 }
                                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                                        font.family: appTheme.dataFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                        color: appTheme.textColor
                                        enabled: root.controlsEnabled
                                        selectByMouse: true
                                        onTextChanged: root.physicalHeightText = text
                                        background: Rectangle {
                                            radius: appTheme.controlRadiusSmall
                                            color: appTheme.bgBaseColor
                                            border.width: 1
                                            border.color: appTheme.cardBorderColor
                                        }
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: appTheme.spaceXs

                                Label {
                                    text: qsTr("Unit")
                                    color: appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }

                                SegmentedCardSwitcher {
                                    objectName: "exportPhysicalUnitSwitcher"
                                    Layout.fillWidth: true
                                    entries: root.physicalUnitEntries
                                    currentValue: root.physicalUnit
                                    enabled: root.controlsEnabled
                                    onSelected: function(index, value) {
                                        if (value && value.length > 0)
                                            root.physicalUnit = value
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: appTheme.spaceXs

                                Label {
                                    text: qsTr("Resolution (DPI)")
                                    color: appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }

                                TextField {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                    text: root.dpiText
                                    validator: IntValidator { bottom: 1; top: 10000 }
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    font.family: appTheme.dataFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    color: appTheme.textColor
                                    enabled: root.controlsEnabled
                                    selectByMouse: true
                                    onTextChanged: root.dpiText = text
                                    background: Rectangle {
                                        radius: appTheme.controlRadiusSmall
                                        color: appTheme.bgBaseColor
                                        border.width: 1
                                        border.color: appTheme.cardBorderColor
                                    }
                                }
                            }
                        }
                    }
                }

                // ── HDR Export ───────────────────────────────────────
                CollapsibleSection {
                    id: hdrSection
                    Layout.fillWidth: true
                    Layout.leftMargin: appTheme.spaceSm
                    Layout.rightMargin: appTheme.spaceSm
                    title: qsTr("HDR Export")
                    expanded: true
                    controlsEnabled: root.controlsEnabled
                    bodyContentHeight: hdrBody.implicitHeight + appTheme.spaceSm

                    ColumnLayout {
                        id: hdrBody
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        spacing: appTheme.spaceMd

                        AdjustmentSlider {
                            objectName: "exportUltraHdrQualitySlider"
                            Layout.fillWidth: true
                            model: ultraHdrQualityModel
                            flickable: settingsScroll
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceXs

                            Label {
                                text: qsTr("Max edge (px)")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            TextField {
                                objectName: "exportUltraHdrMaxEdgeField"
                                Layout.fillWidth: true
                                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                text: root.ultraHdrMaxSideText
                                validator: IntValidator { bottom: 256; top: 8192 }
                                inputMethodHints: Qt.ImhDigitsOnly
                                font.family: appTheme.dataFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                color: appTheme.textColor
                                enabled: root.controlsEnabled
                                selectByMouse: true
                                onTextChanged: root.ultraHdrMaxSideText = text
                                background: Rectangle {
                                    radius: appTheme.controlRadiusSmall
                                    color: appTheme.bgBaseColor
                                    border.width: 1
                                    border.color: appTheme.cardBorderColor
                                }
                            }
                        }

                        ThemeCheckBox {
                            objectName: "exportUltraHdrDitherCheck"
                            Layout.fillWidth: true
                            text: qsTr("Dithering")
                            checked: root.ultraHdrDitherEnabled
                            enabled: root.controlsEnabled
                            alwaysPrimaryText: true
                            onToggled: function(next) { root.ultraHdrDitherEnabled = next }
                        }
                    }
                }

                // ── Export Queue (scrollable) ────────────────────────
                CollapsibleSection {
                    id: queueSection
                    Layout.fillWidth: true
                    Layout.leftMargin: appTheme.spaceSm
                    Layout.rightMargin: appTheme.spaceSm
                    title: root.exportQueueCount > 0
                           ? qsTr("Export Queue (%1)").arg(root.exportQueueCount)
                           : qsTr("Export Queue")
                    expanded: true
                    controlsEnabled: true
                    bodyContentHeight: queueBody.implicitHeight + appTheme.spaceSm

                    ColumnLayout {
                        id: queueBody
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        spacing: appTheme.spaceSm

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceXs

                            Item { Layout.fillWidth: true }

                            IconActionButton {
                                objectName: "exportQueueAddButton"
                                compact: true
                                iconSrc: "qrc:/panel_icons/plus.svg"
                                actionName: qsTr("Add selected to queue")
                                enabled: root.controlsEnabled && root.selectedCount > 0
                                iconColorDefault: appTheme.iconColor
                                iconColorMuted: appTheme.textMutedColor
                                fillIdle: appTheme.bgBaseColor
                                fillHover: appTheme.buttonHoveredFillColor
                                fillPressed: appTheme.buttonPressedFillColor
                                onClicked: root.addSelectedToQueue()
                            }

                            IconActionButton {
                                objectName: "exportQueueClearButton"
                                compact: true
                                iconSrc: "qrc:/panel_icons/trash.svg"
                                actionName: qsTr("Clear export queue")
                                enabled: root.controlsEnabled && root.exportQueueCount > 0
                                iconColorDefault: appTheme.iconColor
                                iconColorMuted: appTheme.textMutedColor
                                fillIdle: appTheme.bgBaseColor
                                fillHover: appTheme.buttonHoveredFillColor
                                fillPressed: appTheme.buttonPressedFillColor
                                onClicked: root.clearQueue()
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: root.queueListMaxHeight
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.bgBaseColor
                            border.width: 1
                            border.color: appTheme.cardBorderColor
                            clip: true

                            ListView {
                                id: queueList
                                objectName: "exportQueueList"
                                anchors.fill: parent
                                anchors.margins: appTheme.spaceXs
                                clip: true
                                spacing: appTheme.spaceXs
                                model: root.visibleExportRows
                                boundsBehavior: Flickable.StopAtBounds
                                ScrollBar.vertical: ScrollBar {
                                    policy: queueList.contentHeight > queueList.height
                                            ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                                    contentItem: Rectangle {
                                        implicitWidth: appTheme.spaceXs + 1
                                        radius: appTheme.badgeRadius
                                        color: root.withAlpha(appTheme.textMutedColor, 0.45)
                                    }
                                    background: Item {}
                                }

                                delegate: RowLayout {
                                    required property var modelData
                                    width: ListView.view ? ListView.view.width : 0
                                    height: Math.max(appTheme.iconButtonHitSize, implicitHeight)
                                    spacing: appTheme.spaceSm

                                    readonly property bool summaryRow: modelData.summaryRow === true
                                    readonly property string itemStatus: root.exportStatusForRow(
                                        modelData.statusKey, summaryRow)

                                    Rectangle {
                                        Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
                                        Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                        Layout.alignment: Qt.AlignVCenter
                                        radius: appTheme.controlRadiusSmall
                                        color: appTheme.cardSurfaceColor
                                        border.width: 1
                                        border.color: appTheme.cardBorderColor

                                        ColorImage {
                                            anchors.centerIn: parent
                                            width: appTheme.iconSourceSizeCompact
                                            height: appTheme.iconSourceSizeCompact
                                            sourceSize.width: appTheme.iconSourceSizeCompact
                                            sourceSize.height: appTheme.iconSourceSizeCompact
                                            fillMode: Image.PreserveAspectFit
                                            source: summaryRow
                                                    ? "qrc:/panel_icons/image.svg"
                                                    : (modelData.isHdr === true
                                                       ? "qrc:/panel_icons/hdr.svg"
                                                       : "qrc:/panel_icons/sdr.svg")
                                            color: appTheme.iconColor
                                            opacity: 0.9
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignVCenter
                                        spacing: 2

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: appTheme.spaceXs

                                            Label {
                                                Layout.fillWidth: true
                                                text: modelData.label
                                                wrapMode: Text.Wrap
                                                color: appTheme.textColor
                                                font.family: appTheme.dataFontFamily
                                                font.pixelSize: appTheme.fontSizeCaption
                                            }

                                        }

                                        RowLayout {
                                            spacing: appTheme.spaceXs
                                            visible: !summaryRow

                                            Rectangle {
                                                Layout.preferredWidth: appTheme.spaceXs + 2
                                                Layout.preferredHeight: appTheme.spaceXs + 2
                                                radius: width / 2
                                                color: root.exportStatusColor(itemStatus)
                                            }

                                            Label {
                                                text: root.exportStatusText(itemStatus)
                                                color: root.exportStatusColor(itemStatus)
                                                font.family: appTheme.uiFontFamily
                                                font.pixelSize: appTheme.fontSizeCaption
                                            }
                                        }
                                    }
                                }
                            }

                            Label {
                                anchors.centerIn: parent
                                visible: root.exportQueueCount === 0
                                text: qsTr("Queue is empty")
                                color: root.withAlpha(appTheme.textMutedColor, 0.72)
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.italic: true
                            }
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: appTheme.spaceSm
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: appTheme.dividerColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceMd
            Layout.rightMargin: appTheme.spaceMd
            Layout.topMargin: appTheme.spaceMd
            Layout.bottomMargin: appTheme.spaceMd
            spacing: appTheme.spaceSm

            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm

                Label {
                    text: qsTr("Export Progress")
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }

                Item { Layout.fillWidth: true }

                Label {
                    text: appModules.importExport.exportCompleted + "/"
                          + (appModules.importExport.exportTotal > 0
                             ? appModules.importExport.exportTotal
                             : root.exportQueueCount)
                    color: appTheme.textMutedColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }

            ThemedProgressBar {
                objectName: "exportProgressBar"
                Layout.fillWidth: true
                active: root.exportBusy
                progressValue: root.exportProgressPercent
                indeterminate: root.exportBusy
                               && appModules.importExport.exportTotal <= 0
                fillColor: root.exportBusy
                           ? appTheme.backgroundTaskWorkingColor
                           : appTheme.backgroundTaskFinishedColor
            }

            Label {
                Layout.fillWidth: true
                visible: appModules.importExport.exportErrorSummary.length > 0
                text: appModules.importExport.exportErrorSummary
                wrapMode: Text.WordWrap
                color: appTheme.dangerColor
                font.family: appTheme.dataFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }
        }
    }
}
