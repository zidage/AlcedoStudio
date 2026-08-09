pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Alcedo.Main 1.0

// Inspector Export page: destination, SDR/HDR options, scrollable queue, and
// themed progress — replaces the former AlbumExportDialog modal.
Item {
    id: root
    objectName: "exportInspectorPanel"

    property var exportQueueState: null
    property var selectionState: null
    property int selectedCount: 0
    property bool pageActive: false

    readonly property int exportQueueCount: exportQueueState ? exportQueueState.exportQueueCount : 0
    readonly property var exportPreviewRows: exportQueueState ? exportQueueState.exportPreviewRows : []
    readonly property bool hdrExportAvailable: exportQueueState ? exportQueueState.hasHdrItems() : false
    readonly property bool exportBusy: appModules.importExport.exportInFlight
    readonly property bool controlsEnabled: !exportBusy

    property string sdrFormat: "JPEG"
    property int sdrBitDepth: 8
    property string tiffCompression: "NONE"
    property int pngLevel: 5
    property bool subfolderEnabled: false
    property bool ultraHdrDitherEnabled: true
    property string outDirText: ""
    property string subfolderText: qsTr("Processed")
    property string sdrMaxSideText: ""
    property string ultraHdrMaxSideText: "8192"

    readonly property int queuePreviewLimit: 36
    readonly property int queueListMaxHeight: 180

    readonly property string effectiveOutDir: {
        if (!subfolderEnabled || subfolderText.trim().length === 0)
            return outDirText
        const base = outDirText.replace(/[/\\]+$/, "")
        return base + "/" + subfolderText.trim()
    }

    readonly property var formatEntries: [
        { label: qsTr("JPEG"), value: "JPEG" },
        { label: qsTr("PNG"), value: "PNG" },
        { label: qsTr("TIFF"), value: "TIFF" },
        { label: qsTr("WEBP"), value: "WEBP" },
        { label: qsTr("EXR"), value: "EXR" }
    ]

    readonly property var bitDepthEntries: {
        const format = root.sdrFormat
        const allow8 = format === "JPEG" || format === "WEBP" || format === "PNG" || format === "TIFF"
        const allow16 = format === "PNG" || format === "TIFF" || format === "EXR"
        const allow32 = format === "TIFF" || format === "EXR"
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

    function preferredBitDepthFor(formatValue) {
        switch (formatValue) {
        case "JPEG":
        case "WEBP":
            return 8
        default:
            return 16
        }
    }

    function isBitDepthAllowed(formatValue, depth) {
        switch (formatValue) {
        case "JPEG":
        case "WEBP":
            return depth === 8
        case "PNG":
            return depth === 8 || depth === 16
        case "TIFF":
            return depth === 8 || depth === 16 || depth === 32
        case "EXR":
            return depth === 16 || depth === 32
        default:
            return depth === 8
        }
    }

    function ensureValidBitDepthSelection() {
        if (root.isBitDepthAllowed(root.sdrFormat, root.sdrBitDepth))
            return
        root.sdrBitDepth = root.preferredBitDepthFor(root.sdrFormat)
    }

    function normalizeUltraHdrMaxSide() {
        const raw = ultraHdrMaxSideText.trim()
        if (raw.length === 0) {
            ultraHdrMaxSideText = "8192"
            return
        }
        const v = parseInt(raw)
        if (isNaN(v))
            ultraHdrMaxSideText = "8192"
        else if (v > 8192)
            ultraHdrMaxSideText = "8192"
        else if (v < 256)
            ultraHdrMaxSideText = "256"
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
            outDirText = appModules.importExport.defaultExportFolder
        normalizeUltraHdrMaxSide()
        ensureValidBitDepthSelection()
        applyRememberedQuality()
        if (exportQueueState)
            exportQueueState.refreshExportPreview()
        appModules.importExport.ResetExportState()
        sdrQualityModel.enabled = root.controlsEnabled
                && (root.sdrFormat === "JPEG" || root.sdrFormat === "WEBP")
        ultraHdrQualityModel.enabled = root.controlsEnabled && root.hdrExportAvailable
        pngLevelModel.enabled = root.controlsEnabled && root.sdrFormat === "PNG"
    }

    function applyRememberedQuality() {
        // Set ranges first, then value. EditorAdjustmentValueModel defaults
        // maximum to 1.0, so assigning 95 before maximum:100 clamps to 1%.
        sdrQualityModel.minimum = 1
        sdrQualityModel.maximum = 100
        sdrQualityModel.defaultValue = 95
        sdrQualityModel.value = appModules.importExport.LoadExportSdrQuality()

        ultraHdrQualityModel.minimum = 1
        ultraHdrQualityModel.maximum = 100
        ultraHdrQualityModel.defaultValue = 95
        ultraHdrQualityModel.value = appModules.importExport.LoadExportUltraHdrQuality()
    }

    function persistExportQuality() {
        appModules.importExport.SaveExportSdrQuality(Math.round(sdrQualityModel.value))
        appModules.importExport.SaveExportUltraHdrQuality(Math.round(ultraHdrQualityModel.value))
    }

    function addSelectedToQueue() {
        if (!exportQueueState || !selectionState)
            return
        exportQueueState.addTargets(selectionState.currentSelectedItems())
        selectionState.clearSelectedImages()
    }

    function clearQueue() {
        if (exportQueueState)
            exportQueueState.clearQueue()
    }

    function startExport() {
        if (!exportQueueState || root.exportQueueCount <= 0 || root.exportBusy)
            return
        normalizeUltraHdrMaxSide()
        persistExportQuality()
        const hasSdrResize = sdrMaxSideText.trim().length > 0
        const sdrMaxSide = hasSdrResize ? parseInt(sdrMaxSideText) : 0
        const ultraHdrMaxSide = parseInt(ultraHdrMaxSideText)
        appModules.importExport.StartExportWithSplitOptionsForTargets(
            effectiveOutDir,
            hasSdrResize,
            isNaN(sdrMaxSide) ? 0 : sdrMaxSide,
            isNaN(ultraHdrMaxSide) ? 8192 : ultraHdrMaxSide,
            sdrFormat,
            Math.round(sdrQualityModel.value),
            sdrBitDepth,
            Math.round(pngLevelModel.value),
            tiffCompression,
            Math.round(ultraHdrQualityModel.value),
            ultraHdrDitherEnabled,
            exportQueueState.exportQueueTargets())
    }

    onSdrFormatChanged: {
        ensureValidBitDepthSelection()
        sdrQualityModel.enabled = root.controlsEnabled
                && (root.sdrFormat === "JPEG" || root.sdrFormat === "WEBP")
        pngLevelModel.enabled = root.controlsEnabled && root.sdrFormat === "PNG"
    }

    onHdrExportAvailableChanged: {
        ensureValidBitDepthSelection()
        ultraHdrQualityModel.enabled = root.controlsEnabled && root.hdrExportAvailable
        if (hdrExportAvailable)
            normalizeUltraHdrMaxSide()
    }

    onControlsEnabledChanged: {
        sdrQualityModel.enabled = root.controlsEnabled
                && (root.sdrFormat === "JPEG" || root.sdrFormat === "WEBP")
        ultraHdrQualityModel.enabled = root.controlsEnabled && root.hdrExportAvailable
        pngLevelModel.enabled = root.controlsEnabled && root.sdrFormat === "PNG"
    }

    onPageActiveChanged: {
        if (pageActive)
            preparePage()
        else
            persistExportQuality()
    }

    Component.onCompleted: applyRememberedQuality()

    EditorAdjustmentValueModel {
        id: sdrQualityModel
        objectName: "exportSdrQualityModel"
        fieldKey: "export_sdr_quality"
        label: qsTr("Quality")
        minimum: 1
        maximum: 100
        defaultValue: 95
        step: 1
        precision: 0
        suffix: "%"
        enabled: root.controlsEnabled
                 && (root.sdrFormat === "JPEG" || root.sdrFormat === "WEBP")
    }

    EditorAdjustmentValueModel {
        id: ultraHdrQualityModel
        objectName: "exportUltraHdrQualityModel"
        fieldKey: "export_uhdr_quality"
        label: qsTr("HDR Quality")
        minimum: 1
        maximum: 100
        defaultValue: 95
        step: 1
        precision: 0
        suffix: "%"
        enabled: root.controlsEnabled && root.hdrExportAvailable
    }

    EditorAdjustmentValueModel {
        id: pngLevelModel
        objectName: "exportPngLevelModel"
        fieldKey: "export_png_level"
        label: qsTr("PNG Level")
        minimum: 0
        maximum: 9
        defaultValue: 5
        value: 5
        step: 1
        precision: 0
        enabled: root.controlsEnabled && root.sdrFormat === "PNG"
    }

    FolderDialog {
        id: exportFolderDialog
        title: qsTr("Select Export Folder")
        onAccepted: root.outDirText = selectedFolder.toString()
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

                // ── Destination ──────────────────────────────────────
                CollapsibleSection {
                    id: destinationSection
                    Layout.fillWidth: true
                    Layout.leftMargin: appTheme.spaceSm
                    Layout.rightMargin: appTheme.spaceSm
                    Layout.topMargin: appTheme.spaceSm
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
                                onClicked: exportFolderDialog.open()
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
                            visible: root.sdrFormat === "JPEG" || root.sdrFormat === "WEBP"
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
                                text: qsTr("Limit longest edge (px)")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            TextField {
                                Layout.fillWidth: true
                                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                text: root.sdrMaxSideText
                                placeholderText: qsTr("No limit")
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
                    }
                }

                // ── HDR Export ───────────────────────────────────────
                CollapsibleSection {
                    id: hdrSection
                    Layout.fillWidth: true
                    Layout.leftMargin: appTheme.spaceSm
                    Layout.rightMargin: appTheme.spaceSm
                    title: qsTr("HDR Export")
                    expanded: root.hdrExportAvailable
                    controlsEnabled: root.controlsEnabled && root.hdrExportAvailable
                    bodyContentHeight: hdrBody.implicitHeight + appTheme.spaceSm

                    ColumnLayout {
                        id: hdrBody
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        spacing: appTheme.spaceMd

                        Label {
                            Layout.fillWidth: true
                            visible: !root.hdrExportAvailable
                            text: qsTr("Add Ultra HDR items to the queue to enable HDR export.")
                            wrapMode: Text.WordWrap
                            color: appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }

                        AdjustmentSlider {
                            objectName: "exportUltraHdrQualitySlider"
                            Layout.fillWidth: true
                            visible: root.hdrExportAvailable
                            model: ultraHdrQualityModel
                            flickable: settingsScroll
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceXs
                            visible: root.hdrExportAvailable
                            enabled: root.controlsEnabled && root.hdrExportAvailable

                            Label {
                                text: qsTr("Encoding longest edge (px)")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            TextField {
                                Layout.fillWidth: true
                                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                text: root.ultraHdrMaxSideText
                                placeholderText: "8192"
                                validator: IntValidator { bottom: 256; top: 8192 }
                                inputMethodHints: Qt.ImhDigitsOnly
                                font.family: appTheme.dataFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                color: appTheme.textColor
                                enabled: root.controlsEnabled && root.hdrExportAvailable
                                selectByMouse: true
                                onTextChanged: root.ultraHdrMaxSideText = text
                                onEditingFinished: root.normalizeUltraHdrMaxSide()
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
                            visible: root.hdrExportAvailable
                            text: qsTr("Dithering")
                            checked: root.ultraHdrDitherEnabled
                            enabled: root.controlsEnabled && root.hdrExportAvailable
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
                                    height: appTheme.iconButtonHitSize
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

                                        Image {
                                            anchors.centerIn: parent
                                            width: appTheme.iconSourceSizeCompact
                                            height: appTheme.iconSourceSizeCompact
                                            sourceSize.width: appTheme.iconSourceSizeCompact
                                            sourceSize.height: appTheme.iconSourceSizeCompact
                                            fillMode: Image.PreserveAspectFit
                                            source: "qrc:/panel_icons/image.svg"
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
                                                elide: Text.ElideRight
                                                color: appTheme.textColor
                                                font.family: appTheme.dataFontFamily
                                                font.pixelSize: appTheme.fontSizeCaption
                                            }

                                            Rectangle {
                                                visible: !summaryRow && modelData.isHdr === true
                                                Layout.preferredWidth: hdrTag.implicitWidth
                                                                       + appTheme.spaceSm
                                                Layout.preferredHeight: appTheme.spaceMd
                                                                       + appTheme.spaceXs
                                                radius: appTheme.badgeRadius
                                                color: root.withAlpha(appTheme.accentColor, 0.18)
                                                border.width: 1
                                                border.color: root.withAlpha(appTheme.accentColor, 0.55)

                                                Label {
                                                    id: hdrTag
                                                    anchors.centerIn: parent
                                                    text: qsTr("HDR")
                                                    color: appTheme.accentColor
                                                    font.family: appTheme.dataFontFamily
                                                    font.pixelSize: appTheme.fontSizeCaption
                                                    font.weight: appTheme.fontWeightStrong
                                                }
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
